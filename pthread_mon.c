/*******************************************************************************
 * Copyright (c) 2015 Genome Research Ltd. 
 * 
 * Author: Robert Davies <rmd+git@sanger.ac.uk> 
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the names Genome Research Ltd and Wellcome Trust Sanger Institute
 *    nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY GENOME RESEARCH LTD AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GENOME RESEARCH LTD OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************/

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>

#ifdef _POSIX_MONOTONIC_CLOCK
#  define CLOCK_ID CLOCK_MONOTONIC
#else
#  define CLOCK_ID CLOCK_REALTIME
#endif

#define OUTPUT_ENV_VAR "PTHREAD_MON_OUT"

static int (*real_pthread_create)(pthread_t *thread,
				  const pthread_attr_t *attr,
				  void *(*start_routine) (void *),
				  void *arg);
static int (*real_pthread_mutex_lock)(pthread_mutex_t *mutex);
static int (*real_pthread_mutex_trylock)(pthread_mutex_t *mutex);
static int (*real_pthread_mutex_unlock)(pthread_mutex_t *mutex);
static int (*real_pthread_cond_signal_1)(pthread_cond_t *cond);
static int (*real_pthread_cond_signal_2)(pthread_cond_t *cond);
static int (*real_pthread_cond_broadcast_1)(pthread_cond_t *cond);
static int (*real_pthread_cond_broadcast_2)(pthread_cond_t *cond);
static int (*real_pthread_cond_wait_1)(pthread_cond_t *cond,
				       pthread_mutex_t *mutex);
static int (*real_pthread_cond_wait_2)(pthread_cond_t *cond,
				       pthread_mutex_t *mutex);
static int (*real_pthread_cond_timedwait_1)(pthread_cond_t *cond,
					    pthread_mutex_t *mutex,
					    const struct timespec *abstime);
static int (*real_pthread_cond_timedwait_2)(pthread_cond_t *cond,
					    pthread_mutex_t *mutex,
					    const struct timespec *abstime);

static void init(void) __attribute__((constructor));
static void finish(void) __attribute__((destructor));

static struct timespec tzero = { 0, 0 };

typedef enum {
  Running    = 0,
  Finished   = 1,
  Cont_mutex = 0x10,
  Wait_mutex = 0x11,
  Cont_cond  = 0x20,
  Wait_cond  = 0x21,
  Sig_cond   = 0x30,
  Broad_cond = 0x40,
} EventType;

typedef struct {
  double     when;
  EventType  what;
  void      *item;
  void      *where;
} Event;

typedef struct {
  void *(*start_routine) (void *);
  void *arg;
  Event *events;
  size_t events_sz;
  size_t nevents;
  int    id;
} ThreadParams;

#define INIT_EVENTS_SZ 100000

static __thread ThreadParams *tp = NULL;
static int thread_counter = 0;
pthread_mutex_t tc_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread int in_myself = 0;
static char *output_file = NULL;
static FILE *output = NULL;

static int get_thread_id() {
  int id;
  real_pthread_mutex_lock(&tc_mutex);
  id = thread_counter++;
  real_pthread_mutex_unlock(&tc_mutex);
  return id;
}

#define INIT_SYM(F) do { \
    *(void**) (&real_##F) = dlsym(RTLD_NEXT, #F);	\
    assert(real_##F != NULL);				\
  } while (0)

#define INIT_SYM_VERS(F, N, V) do {				\
    *(void**) (&real_##F##_##N) = dlvsym(RTLD_NEXT, #F, V);	\
    assert(real_##F##_##N != NULL);					\
  } while (0)

static void init(void) {
  char *fname;
  INIT_SYM(pthread_create);
  INIT_SYM(pthread_mutex_lock);
  INIT_SYM(pthread_mutex_trylock);
  INIT_SYM(pthread_mutex_unlock);
  INIT_SYM_VERS(pthread_cond_signal, 1, "GLIBC_2.2.5");
  INIT_SYM_VERS(pthread_cond_signal, 2, "GLIBC_2.3.2");
  INIT_SYM_VERS(pthread_cond_broadcast, 1, "GLIBC_2.2.5");
  INIT_SYM_VERS(pthread_cond_broadcast, 2, "GLIBC_2.3.2");
  INIT_SYM_VERS(pthread_cond_wait, 1, "GLIBC_2.2.5");
  INIT_SYM_VERS(pthread_cond_wait, 2, "GLIBC_2.3.2");
  INIT_SYM_VERS(pthread_cond_timedwait, 1, "GLIBC_2.2.5");
  INIT_SYM_VERS(pthread_cond_timedwait, 2, "GLIBC_2.3.2");
  clock_gettime(CLOCK_ID, &tzero);
  tp = calloc(1, sizeof(ThreadParams));
  assert(NULL != tp);
  tp->events_sz = INIT_EVENTS_SZ;
  tp->events = malloc(tp->events_sz * sizeof(Event));
  assert(NULL != tp->events);
  tp->id = get_thread_id();

  fname = getenv(OUTPUT_ENV_VAR);
  if (NULL != fname) {
    char *pos = strstr(fname, "%p");
    if (NULL == pos) {
      output_file = strdup(fname);
      assert(NULL != output_file);
    } else {
      size_t sz = strlen(fname) + 16;
      output_file = malloc(sz);
      assert(NULL != output_file);
      snprintf(output_file, sz, "%.*s%d%s",
	       (int) (pos - fname), fname, (int) getpid(), pos + 2);
    }
  }
  if (NULL != output_file) {
    output = fopen(output_file, "w");
    if (NULL == output) {
      fprintf(stderr, "Warning: couldn't open %s for writing : %s",
	      output_file, strerror(errno));
    }
  }
}

static void handle_thread_done(void *arg) {
  ThreadParams *p = (ThreadParams *) arg;
  size_t i;
  FILE *out = NULL != output ? output : stderr;

  for (i = 0; i < p->nevents; i++) {
    fprintf(out, "%.9f %d %02x %p %p\n",
	    p->events[i].when, p->id,
	    (int) p->events[i].what, p->events[i].item, p->events[i].where);
  }

  free(p);
}

static void finish(void) {
  if (NULL != tp) handle_thread_done(tp);

  if (NULL != output) {
    int res = fclose(output);
    if (0 != res) {
      fprintf(stderr, "Warning: Error writing to %s : %s\n",
	      output_file, strerror(errno));
    }
  }
}

static void record_event(ThreadParams *p, EventType what,
			 void *item, void *where) {
  struct timespec now = { 0, 0 };
  int save_errno = errno;

  if (p->nevents == p->events_sz) {
    size_t new_sz = p->events_sz * 2;
    Event *new_events = realloc(p->events, new_sz * sizeof(Event));
    
    if (NULL == new_events) {
      errno = save_errno;
      return;
    }
    p->events_sz = new_sz;
    p->events = new_events;
  }

  clock_gettime(CLOCK_ID, &now);
  now.tv_sec  -= tzero.tv_sec;
  now.tv_nsec -= tzero.tv_nsec;
  if (now.tv_nsec < 0) {
    --now.tv_sec;
    now.tv_nsec += 1000000000;
  }

  p->events[p->nevents].when  = ((double) now.tv_sec
				 + (double) now.tv_nsec / 1.0e9);
  p->events[p->nevents].what  = what;
  p->events[p->nevents].item  = item;
  p->events[p->nevents].where = where;
  p->nevents++;
  errno = save_errno;
}

static void * wrap_start_routine(void *arg) {
  ThreadParams *p = (ThreadParams *) arg;
  tp = p;

  pthread_cleanup_push(handle_thread_done, p);
  p->start_routine(p->arg);
  pthread_cleanup_pop(1);
}

#define WHERE __builtin_extract_return_addr(__builtin_return_address(0))

int pthread_create(pthread_t *thread,
		   const pthread_attr_t *attr,
		   void *(*start_routine) (void *),
		   void *arg) {
  int res;
  ThreadParams *p = malloc(sizeof(ThreadParams));
  if (NULL == p) {
    errno = EAGAIN;
    return errno;
  }

  p->start_routine = start_routine;
  p->arg = arg;
  p->events_sz = INIT_EVENTS_SZ;
  p->nevents   = 0;
  p->events = malloc(p->events_sz * sizeof(Event));
  if (NULL == p->events) {
    free(p);
    errno = EAGAIN;
    return errno;
  }
  p->id = get_thread_id();

  res = real_pthread_create(thread, attr, wrap_start_routine, p);
  if (0 != res) {
    free(p->events);
    free(p);
  }
  return res;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
  int res;
  int save_errno;

  if (in_myself) return real_pthread_mutex_lock(mutex);
  in_myself = -1;
#if 1
  save_errno = errno;
  res = real_pthread_mutex_trylock(mutex);
  if (0 == res || EBUSY != res) {
    in_myself = 0;
    return res;
  }
  errno = save_errno;
#endif
  record_event(tp, Wait_mutex, mutex, WHERE);
  res = real_pthread_mutex_lock(mutex);
  record_event(tp, Cont_mutex, mutex, WHERE);
  in_myself = 0;
  return res;
}

#define PTH_COND_SIGNAL(N, V)						\
  int pthread_cond_signal_##N(pthread_cond_t *cond) {			\
    int res;								\
    if (in_myself) return real_pthread_cond_signal_##N(cond);		\
    in_myself = -1;							\
    record_event(tp, Sig_cond, cond, WHERE);				\
    res = real_pthread_cond_signal_##N(cond);				\
    in_myself = 0;							\
    return res;								\
  }									\
  __asm__(".symver pthread_cond_signal_"#N", pthread_cond_signal@"V);

PTH_COND_SIGNAL(1, "GLIBC_2.2.5")
PTH_COND_SIGNAL(2, "@GLIBC_2.3.2")

#define PTH_COND_BCAST(N, V)						\
  int pthread_cond_broadcast_##N(pthread_cond_t *cond) {		\
    int res;								\
    if (in_myself) return real_pthread_cond_broadcast_##N(cond);	\
    in_myself = -1;							\
    record_event(tp, Broad_cond, cond, WHERE);				\
    res = real_pthread_cond_broadcast_##N(cond);			\
    in_myself = 0;							\
    return res;								\
  }									\
  __asm__(".symver pthread_cond_broadcast_"#N", pthread_cond_broadcast@"V);

PTH_COND_BCAST(1, "GLIBC_2.2.5")
PTH_COND_BCAST(2, "@GLIBC_2.3.2")

#define PTH_COND_WAIT(N, V)						\
  int pthread_cond_wait_##N(pthread_cond_t *cond,			\
			    pthread_mutex_t *mutex) {			\
    int res;								\
    if (in_myself) return real_pthread_cond_wait_##N(cond, mutex);	\
    in_myself = -1;							\
    record_event(tp, Wait_cond, cond, WHERE);				\
    res = real_pthread_cond_wait_##N(cond, mutex);			\
    record_event(tp, Cont_cond, cond, WHERE);				\
    in_myself = 0;							\
    return res;								\
  }									\
  __asm__(".symver pthread_cond_wait_"#N", pthread_cond_wait@"V);

PTH_COND_WAIT(1, "GLIBC_2.2.5")
PTH_COND_WAIT(2, "@GLIBC_2.3.2")

#define PTH_COND_TIMEDWAIT(N, V)					\
  int pthread_cond_timedwait_##N(pthread_cond_t *cond,			\
				 pthread_mutex_t *mutex,		\
				 const struct timespec *abstime) {	\
    int res;								\
    if (in_myself) {							\
      return real_pthread_cond_timedwait_##N(cond, mutex, abstime);	\
    }									\
    in_myself = -1;							\
    record_event(tp, Wait_cond, cond, WHERE);				\
    res = real_pthread_cond_timedwait_##N(cond, mutex, abstime);	\
    record_event(tp, Cont_cond, cond, WHERE);				\
    in_myself = 0;							\
    return res;								\
  }									\
  __asm__(".symver pthread_cond_timedwait_"#N", pthread_cond_timedwait@"V);

PTH_COND_TIMEDWAIT(1, "GLIBC_2.2.5")
PTH_COND_TIMEDWAIT(2, "@GLIBC_2.3.2")

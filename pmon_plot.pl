#!/usr/bin/perl
# Copyright (c) 2015 Genome Research Ltd. 
# 
# Author: Robert Davies <rmd+git@sanger.ac.uk> 
# 
# Redistribution and use in source and binary forms, with or without 
# modification, are permitted provided that the following conditions are met: 
# 
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
# 3. Neither the names Genome Research Ltd and Wellcome Trust Sanger Institute
#    nor the names of its contributors may be used to endorse or promote
#    products derived from this software without specific prior written
#    permission.
# 
# THIS SOFTWARE IS PROVIDED BY GENOME RESEARCH LTD AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL GENOME RESEARCH LTD OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

use strict;
use warnings;
use Getopt::Long;

my $start = 0;
my $end;
my $mutex  = 1;
my $signal = 1;
my $cond   = 1;
my $wait   = 0;
my $run    = 1;
my $out    = "";

my $usage = "Usage: $0 -start <time> -end <time> -out <data_file> <input>\n";

GetOptions('start=s' => \$start, 'end=s' => \$end, 'stop=s' => \$end,
	   'mutex!' => \$mutex, 'signal!' => \$signal, 'cond!' => \$cond,
	   'wait!' => \$wait, 'run!' => \$run, 'out=s' => \$out)
    || die $usage;

unless ($out) {
    die "Error: -out option is needed.\n$usage\n";
}

my %threads;
while (<>) {
    my ($time, $thread, $status) = split();
    if (defined($end) && $time < $end) {
	push(@{$threads{$thread}}, [$time, hex($status)]);
    }
}

open(my $data_out, '>', $out) || die "Couldn't open $out : $!\n";

my @counts = (0) x 5;

foreach my $thread (sort { $a <=> $b } keys %threads) {
    my $events = $threads{$thread};
    my $stopped = 0;
    my $last_restart = 0;
    my $last_stop = 0;
    my $count = 0;
    foreach my $evt (@$events) {
	if ($evt->[0] > $end) { last; }
	my $status = $evt->[1] & 1;
	my $etype  = ($evt->[1] & 0xf0) / 16;
	my $is_signal = $etype >= 3;
	next if (!$mutex && $etype == 1);
	next if (!$cond  && $etype == 2);
	if (!$is_signal && $status != $stopped) {
	    if ($status) {
		if ($run && $evt->[0] >= $start) {
		    printf($data_out "%.9f %f 0\n%.9f %f 0\n\n",
			   $last_restart > $start ? $last_restart : $start,
			   $thread + $count / 20,
			   $evt->[0], $thread + $count / 20);
		    $counts[0]++;
		}
		$last_stop = $evt->[0];
	    } else {
		if ($wait && $evt->[0] >= $start) {
		    my $offset = $etype == 1 ? 0.1 : 0.2;
		    printf($data_out "%.9f %f %d\n%.9f %f %d\n\n",
			   $last_stop > $start ? $last_stop : $start,
			   $thread + $offset, $etype,
			   $evt->[0], $thread + $offset, $etype);
		    $counts[$etype]++;
		}
		$last_restart = $evt->[0];
	    }
	    $stopped = $status;
	}
	if ($evt->[0] >= $start && $signal && $is_signal) {
	    printf($data_out "%.9f %f %d\n%.9f %f %d\n\n",
		   $evt->[0], $thread + 0.3, $etype,
		   $evt->[0], $thread + 0.35, $etype);
	    $counts[$etype]++;
	}
    }
    if (!$stopped && $last_restart < $end) {
	printf($data_out "%.9f %f 0\n%.9f %f 0\n\n",
	       $last_restart > $start ? $last_restart : $start,
	       $thread + $count / 20,
	       $end, $thread + $count / 20);
	$counts[0]++;
    }
}

close($data_out) || die "Error writing to $out : $_\n";

my @line_types = (1, 9, 5, 3, 7);
my @titles = ('Running', 'Mutex', 'Conditional', 'Signal', 'Broadcast');
my $plot_file = qq[plot "$out"];
for (my $i = 0; $i < @line_types; $i++) {
    next unless ($counts[$i]);
    printf('%s using 1:($3 == %d ? $2 : 1/0) w l lt %d lw 2 title "%s"',
	   $plot_file, $i, $line_types[$i], $titles[$i]);
    $plot_file = ', ""';
}
print "\n";
exit;

=head1 NAME

pmon_plot.pl - Convert pthread_mon data to gnuplot data files

=head1 SYNOPSIS

pmon_plot.pl -start <time> -stop <time> -out <data_file> <input_file>

=head1 DESCRIPTION

A simple script to convert the output from pthread_mon.so into something
that can be drawn using gnuplot.  If given too much data, gnuplot can
become slow and may run into problems with loss of significant figures.
This script can be used to select a small region of the data to draw.

The script writes the plot data to the location given in the -out
parameter.  It also writes a gnuplot script to view the data to STDOUT.

=head1 OPTIONS

=over 4

=item -out <data_file>

The location of the data file for gnuplot.  This option must be present.

=item -start <time>

Time-point in the input file to start plotting from.

=item -stop <time>

Time-point where the plot should end.

=item -end <time>

Synonym for -stop.

=item -[no]mutex

If -nomutex is used, periods where threads wait for mutexes are ignored
and appear as if the thread is running.  Default is -mutex.

=item -[no]cond

If -nocond is used, periods where threads wait for condes are ignored
and appear as if the thread is running.  Default is -cond.

=item -[no]signal

Switch showing cond_signal and cond_broadcast events on or off.  Default is
-signal.

=item -[no]wait

Switch drawing lines representing times when the thread is waiting on or
off.  Default it -nowait.

=item -[no]run

Switch drawing lines representing times when the thread is not waiting on or
off.  Default is -run.

=back

=head1 AUTHOR

Rob Davies.

=cut

#!/usr/bin/perl

use strict;
use warnings;
use Data::Dumper;

open FILE, $ARGV[0];
my @lines = <FILE>;
close FILE;

my %lock;
my $line_no = 0;

my $vmlinux = "/lib/modules/`uname -r`/build/vmlinux";

for my $l (@lines) {
   $line_no++;

   if($l =~ m/TID\s+(\d+).*Acquired (\w+) lock ([a-fA-F0-9]+) \(caller ([a-fA-F0-9]+)/) {
      $lock{$3}->{ntimes}++;
      $lock{$3}->{tid}->{$1}++;
      if($lock{$3}->{tid}->{$1} > 1) {
         my $fun = `addr2line -e $vmlinux $lock{$3}->{last_owner_fun}`;
         chop($fun);
         print "[Line $line_no] WARNING: $1 has tried to acquire lock $3 $lock{$3}->{tid}->{$1} times (last owner = $fun at line $lock{$3}->{last_owner_at_line})\n";
      }
      $lock{$3}->{last_owner_fun}=$4;
      $lock{$3}->{last_owner_at_line}=$line_no;

      my @w_tmp = @{$lock{$3}->{waiting}};
      @{$lock{$3}->{waiting}} = ();
      for my $w (@w_tmp) {
         if($w->[0] != $1) {
            push @{$lock{$3}->{waiting}}, $w;
         }
      }

   }
   elsif($l =~ m/TID\s+(\d+).*Acquiring (\w+) lock ([a-fA-F0-9]+) \(caller ([a-fA-F0-9]+)/) {
      my @to_i = ($1, $2, $4, $line_no);
      push @{$lock{$3}->{waiting}}, \@to_i;
   }
   elsif($l =~ m/TID\s+(\d+).*Released (\w+) lock (\w+)/) {
      $lock{$3}->{ntimes}--;
      $lock{$3}->{tid}->{$1}--;
      if($lock{$3}->{tid}->{$1} < 0) {
         if(defined $lock{$3}->{last_owner_fun}) {
            my $fun = `addr2line -e $vmlinux $lock{$3}->{last_owner_fun}`;
            chop($fun);
            print "[Line $line_no] WARNING: lock $3 has been released to many times (last owner = $fun at line $lock{$3}->{last_owner_at_line})\n";
         }
      }
   }
}

for my $l (keys %lock) {
   if(defined $lock{$l}->{ntimes}) {
      if($lock{$l}->{ntimes} > 0) {
         print "Lock $l has not been released\n";
         for my $o (keys %{$lock{$l}->{tid}}) {
            next if(!$lock{$l}->{tid}->{$o});
            my $fun = `addr2line -e $vmlinux $lock{$l}->{last_owner_fun}`;
            chop($fun);
            print "\tOwner is tid $o ($lock{$l}->{tid}->{$o} times, acquired for the last time by $fun, line $lock{$l}->{last_owner_at_line})\n" if(defined $lock{$l}->{tid}->{$o});
         }
      }
      elsif($lock{$l}->{ntimes} < 0) {
         print "!!! Lock $l has been released to many times !\n";
      }
   }

   elsif(scalar(@{$lock{$l}->{waiting}}) > 0) {
      my @w_tmp = @{$lock{$l}->{waiting}};
      print "Possible deadlock detected on lock $l\n";
      for my $w (@w_tmp) {
         my $fun = `addr2line -e $vmlinux $w->[2]`;
         chop($fun);
         print "\tThread $w->[0] is waiting at $w->[2] at line $w->[3]\n";
      }
   }
}

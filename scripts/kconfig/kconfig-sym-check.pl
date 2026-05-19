#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0

use warnings;
use strict;

my $srctree = shift @ARGV;
my $kconfig_sym_check_excludes = defined $ARGV[0] ? $ARGV[0] : undef;

my @files = `git -C \Q$srctree\E ls-files '*Kconfig*' 2>/dev/null`;
if (@files) {
	chomp @files;
	@files = map { "$srctree/$_" } @files;
} else {
	@files = `find \Q$srctree\E -name '*Kconfig*'`;
	chomp @files;
}

my %configs = ();
my %refs = ();

foreach my $file (@files) {
	open F, $file or die "Cannot open $file: $!";

	my $help = 0;
	my $help_level;
	my $level;

	while (<F>) {
		chomp;

		next if /^\s*$/;
		next if /^\s*#/;

		/^(\s*)/;
		$level = length $1;

		if ($help && $level < $help_level) {
			$help = 0;
		}

		next if ($help);

		if (/^\s*(help|\-\-\-help\-\-\-)$/) {
			$help = 1;
			$_ = <F>;
			/^(\s*)/;
			$help_level = length $1;
			next;
		}

		if (/^\s*(config|menuconfig)\s+([a-zA-Z0-9_]+)\s*(#.*)?$/) {
			$configs{$2}++;
			next;
		}

		if (/^\s*(default|def_bool|def_tristate|select|depends\s+on|imply|visible\s+if|range|if)\s+(.+)\s*$/) {
			my $s = $2;
			$s =~ s/"[^"]*"//g;
			$s =~ s/'[^']*'//g;
			$s =~ s/#.*//;
			$s =~ s/\$\([^)]*\)//g;
			$s =~ s/%%[^%]*%%//g;
			my @syms = split /[^a-zA-Z0-9_]+/, $s;
			map {
				$refs{$_}++ if (/[a-zA-Z]/ && $_ ne "if" && $_ ne "y" && $_ ne "n" && $_ ne "m" && !(/^0[xX]/ && !/[g-wy-zG-WY-Z]/));
			} @syms
		}
	}

	close F;
}

my %known_syms = ();
if (defined $kconfig_sym_check_excludes) {
	my $file = $kconfig_sym_check_excludes;
	open F, $file or die "Cannot open $file: $!";
	while (<F>) {
		chomp;
		next if /^\s*$/;
		next if /^\s*#/;
		$known_syms{$1}++ if (/^\s*([a-zA-Z0-9_]+)\s*(#.*)?$/);
	}
}

my $ret = 0;
foreach my $k (sort keys %refs) {
	next if (exists $configs{$k} || exists $known_syms{$k});

	print "$k";
	print " - warning: '$k' is probably not what you want; Kconfig tristate literals are always lowercase ('n', 'y', 'm')" if ($k eq "N" || $k eq "Y" || $k eq "M");
	print "\n";

	$ret = 1;
}

exit $ret;

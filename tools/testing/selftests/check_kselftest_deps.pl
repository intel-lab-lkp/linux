#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0
use warnings;
use strict;
use Getopt::Long;
use File::Spec;
use File::Basename;

# set the environment variable SILENCE to silence
# debug output.

# Check if required arguments are provided
die "Usage: $0 <selftest_path> <compiler>\n" unless @ARGV >= 2;

my $test_path = $ARGV[0];
my $cc = join(' ', @ARGV[1..$#ARGV]);
my $script_dir = dirname(__FILE__);

my $silenceprint;
$silenceprint = 1 if (defined($ENV{SILENCE}));

sub dprint {
	return if ($silenceprint);
	print STDERR @_;
}

my $uname = `uname -r`;
chomp $uname;

my @searchconfigs = (
	{
		"file" => ".config",
		"exec" => "cat",
	},
	{
		"file" => "/proc/config.gz",
		"exec" => "zcat",
	},
	{
		"file" => "/boot/config-$uname",
		"exec" => "cat",
	},
	{
		"file" => "/boot/vmlinuz-$uname",
		"exec" => "scripts/extract-ikconfig",
		"test" => "scripts/extract-ikconfig",
	},
	{
		"file" => "vmlinux",
		"exec" => "scripts/extract-ikconfig",
		"test" => "scripts/extract-ikconfig",
	},
	{
		"file" => "/lib/modules/$uname/kernel/kernel/configs.ko",
		"exec" => "scripts/extract-ikconfig",
		"test" => "scripts/extract-ikconfig",
	},
	{
		"file" => "/lib/modules/$uname/build/.config",
		"exec" => "cat",
	},
	{
		"file" => "kernel/configs.ko",
		"exec" => "scripts/extract-ikconfig",
		"test" => "scripts/extract-ikconfig",
	},
	{
		"file" => "kernel/configs.o",
		"exec" => "scripts/extract-ikconfig",
		"test" => "scripts/extract-ikconfig",
	},
);

sub read_config {
	foreach my $conf (@searchconfigs) {
		my $file = $conf->{"file"};

		next unless -f $file;

	if (defined $conf->{"test"}) {
		`$conf->{"test"} $file 2>/dev/null`;
		next if $?;
	}

	my $exec = $conf->{"exec"};

	# dprint "Kernel config: '$file'\n";

	open(my $infile, '-|', "$exec $file") or die "Failed to run $exec $file";
	my @config_content = <$infile>;
	close $infile;

	return @config_content;
	}

	dprint "Unable to find kernel config file, skipping check\n";
	exit 0;
}

sub check_libs {
	my $command = "cd $script_dir && ./kselftest_deps.sh \"$cc\" $test_path";
	my $lib_test = `$command 2>&1`;
	my $exit_code = $? >> 8;

	if ($exit_code != 0) {
		die "Error: Failed to run kselftest_deps.sh with exit code $exit_code\n";
	}

	return $lib_test;
}

# Check for missing libraries
my $lib_test = check_libs();
my $fail_libs;

if ($lib_test =~
/(--------------------------------------------------------\s
*Missing libraries system.*?
--------------------------------------------------------)/s) {
	$fail_libs = $1;
}

dprint("$fail_libs\n") if $fail_libs;

# Read and parse kernel config
my @config_file = read_config();
my %kern_configs;
foreach my $line (@config_file) {
	chomp $line;
	next if $line =~ /^\s*$/ || $line =~ /^#/;

	if ($line =~ /^(CONFIG_\w+)=(.+)$/) {
	$kern_configs{$1} = $2;
	}
}

# Read and parse test config
my %test_configs;
open(my $fh, '<', "$test_path/config") or exit 0;

while (my $line = <$fh>) {
	chomp $line;
	next if $line =~ /^\s*$/ || $line =~ /^#/;

	if ($line =~ /^(CONFIG_\w+)=(.+)$/) {
		$test_configs{$1} = $2;
	}
}
close $fh;

# Compare selftest configs with kernel configs
my $all_match = 1;
my @missing_or_mismatched;

foreach my $key (keys %test_configs) {
	if (!exists $kern_configs{$key} || $kern_configs{$key} ne $test_configs{$key}) {
		push @missing_or_mismatched, "Required: $key=$test_configs{$key}";
		$all_match = 0;
	}
}

if ($all_match && !$fail_libs) {
	exit 0;
} else {
	dprint("--------------------------------------------------------\n") unless $fail_libs;
	dprint("$_\n") for @missing_or_mismatched;
	dprint("--------------------------------------------------------\n") if @missing_or_mismatched;

	exit 1;
}

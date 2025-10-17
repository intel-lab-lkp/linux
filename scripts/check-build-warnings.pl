#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0
#
# check-build-warnings.pl - Compare kernel build warnings against a baseline
#
# This script builds the kernel and records warnings or checks warnings
# against previously recorded warnings. The idea here is to capture
# guidance in Documentation/process/maintainer-netdev.rst in a script
# wrt/ not making new errors and to provide output simple to include on
# patches.
#
# Usage:
#	scripts/check-build-warnings.pl [options] [make-target]
#
# Examples:
#	# Save baseline
#	scripts/check-build-warnings.pl --save-baseline
#
#	# Check for new warnings after making changes
#	scripts/check-build-warnings.pl --check
#
#	# Build with W=1 warnings enabled
#	scripts/check-build-warnings.pl --check --warn=1
#
#	# Just show current warnings without comparison
#	scripts/check-build-warnings.pl --list

use strict;
use warnings;
use Getopt::Long qw(:config no_ignore_case);
use File::Basename;
use Cwd qw(abs_path);

my $scriptname = basename($0);
my $objtree = $ENV{'O'} || $ENV{'KBUILD_OUTPUT'} || '.';
my $srctree = abs_path(dirname($0) . "/..");

# Default options
my $help = 0;
my $save_baseline = 0;
my $check_warnings = 0;
my $list_only = 0;
my $warn_level = "";
my $jobs = "";
my $no_clean = 0;
my $build_target = "all";

sub show_usage {
	print << "EOF";
Usage: $scriptname [options] [make-target]

Compare kernel build warnings against a baseline to detect new warnings.
Baselines are automatically created per target and warning level.

Options:
    -s, --save-baseline     Build and save current warnings as baseline
    -c, --check             Build and compare against baseline, error if new warnings
    -l, --list              Build and list all warnings without comparison
    -w, --warn=LEVEL        Set warning level (1, 2, 3, 12, 123, etc.)
    -j, --jobs=N            Number of parallel jobs (passed to make -j)
    -n, --no-clean          Skip cleaning before build (faster but may miss warnings)
    -h, --help              Show this help message

Examples:
    # Initial setup - save baseline for default target
    $scriptname --save-baseline

    # After making changes - check for new warnings
    $scriptname --check

    # Use stricter warning levels (creates separate baseline)
    $scriptname --save-baseline --warn=1
    $scriptname --check --warn=1

    # Build specific target with warning check
    $scriptname --check drivers/dma/

Exit codes:
    0 - Success (no new warnings)
    1 - New warnings found (when using --check)
    2 - Build failed
    3 - Missing baseline (when using --check)

Note: By default, the script cleans the build target before building to ensure
      all warnings are captured (cached object files won't emit warnings). This
      can be skipped with --no-clean for faster builds, but may miss warnings
      from unchanged files. CONFIG_WERROR is automatically disabled to capture
      warnings without failing the build.
EOF
}

# Parse command line options
GetOptions(
	'save-baseline|s' => \$save_baseline,
	'check|c' => \$check_warnings,
	'list|l' => \$list_only,
	'warn|w=s' => \$warn_level,
	'jobs|j=s' => \$jobs,
	'no-clean|n' => \$no_clean,
	'help|h' => \$help,
) or die("Error in command line arguments\n");

if ($help) {
	show_usage();
	exit(0);
}

# Handle build target
if (@ARGV) {
	$build_target = join(' ', @ARGV);
}

# Add warning flags
my $warn_flags = "";
if ($warn_level) {
	$warn_flags .= "W=$warn_level";
}

# Generate baseline filename based on target and flags (kernel-style)
# Store warnings alongside the build artifacts, like .cmd files
sub get_baseline_filename {
	my ($target, $flags) = @_;

	# Split target into directory and basename
	my $dir = dirname($target);
	my $base = basename($target);

	# Add warning level suffix if present
	my $suffix = "";
	if ($flags) {
		$suffix = ".$flags";
		$suffix =~ s/=//g;  # W=1 becomes .W1
	}

	# For targets in root directory (all, vmlinux, bzImage, etc.)
	if ($dir eq ".") {
		return "$objtree/.$base$suffix.warnings";
	}

	# For targets in subdirectories, store alongside like .cmd files
	return "$objtree/$dir/.$base$suffix.warnings";
}

my $baseline_file = get_baseline_filename($build_target, $warn_flags);

# Construct make command
my $make_cmd = "make";
$make_cmd .= " -j$jobs" if $jobs;

# Disable CONFIG_WERROR to allow warnings without failing the build
$make_cmd .= " KCFLAGS=-Wno-error";

if ($warn_flags) {
	$make_cmd .= " $warn_flags";
}

$make_cmd .= " $build_target";

# Function to extract and normalize warnings from build output
sub extract_warnings {
	my ($output) = @_;
	my @warnings;
	my %seen;

	foreach my $line (split(/\n/, $output)) {
		# Match common warning/error patterns with line numbers
		# Examples:
		#   path/to/file.c:123:45: warning: something
		#   path/to/file.c:123: warning: something
		#   WARNING: at the start of line (modpost, etc.)
		#   path/to/file.c:123:45: error: something (compilation errors)
		my $is_warning = 0;

		# Format: file.c:123:45: warning: or file.c:123: warning:
		if ($line =~ /:\d+:(?:\d+:)?\s*warning:/) {
			$is_warning = 1;
		}
		# Format: WARNING: at start of line (kernel build system warnings)
		elsif ($line =~ /^WARNING:/) {
			$is_warning = 1;
		}
		# Format: file.c:123:45: error: (compilation errors)
		elsif ($line =~ /:\d+:(?:\d+:)?\s*error:/) {
			$is_warning = 1;
		}

		next unless $is_warning;

		# Normalize the warning message
		my $normalized = $line;
		$normalized =~ s/^\s+|\s+$//g;  # Trim whitespace

		# Skip duplicate warnings
		next if exists $seen{$normalized};
		$seen{$normalized} = 1;

		push @warnings, $normalized;
	}

	return @warnings;
}

# Function to run build and capture warnings
sub build_and_capture {
	# Clean the target first unless --no-clean is specified
	unless ($no_clean) {
		print "Cleaning build artifacts...\n";
		`make clean 2>&1`;
	}

	print "Building kernel with: $make_cmd\n";

	my $output = "";
	my $exit_code;

	# Capture output without displaying
	$output = `$make_cmd 2>&1`;
	$exit_code = $? >> 8;

	return ($output, $exit_code);
}

# Function to save warnings to file with metadata
sub save_warnings {
	my ($file, $target, $wflags, @warnings) = @_;

	open(my $fh, '>', $file) or die "Cannot write to $file: $!\n";

	# Write metadata as comments
	print $fh "# BUILD_TARGET=$target\n";
	print $fh "# WARN_FLAGS=$wflags\n";
	print $fh "#\n";

	foreach my $warning (sort @warnings) {
		print $fh "$warning\n";
	}
	close($fh);
}

# Function to load warnings from file and extract metadata
sub load_warnings {
	my ($file) = @_;
	my @warnings;
	my %metadata;

	return (\@warnings, \%metadata) unless -f $file;

	open(my $fh, '<', $file) or die "Cannot read from $file: $!\n";
	while (my $line = <$fh>) {
		chomp($line);

		# Parse metadata comments
		if ($line =~ /^# BUILD_TARGET=(.*)$/) {
			$metadata{target} = $1;
		} elsif ($line =~ /^# WARN_FLAGS=(.*)$/) {
			$metadata{warn_flags} = $1;
		} elsif ($line =~ /^#/) {
			# Skip other comments
			next;
		} elsif ($line) {
			# Regular warning line
			push @warnings, $line;
		}
	}
	close($fh);

	return (\@warnings, \%metadata);
}

# Function to compare warnings
sub compare_warnings {
	my ($baseline_ref, $current_ref) = @_;
	my %baseline = map { $_ => 1 } @$baseline_ref;
	my @new_warnings;
	my @fixed_warnings;

	# Find new warnings
	foreach my $warning (@$current_ref) {
		push @new_warnings, $warning unless exists $baseline{$warning};
	}

	# Find fixed warnings
	my %current = map { $_ => 1 } @$current_ref;
	foreach my $warning (@$baseline_ref) {
		push @fixed_warnings, $warning unless exists $current{$warning};
	}

	return (\@new_warnings, \@fixed_warnings);
}

# Main logic
if ($save_baseline) {
	print "Building and saving baseline...\n";
	my ($output, $exit_code) = build_and_capture();

	if ($exit_code != 0) {
		print STDERR "Build failed with exit code $exit_code\n";
		exit(2);
	}

	my @warnings = extract_warnings($output);
	save_warnings($baseline_file, $build_target, $warn_flags, @warnings);

	printf("Baseline saved to %s (%d warnings)\n", $baseline_file, scalar(@warnings));
	printf("  Target: %s\n", $build_target);
	printf("  Flags:  %s\n", $warn_flags || "none");
	exit(0);
}
elsif ($check_warnings) {
	unless (-f $baseline_file) {
		print STDERR "Error: Baseline file not found: $baseline_file\n";
		print STDERR "Run with --save-baseline first to create a baseline\n";
		exit(3);
	}

	print "Building and checking for new warnings...\n";
	my ($output, $exit_code) = build_and_capture();

	if ($exit_code != 0) {
		print STDERR "Build failed with exit code $exit_code\n";
		exit(2);
	}

	my ($baseline_ref, $baseline_meta) = load_warnings($baseline_file);
	my @current = extract_warnings($output);

	my ($new_warnings_ref, $fixed_warnings_ref) = compare_warnings($baseline_ref, \@current);

	printf("\nWarning summary:\n");
	printf("  Baseline warnings: %d\n", scalar(@$baseline_ref));
	printf("  Current warnings:  %d\n", scalar(@current));
	printf("  New warnings:      %d\n", scalar(@$new_warnings_ref));
	printf("  Fixed warnings:    %d\n", scalar(@$fixed_warnings_ref));

	if (@$fixed_warnings_ref) {
		print "\nFixed warnings:\n";
		foreach my $warning (@$fixed_warnings_ref) {
			print "  [-] $warning\n";
		}
	}

	if (@$new_warnings_ref) {
		print "\nNew warnings introduced:\n";
		foreach my $warning (@$new_warnings_ref) {
			print "  [+] $warning\n";
		}
		print "\nERROR: New warnings detected!\n";
		exit(1);
	}

	print "\nSUCCESS: No new warnings introduced\n";
	exit(0);
}
elsif ($list_only) {
	print "Building and listing warnings...\n";
	my ($output, $exit_code) = build_and_capture();

	if ($exit_code != 0) {
		print STDERR "Build failed with exit code $exit_code\n";
		exit(2);
	}

	my @warnings = extract_warnings($output);

	printf("\nTotal warnings: %d\n\n", scalar(@warnings));
	foreach my $warning (@warnings) {
		print "$warning\n";
	}

	exit(0);
}
else {
	print STDERR "Error: Must specify one of --save-baseline, --check, or --list\n\n";
	show_usage();
	exit(1);
}

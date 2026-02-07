#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0
#
# (c) 2026, Jason Hall <jason.kei.hall@gmail.com>

use strict;
use warnings;

sub process_rust {
    my ($line, $rawline, $herecurr) = @_;

    # Check for Rust unwrap/expect usage.
    # We skip lines that are already comments, assert macros (common in tests),
    # or have a '// PANIC:' justification.
    if ($line =~ /^\+/) {
        if ($line =~ /(?:\.|::)(?:unwrap|expect)\s*\(/ &&
            $rawline !~ /\/\/\s*PANIC:/ &&
            $line !~ /^\+\s*\/\// &&
            $line !~ /^\+\s*assert/) {
            return ("RUST_UNWRAP",
                    "unwrap() and expect() should generally be avoided in Rust kernel code.\n" .
                    "If the use is intended, please justify it with a '// PANIC:' comment.\n" .
                    "See: https://rust.docs.kernel.org/kernel/error/type.Result.html#error-codes-in-c-and-rust\n" .
                    $herecurr);
        }
    }
    return ();
}

1;

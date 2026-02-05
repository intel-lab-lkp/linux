#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0
#
# (c) 2026, Jason K. Hall <jason.kei.hall@gmail.com>

use strict;
use warnings;

sub process_rust {
    my ($line, $rawline, $herecurr) = @_;

    # check for Rust unwrap/expect
    if ($line =~ /^\+/) {
        if ($line =~ /(?:\.|::)(?:unwrap|expect)\s*\(/ &&
            $rawline !~ /\/\/\s*PANIC:/ &&
            $line !~ /^\+\s*\/\// &&
            $line !~ /^\+\s*assert/) {
            return ("RUST_UNWRAP",
                    "Avoid unwrap() or expect() in Rust code; use proper error handling (Result) or justify with a '// PANIC: ...' comment.\n" . $herecurr);
        }
    }
    return ();
}

1;
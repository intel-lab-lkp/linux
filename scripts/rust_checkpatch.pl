#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0
#
# (c) 2026, Jason Hall <jason.kei.hall@gmail.com>

use strict;
use warnings;

sub process_rust {
    my ($line, $rawline, $herecurr) = @_;

    # Reserve for future Rust-specific lints
    return ();
}

1;

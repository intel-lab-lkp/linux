# SPDX-License-Identifier: GPL-2.0
"""
Test detection of dead defaults (different defaults that can never be active).
"""

def test(conf):
    assert conf.olddefconfig() == 0
    assert conf.stderr_contains('expected_stderr')

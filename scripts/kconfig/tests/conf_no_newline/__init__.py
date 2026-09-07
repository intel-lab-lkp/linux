# SPDX-License-Identifier: GPL-2.0
"""
Preserve complete string, int and hex answers at end of input.
"""

import subprocess

import pytest

from conftest import CONF_PATH


@pytest.mark.parametrize('mode', ['--oldaskconfig', '--oldconfig'])
@pytest.mark.parametrize('newline', ['', '\n'], ids=['eof', 'newline'])
@pytest.mark.parametrize('symbol_type, value, expected', [
    ('string', 'abcdef', '"abcdef"'),
    ('int', '42', '42'),
    ('hex', '0xff', '0xff'),
])
def test(mode, newline, symbol_type, value, expected, tmp_path, monkeypatch):
    (tmp_path / 'Kconfig').write_text(
        'config TEST\n\t{} "Test value"\n'.format(symbol_type))
    monkeypatch.setenv('srctree', str(tmp_path))
    monkeypatch.setenv('KCONFIG_DEFCONFIG_LIST', '')

    result = subprocess.run([CONF_PATH, mode, 'Kconfig'],
                            input=value + newline, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            cwd=tmp_path, timeout=10)

    assert result.returncode == 0, result.stderr
    assert 'CONFIG_TEST={}\n'.format(expected) in (tmp_path / '.config').read_text()

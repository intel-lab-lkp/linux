# SPDX-License-Identifier: GPL-2.0-only
"""Validate KCONFIG_PROBABILITY without changing the supported distributions."""

import pytest


@pytest.mark.parametrize('probability', [
    'invalid', ' ', '50%', '50 ', '0x32', '10 20', '10:20x',
    '10:20:invalid', '10:20:30x',
    ':50', '50:', '10::20', '10:20:', '10:20:30:', '10:20:30:40',
    '-1', '101', '0:101', '0:0:101', '60:41', '0:60:41',
    '4294967296', '-4294967296',
    '999999999999999999999999', '-999999999999999999999999',
])
def test_invalid(conf, monkeypatch, probability):
    monkeypatch.setenv('KCONFIG_PROBABILITY', probability)

    assert conf.randconfig(seed=0) == 1
    assert 'KCONFIG_PROBABILITY:' in conf.stderr


@pytest.mark.parametrize('probability, boolean, tristate', [
    ('0', 'n', 'n'),
    ('0:0', 'n', 'n'),
    ('100:0', 'y', 'y'),
    ('0:100', 'y', 'm'),
    ('100:0:0', 'y', 'n'),
    ('0:100:0', 'n', 'y'),
    ('0:0:100', 'n', 'm'),
    ('000:000:100', 'n', 'm'),
    ('+100:0', 'y', 'y'),
    (' \t100:0', 'y', 'y'),
    ('0: \t100:0', 'n', 'y'),
])
def test_valid(conf, monkeypatch, probability, boolean, tristate):
    monkeypatch.setenv('KCONFIG_PROBABILITY', probability)

    assert conf.randconfig(seed=0) == 0
    for symbol, value in [('BOOL', boolean), ('TRI', tristate)]:
        if value == 'n':
            expected = '# CONFIG_{} is not set'.format(symbol)
        else:
            expected = 'CONFIG_{}={}'.format(symbol, value)
        assert expected in conf.config.splitlines()


def test_empty(conf, monkeypatch):
    monkeypatch.delenv('KCONFIG_PROBABILITY', raising=False)
    assert conf.randconfig(seed=0) == 0
    default_config = conf.config

    monkeypatch.setenv('KCONFIG_PROBABILITY', '')
    assert conf.randconfig(seed=0) == 0
    assert conf.config == default_config

# SPDX-License-Identifier: GPL-2.0-only
"""Validate KCONFIG_PROBABILITY without changing the supported distributions."""

import pytest


@pytest.fixture(autouse=True)
def clear_werror(monkeypatch):
    # extra_env can set strict mode, but cannot remove an inherited flag.
    monkeypatch.delenv('KCONFIG_WERROR', raising=False)


@pytest.mark.parametrize('probability', [
    'invalid', ' ', '50%', '50 ', '0x32', '10 20', '10:20x',
    '10:20:invalid', '10:20:30x',
    ':50', '50:', '10::20', '10:20:', '10:20:30:', '10:20:30:40',
    '-0', '+0', '+', '-', '+100:0', ' \t100:0', '0: \t100:0',
])
def test_malformed_warns(conf, probability):
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': probability,
        'KCONFIG_SEED': '0',
    }) == 0
    assert 'warning: KCONFIG_PROBABILITY has malformed format' in conf.stderr
    assert conf.stderr.count('warning:') == 1
    assert conf.config is not None


@pytest.mark.parametrize('probability, equivalent', [
    ('50%', '50:0:0'),
    ('-0', '0'),
])
@pytest.mark.parametrize('seed', range(20))
def test_malformed_preserves_config(conf, probability, equivalent, seed):
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': probability,
        'KCONFIG_SEED': hex(seed),
    }) == 0
    assert 'warning: KCONFIG_PROBABILITY has malformed format' in conf.stderr
    assert conf.stderr.count('warning:') == 1
    malformed = conf.config

    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': equivalent,
        'KCONFIG_SEED': hex(seed),
    }) == 0
    assert 'warning:' not in conf.stderr
    assert conf.config == malformed


@pytest.mark.parametrize('werror', ['', '0', '1'])
@pytest.mark.parametrize('probability, status', [
    ('', 0), ('50', 0), ('50%', 1), ('-0', 1), ('10:20:30:40', 1),
])
def test_werror(conf, probability, status, werror):
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': probability,
        'KCONFIG_SEED': '0',
        'KCONFIG_WERROR': werror,
    }) == status
    if status:
        assert 'warning: KCONFIG_PROBABILITY has malformed format' in conf.stderr
        assert conf.stderr.count('warning:') == 1
    else:
        assert 'warning:' not in conf.stderr


@pytest.mark.parametrize('probability', [
    '-1', '101', '+101', '0:101', '0:0:101', '60:41', '0:60:41',
    '4294967296', '-4294967296',
    '999999999999999999999999', '-999999999999999999999999',
])
def test_out_of_range(conf, probability):
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': probability,
        'KCONFIG_SEED': '0',
    }) == 1
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
])
def test_valid(conf, probability, boolean, tristate):
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': probability,
        'KCONFIG_SEED': '0',
    }) == 0
    assert 'warning:' not in conf.stderr
    for symbol, value in [('BOOL', boolean), ('TRI', tristate)]:
        if value == 'n':
            expected = '# CONFIG_{} is not set'.format(symbol)
        else:
            expected = 'CONFIG_{}={}'.format(symbol, value)
        assert expected in conf.config.splitlines()


@pytest.mark.parametrize('seed', range(20))
def test_single_probability_matches_tristate_split(conf, seed):
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': '50',
        'KCONFIG_SEED': hex(seed),
    }) == 0
    assert 'warning:' not in conf.stderr
    single = conf.config

    # 50% boolean y; 25% tristate y, 25% m, and 50% n.
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': '50:25:25',
        'KCONFIG_SEED': hex(seed),
    }) == 0
    assert 'warning:' not in conf.stderr
    assert conf.config == single


def test_empty(conf, monkeypatch):
    # extra_env overrides inherited variables, but cannot remove them.
    monkeypatch.delenv('KCONFIG_PROBABILITY', raising=False)
    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_SEED': '0',
    }) == 0
    assert 'warning:' not in conf.stderr
    default_config = conf.config

    assert conf._run_conf('--randconfig', extra_env={
        'KCONFIG_PROBABILITY': '',
        'KCONFIG_SEED': '0',
    }) == 0
    assert 'warning:' not in conf.stderr
    assert conf.config == default_config

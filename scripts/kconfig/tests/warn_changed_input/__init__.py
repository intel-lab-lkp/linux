# SPDX-License-Identifier: GPL-2.0
"""
Test optional warnings for user-provided values changed by Kconfig.

Warnings should stay disabled by default, and should only appear when
KCONFIG_WARN_CHANGED_INPUT is enabled.
"""


def test(conf):
    assert conf.olddefconfig('config') == 0
    assert 'user-provided values changed by Kconfig' not in conf.stdout

    assert conf._run_conf('--olddefconfig', dot_config='config',
                          extra_env={
                              'KCONFIG_WARN_CHANGED_INPUT': '1',
                          }) == 0
    assert conf.stdout_contains('expected_stdout')
    assert conf.config_matches('expected_config')

    assert conf._run_conf('--savedefconfig=defconfig', dot_config='config',
                          out_file='defconfig',
                          extra_env={
                              'KCONFIG_WARN_CHANGED_INPUT': '1',
                          }) == 0
    assert conf.stdout_contains('expected_stdout')
    assert conf.config_matches('expected_defconfig')

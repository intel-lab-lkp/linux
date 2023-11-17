# SPDX-License-Identifier: GPL-2.0
#
# Utilities useful in DPLL system integration testing.
#
# Copyright (c) 2023, Intel Corporation.
# Author: Michal Michalik <michal.michalik@intel.com>

from .ynlfamilyhandler import YnlFamilyHandler


def read_nsim_debugfs(entry):
    if not entry.exists():
        raise FileNotFoundError

    with open(entry) as f:
        return f.read()


def write_nsim_debugfs(entry, data):
    if not entry.exists():
        raise FileNotFoundError

    with open(entry, 'w') as f:
        return f.write(data)


def get_dpll_id(clock_id, test_module, _type):
    '''
    YNL helper for getting the DPLL clock ID
    '''
    yfh = YnlFamilyHandler()
    yfh.do = 'device-id-get'
    yfh.attrs = {
        'module-name': test_module,
        'clock-id': clock_id,
        'type': _type
        }
    return yfh.execute()['id']


def get_dpll(clock_id, test_module, _type):
    '''
    YNL helper for getting the DPLL clock object
    '''
    _id = get_dpll_id(clock_id, test_module, _type)
    yfh = YnlFamilyHandler()
    yfh.do = 'device-get'
    yfh.attrs = {'id': _id}
    return yfh.execute()


def get_all_pins():
    '''
    YNL helper for getting the all DPLL pins
    '''
    yfh = YnlFamilyHandler()
    yfh.dump = 'pin-get'
    return yfh.execute()


def get_pin_id(test_module, clock_id, board_l, panel_l, package_l, type):
    '''
    YNL helper for getting DPLL pin ID
    '''
    yfh = YnlFamilyHandler()
    yfh.do = 'pin-id-get'
    yfh.attrs = {'module-name': test_module,
                 'clock-id': clock_id,
                 'board-label': board_l,
                 'panel-label': panel_l,
                 'package-label': package_l,
                 'type': type}
    return yfh.execute()['id']


def get_pin(_id):
    '''
    YNL helper for getting the DPLL pin object
    '''
    yfh = YnlFamilyHandler()
    yfh.do = 'pin-get'
    yfh.attrs = {'id': _id}
    return yfh.execute()


def set_pin(_id, params):
    '''
    YNL helper for setting the DPLL pin parameters
    '''
    yfh = YnlFamilyHandler()
    yfh.do = 'pin-set'
    yfh.attrs = params
    yfh.attrs['id'] = _id
    return yfh.execute()

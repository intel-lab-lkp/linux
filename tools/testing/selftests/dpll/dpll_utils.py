# SPDX-License-Identifier: GPL-2.0
#
# Utilities useful in DPLL system integration testing.
#
# Copyright (c) 2023, Intel Corporation.
# Author: Michal Michalik <michal.michalik@intel.com>

import re

from .ynlfamilyhandler import YnlFamilyHandler


def parse_header(filepath):
    '''
    Simple parser for C headers to dynamically read the defines/consts
    '''
    FILTER = ('(u32)', '(u64)', '(', ')')

    with open(filepath) as f:
        header = f.read()

    items = {}
    matches = re.findall(r'''\#define
                             \s+?
                             ([\w\d]*)  # key
                             \s+?
                             ([\w\d)()"]+)  # value
                          ''', header, re.MULTILINE | re.VERBOSE)
    for key, value in matches:
        for f in FILTER:
            value = value.replace(f, '')
        if value.isnumeric():
            items[key] = int(value)
        elif not value:
            items[key] = True  # let's just treat it as a flag
        else:
            items[key] = value

    return items


def get_dpll_id(clock_id, test_module, type):
    '''
    YNL helper for getting the DPLL clock ID
    '''
    yfh = YnlFamilyHandler()
    yfh.do = 'device-id-get'
    yfh.attrs = {
        'module-name': test_module,
        'clock-id': clock_id,
        'type': type
        }
    return yfh.execute()['id']


def get_dpll(clock_id, test_module, type):
    '''
    YNL helper for getting the DPLL clock object
    '''
    _id = get_dpll_id(clock_id, test_module, type)
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

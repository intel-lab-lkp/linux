# SPDX-License-Identifier: GPL-2.0
#
# System integration tests for DPLL interface.
#
# Can be used directly, but strongly suggest using wrapper: run_dpll_tests.sh
# The wrapper takes care about fulfilling all the requirements needed to
# run all the tests.
#
# Copyright (c) 2023, Intel Corporation.
# Author: Michal Michalik <michal.michalik@intel.com>

import subprocess
from pathlib import Path

import pytest

from .consts import DPLL_TYPE, DPLL_LOCK_STATUS, DPLL_PIN_TYPE, DPLL_PIN_CAPS
from .dpll_utils import get_dpll, get_dpll_id, get_pin_id, \
    get_all_pins, get_pin, set_pin, read_nsim_debugfs, write_nsim_debugfs
from .ynlfamilyhandler import YnlFamilyHandler
from lib.ynl import NlError


DPLL_CONSTS = 'drivers/net/netdevsim/dpll.c'
TEST_MODULE = 'netdevsim'
NETDEVSIM_PATH = '/sys/bus/netdevsim/'
NETDEVSIM_NEW_DEVICE = Path(NETDEVSIM_PATH) / 'new_device'
NETDEVSIM_DEL_DEVICE = Path(NETDEVSIM_PATH) / 'del_device'
NETDEVSIM_DEVICES = Path(NETDEVSIM_PATH) / 'devices'
NETDEVSIM_DEBUGFS = '/sys/kernel/debug/netdevsim/netdevsim{}/'


@pytest.fixture(scope="class", params=((0,), (1, 0), (0, 1)))
def env(request):
    environment = {}
    environment['dev_id'] = 0
    environment['dbgfs'] = Path(
        NETDEVSIM_DEBUGFS.format(environment['dev_id']))

    for i in request.param:
        with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
            f.write(f'{i} 1 4')

    environment['clock_id'] = int(read_nsim_debugfs(
        environment['dbgfs'] / 'dpll_clock_id'))

    yield environment

    for i in request.param:
        with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
            f.write(f'{i}')


class TestDPLL:
    def test_if_module_is_loaded(self):
        '''
        Checks if the module is successfully loaded at all. It should be already
        covered in the class setup (raise exception) - but just to make sure.
        '''
        s = subprocess.run(['lsmod'], check=True, capture_output=True)
        assert TEST_MODULE in str(s.stdout)

    def test_get_two_dplls(self, env):
        '''
        Checks if the netlink is returning the expected DPLLs. Need to make sure
        that even if "other" DPLLs exist in the system we check only ours.
        '''
        yfh = YnlFamilyHandler()
        yfh.dump = 'device-get'
        reply = yfh.execute()

        dplls = filter(lambda i: TEST_MODULE == i['module-name']
                       and i['clock-id'] == env['clock_id'],
                       reply)
        assert len(list(dplls)) == 2

    def test_get_two_distinct_dplls(self):
        '''
        Checks if the netlink is returning the expected, distinct DPLLs created
        by the tested module. Expect EEC and PPS.
        '''
        yfh = YnlFamilyHandler()
        yfh.dump = 'device-get'
        reply = yfh.execute()

        dplls = filter(lambda i: TEST_MODULE in i['module-name'], reply)
        types = set(i['type'] for i in dplls)

        assert types == {'eec', 'pps'}

    @pytest.mark.parametrize("dtype", [DPLL_TYPE.EEC, DPLL_TYPE.PPS])
    def test_finding_dpll_id(self, env, dtype):
        '''
        Checks if it is possible to find the DPLL id using 'device-id-get' do cmd.
        '''
        _id = get_dpll_id(env['clock_id'], TEST_MODULE,
                          dtype.value)
        assert isinstance(_id, int)

    @pytest.mark.parametrize("clk,dtype,exc", [(123, DPLL_TYPE.EEC.value, KeyError),
                                               (234, 4, NlError),
                                               (123, 4, NlError)])
    def test_finding_fails_correctly(self, clk, dtype, exc):
        '''
        Make sure the DPLL interface does not return any garbage on incorrect
        input like wrong DPLL type or clock id.
        '''
        with pytest.raises(exc):
            get_dpll_id(clk, TEST_MODULE, dtype)

    @pytest.mark.parametrize("dtype", [DPLL_TYPE.EEC, DPLL_TYPE.PPS])
    def test_get_only_one_dpll(self, env, dtype):
        '''
        Checks if the netlink is returning the expected DPLLs created
        by the tested module, filtered on input. Expect EEC and PPS here.
        '''
        _id = get_dpll_id(env['clock_id'], TEST_MODULE, dtype.value)

        yfh = YnlFamilyHandler()
        yfh.do = 'device-get'
        yfh.attrs = {'id': _id}
        reply = yfh.execute()

        assert reply['type'] == dtype.name.lower()

    @pytest.mark.parametrize("dtype, dbgf", [(DPLL_TYPE.EEC, 'dpll_e_temp'),
                                             (DPLL_TYPE.PPS, 'dpll_p_temp')])
    def test_get_temperature(self, env, dtype, dbgf):
        '''
        Checks if it is possible to get correct DPLL temperature for both DPLLs.
        '''
        desired_temp = int(read_nsim_debugfs(env['dbgfs'] / dbgf))

        dpll = get_dpll(env['clock_id'], TEST_MODULE, dtype.value)

        assert dpll['temp'] == desired_temp

    @pytest.mark.parametrize("dtype, lock, dbgf",
                             [(DPLL_TYPE.EEC, DPLL_LOCK_STATUS.UNLOCKED, "dpll_e_status"),
                              (DPLL_TYPE.PPS, DPLL_LOCK_STATUS.UNLOCKED,
                               "dpll_p_status"),
                              (DPLL_TYPE.EEC, DPLL_LOCK_STATUS.LOCKED, "dpll_e_status"),
                              (DPLL_TYPE.PPS, DPLL_LOCK_STATUS.LOCKED, "dpll_p_status")])
    def test_get_lock(self, env, dtype, lock, dbgf):
        '''
        Checks if it is possible to get correct DPLL lock status for both DPLLs.
        '''
        write_nsim_debugfs(env['dbgfs'] / dbgf, str(lock.value))

        dpll = get_dpll(env['clock_id'], TEST_MODULE,
                        dtype.value)
        assert dpll['lock-status'] == lock.name.lower()

    @pytest.mark.parametrize("dtype, desired_pins", [(DPLL_TYPE.EEC, 3), (DPLL_TYPE.PPS, 2)])
    def test_dump_pins_in_each_dpll(self, env, dtype, desired_pins):
        '''
        Checks if it is possible to dump all pins for each DPLL separetely,
        filtered on output.
        '''
        dpll = get_dpll(env['clock_id'], TEST_MODULE,
                        dtype.value)

        yfh = YnlFamilyHandler()
        yfh.dump = 'pin-get'
        reply = yfh.execute()

        pins = filter(lambda p: any(i['parent-id'] == dpll['id']
                      for i in p.get('parent-device', [])), reply)

        assert len(list(pins)) == desired_pins

    def test_dump_all_pins_in_both_dplls(self, env):
        '''
        Checks if it is possible to dump all pins for both DPLLs, filtered by
        clock id on output.
        '''
        desired_pins = 3  # all pins are in EEC

        reply = get_all_pins()

        pins = filter(lambda p: p['clock-id'] == env['clock_id'], reply)

        assert len(list(pins)) == desired_pins

    @pytest.mark.parametrize("pin, pin_name, priority, caps",
                             [(DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK', 7,
                               DPLL_PIN_CAPS.PRIO_CAN_CHG.value |
                               DPLL_PIN_CAPS.STATE_CAN_CHG.value),
                              (DPLL_PIN_TYPE.GNSS, 'GNSS', 5,
                               DPLL_PIN_CAPS.PRIO_CAN_CHG.value),
                              (DPLL_PIN_TYPE.EXT, 'PPS', 6,
                               DPLL_PIN_CAPS.PRIO_CAN_CHG.value |
                               DPLL_PIN_CAPS.STATE_CAN_CHG.value |
                               DPLL_PIN_CAPS.DIR_CAN_CHG.value)])
    def test_get_a_single_pin_from_dump(self, env, pin, pin_name, priority,
                                        caps):
        '''
        Checks if it is possible to get all distinct pins for both DPLLs, filtered
        by clock id and type on output. Additionally, verify if the priority is
        assigned correctly and not mixed up.
        '''
        reply = get_all_pins()

        pin_name = pin.name.lower().replace('_', '-')
        pins = filter(lambda p:
                      p['clock-id'] == env['clock_id'] and p['type'] == pin_name, reply)
        pins = list(pins)

        assert len(pins) == 1
        assert pins[0]['capabilities'] == caps
        for p in pins[0]['parent-device']:
            assert p['prio'] == priority

    @pytest.mark.parametrize("pin, pin_name",
                             [(DPLL_PIN_TYPE.EXT, 'PPS'),
                              (DPLL_PIN_TYPE.GNSS, 'GNSS'),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0')])
    def test_get_a_single_pin_id(self, env, pin, pin_name):
        '''
        Checks if it is possible to get single pins using 'get-pin-id' do
        command.
        '''
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, env['clock_id'], board_l, panel_l,
                         package, pin.value)
        assert isinstance(_id, int)

    @pytest.mark.parametrize("pin, pin_name, param, value",
                             [(DPLL_PIN_TYPE.EXT, 'PPS', 'prio', 1),
                              (DPLL_PIN_TYPE.GNSS, 'GNSS', 'prio', 2),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0', 'prio', 3)])
    def test_set_a_single_pin_prio(self, env, pin, pin_name, param, value):
        '''
        Checks if it is possible to set pins priority using 'set-pin' do
        command.
        '''
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, env['clock_id'], board_l, panel_l,
                         package, pin.value)

        pins_before = get_all_pins()
        pin_before = get_pin(_id)

        # both DPLL's are handled the same in the test module
        first_dpll_id = pin_before['parent-device'][0]['parent-id']
        set_pin(_id, {"parent-device":
                      {"parent-id": first_dpll_id, param: value}})

        pins_after = get_all_pins()

        # assume same order, if that might change - test need to be updated
        for i in range(len(pins_before)):
            if pins_after[i]['id'] != _id:
                assert pins_after[i] == pins_before[i]
            else:
                assert pins_after[i]["parent-device"][0][param] == value

        # set the original value back to leave the same state after test
        original_value = pin_before["parent-device"][0][param]
        set_pin(_id, {"parent-device":
                      {"parent-id": first_dpll_id, param: original_value}})

    @pytest.mark.parametrize("pin, pin_name, param, value",
                             [(DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0',
                               'frequency', int(1e6)),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0',
                               'frequency', int(12e6))])
    def test_set_a_single_pin_freq(self, env, pin, pin_name, param, value):
        '''
        Checks if it is possible to set pins frequency using 'set-pin' do
        command.
        '''
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, env['clock_id'], board_l, panel_l,
                         package, pin.value)

        pins_before = get_all_pins()
        pin_before = get_pin(_id)

        set_pin(_id, {param: value})

        pins_after = get_all_pins()

        # assume same order, if that might change - test need to be updated
        for i in range(len(pins_before)):
            if pins_after[i]['id'] != _id:
                assert pins_after[i] == pins_before[i]
            else:
                assert pins_after[i][param] == value

        # set the original value back to leave the same state after test
        set_pin(_id, {param: pin_before[param]})

    @pytest.mark.parametrize("pin, pin_name, param, value",
                             [(DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0',
                               'frequency', int(1e5)),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0',
                               'frequency', int(130e6))])
    def test_set_a_single_pin_fail(self, env, pin, pin_name, param, value):
        '''
        Checks if we fail correctly trying to set incorrect pin frequency.
        '''
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, env['clock_id'], board_l, panel_l,
                         package, pin.value)

        with pytest.raises(NlError):
            set_pin(_id, {param: value})


@pytest.fixture(scope="class")
def ntf_env():
    '''
    This test suite prepares the env by arming the event tracking,
    loading the driver, changing pin, unloading the driver and gathering
    logs for further processing.
    '''
    environment = {}
    environment['dev_id'] = 0
    environment['dbgfs'] = Path(
        NETDEVSIM_DEBUGFS.format(environment['dev_id']))

    yfh = YnlFamilyHandler(ntf='monitor')

    with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
        f.write(f'{environment["dev_id"]} 1 4')

    pin = DPLL_PIN_TYPE.GNSS
    clock_id = read_nsim_debugfs(environment['dbgfs'] / 'dpll_clock_id')
    board_l = f'{pin.name}_brd'
    panel_l = f'{pin.name}_pnl'
    package = f'{pin.name}_pcg'

    _id = get_pin_id(TEST_MODULE, clock_id, board_l,
                     panel_l, package, pin.value)

    g_pin = get_pin(_id)

    # both DPLL's are handled the same in the test module
    first_dpll_id = g_pin['parent-device'][0]['parent-id']
    set_pin(_id, {"parent-device": {"parent-id": first_dpll_id, 'prio': 2}})

    with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
        f.write(f'{environment["dev_id"]}')

    yfh.ynl.check_ntf()
    environment['events'] = yfh.ynl.async_msg_queue

    yield environment


class TestDPLLsNTF:
    @pytest.mark.parametrize(('event', 'count'), [('device-create-ntf', 2),
                                                  ('device-delete-ntf', 2),
                                                  ('pin-change-ntf', 1),
                                                  ('pin-create-ntf', 5),
                                                  ('pin-delete-ntf', 5)])
    def test_number_of_events(self, ntf_env, event, count):
        '''
        Checks if we are getting exact number of events that we expect to be
        gathered while monitoring the DPLL subsystem.
        '''
        f_events = filter(lambda i: i['name'] == event, ntf_env['events'])
        assert len(list(f_events)) == count

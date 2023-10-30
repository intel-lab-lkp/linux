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

import pytest
import subprocess
from pathlib import Path

from .consts import KSRC, DPLL_TYPE, DPLL_LOCK_STATUS, DPLL_PIN_TYPE
from .dpll_utils import parse_header, get_dpll, get_dpll_id, get_pin_id, \
    get_all_pins, get_pin, set_pin
from .ynlfamilyhandler import YnlFamilyHandler
from lib.ynl import NlError


DPLL_CONSTS = 'drivers/net/netdevsim/dpll.h'
TEST_MODULE = 'netdevsim'
NETDEVSIM_PATH = '/sys/bus/netdevsim/'
NETDEVSIM_NEW_DEVICE = Path(NETDEVSIM_PATH) / 'new_device'
NETDEVSIM_DEL_DEVICE = Path(NETDEVSIM_PATH) / 'del_device'
NETDEVSIM_DEVICES = Path(NETDEVSIM_PATH) / 'devices'


class TestDPLL:
    DEV_ID = 0

    @classmethod
    def setup_class(cls):
        cls.module_consts = parse_header(Path(KSRC) / DPLL_CONSTS)
        with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID} 1 4')

    @classmethod
    def teardown_class(cls):
        with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID}')

    def test_if_module_is_loaded(self):
        '''
        Checks if the module is successfully loaded at all. It should be already
        covered in the class setup (raise exception) - but just to make sure.
        '''
        s = subprocess.run(['lsmod'], check=True, capture_output=True)
        assert TEST_MODULE in str(s.stdout)

    def test_get_two_dplls(self):
        '''
        Checks if the netlink is returning the expected DPLLs. Need to make sure
        that even if "other" DPLLs exist in the system we check only ours.
        '''
        yfh = YnlFamilyHandler()
        yfh.dump = 'device-get'
        reply = yfh.execute()

        dplls = filter(lambda i: TEST_MODULE == i['module-name']
                       and i['clock-id'] == self.module_consts['DPLLS_CLOCK_ID'],
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
    def test_finding_dpll_id(self, dtype):
        '''
        Checks if it is possible to find the DPLL id using 'device-id-get' do cmd.
        '''
        _id = get_dpll_id(self.module_consts['DPLLS_CLOCK_ID'], TEST_MODULE,
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
    def test_get_only_one_dpll(self, dtype):
        '''
        Checks if the netlink is returning the expected DPLLs created
        by the tested module, filtered on input. Expect EEC and PPS here.
        '''
        _id = get_dpll_id(self.module_consts['DPLLS_CLOCK_ID'], TEST_MODULE,
                          dtype.value)

        yfh = YnlFamilyHandler()
        yfh.do = 'device-get'
        yfh.attrs = {'id': _id}
        reply = yfh.execute()

        assert reply['type'] == dtype.name.lower()

    @pytest.mark.parametrize("dtype", [DPLL_TYPE.EEC, DPLL_TYPE.PPS])
    def test_get_temperature(self, dtype):
        '''
        Checks if it is possible to get correct DPLL temperature for both DPLLs.
        '''
        if dtype == DPLL_TYPE.EEC:
            desired_temp = self.module_consts['EEC_DPLL_TEMPERATURE']
        else:
            desired_temp = self.module_consts['PPS_DPLL_TEMPERATURE']

        dpll = get_dpll(self.module_consts['DPLLS_CLOCK_ID'], TEST_MODULE,
                        dtype.value)

        assert dpll['temp'] == desired_temp

    @pytest.mark.parametrize("dtype, lock", [(DPLL_TYPE.EEC, DPLL_LOCK_STATUS.UNLOCKED),
                                             (DPLL_TYPE.PPS, DPLL_LOCK_STATUS.LOCKED)])
    def test_get_lock(self, dtype, lock):
        '''
        Checks if it is possible to get correct DPLL lock status for both DPLLs.
        '''
        dpll = get_dpll(self.module_consts['DPLLS_CLOCK_ID'], TEST_MODULE,
                        dtype.value)
        assert dpll['lock-status'] == lock.name.lower()

    @pytest.mark.parametrize("dtype", [DPLL_TYPE.EEC, DPLL_TYPE.PPS])
    def test_dump_pins_in_each_dpll(self, dtype):
        '''
        Checks if it is possible to dump all pins for each DPLL separetely,
        filtered on output.
        '''
        if dtype == DPLL_TYPE.EEC:
            desired_pins = self.module_consts['EEC_PINS_NUMBER']
        else:
            desired_pins = self.module_consts['PPS_PINS_NUMBER']

        dpll = get_dpll(self.module_consts['DPLLS_CLOCK_ID'], TEST_MODULE,
                        dtype.value)

        yfh = YnlFamilyHandler()
        yfh.dump = 'pin-get'
        reply = yfh.execute()

        pins = filter(lambda p: any(i['parent-id'] == dpll['id']
                      for i in p.get('parent-device', [])), reply)

        assert len(list(pins)) == desired_pins

    def test_dump_all_pins_in_both_dplls(self):
        '''
        Checks if it is possible to dump all pins for both DPLLs, filtered by
        clock id on output.
        '''
        desired_pins = self.module_consts['EEC_PINS_NUMBER']  # all are in EEC
        clock_id = self.module_consts['DPLLS_CLOCK_ID']

        reply = get_all_pins()

        pins = filter(lambda p: p['clock-id'] == clock_id, reply)

        assert len(list(pins)) == desired_pins

    @pytest.mark.parametrize("pin, pin_name", [(DPLL_PIN_TYPE.EXT, 'PPS'),
                                               (DPLL_PIN_TYPE.GNSS, 'GNSS'),
                                               (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK')])
    def test_get_a_single_pin_from_dump(self, pin, pin_name):
        '''
        Checks if it is possible to get all distinct pins for both DPLLs, filtered
        by clock id and type on output. Additionally, verify if the priority is
        assigned correctly and not mixed up.
        '''
        clock_id = self.module_consts['DPLLS_CLOCK_ID']
        priority = self.module_consts[f'PIN_{pin_name}_PRIORITY']
        caps = self.module_consts[f'PIN_{pin_name}_CAPABILITIES']

        reply = get_all_pins()

        pin_name = pin.name.lower().replace('_', '-')
        pins = filter(lambda p:
                      p['clock-id'] == clock_id and p['type'] == pin_name, reply)
        pins = list(pins)

        assert len(pins) == 1
        assert pins[0]['capabilities'] == caps
        for p in pins[0]['parent-device']:
            assert p['prio'] == priority

    @pytest.mark.parametrize("pin, pin_name",
                             [(DPLL_PIN_TYPE.EXT, 'PPS'),
                              (DPLL_PIN_TYPE.GNSS, 'GNSS'),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0')])
    def test_get_a_single_pin_id(self, pin, pin_name):
        '''
        Checks if it is possible to get single pins using 'get-pin-id' do
        command.
        '''
        clock_id = self.module_consts['DPLLS_CLOCK_ID']
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, clock_id, board_l, panel_l, package, pin.value)
        assert isinstance(_id, int)

    @pytest.mark.parametrize("pin, pin_name, param, value",
                             [(DPLL_PIN_TYPE.EXT, 'PPS', 'prio', 1),
                              (DPLL_PIN_TYPE.GNSS, 'GNSS', 'prio', 2),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0', 'prio', 3)])
    def test_set_a_single_pin_prio(self, pin, pin_name, param, value):
        '''
        Checks if it is possible to set pins priority using 'set-pin' do
        command.
        '''
        clock_id = self.module_consts['DPLLS_CLOCK_ID']
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, clock_id, board_l, panel_l, package, pin.value)

        pins_before = get_all_pins()
        pin_before = get_pin(_id)

        # both DPLL's are handled the same in the test module
        first_dpll_id = pin_before['parent-device'][0]['parent-id']
        set_pin(_id, {"parent-device": {"parent-id": first_dpll_id, param: value}})

        pins_after = get_all_pins()

        # assume same order, if that might change - test need to be updated
        for i in range(len(pins_before)):
            if pins_after[i]['id'] != _id:
                assert pins_after[i] == pins_before[i]
            else:
                assert pins_after[i]["parent-device"][0][param] == value

        # set the original value back to leave the same state after test
        original_value = pin_before["parent-device"][0][param]
        set_pin(_id, {"parent-device": {"parent-id": first_dpll_id, param: original_value}})

    @pytest.mark.parametrize("pin, pin_name, param, value",
                             [(DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0', 'frequency', int(1e6)),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0', 'frequency', int(12e6))])
    def test_set_a_single_pin_freq(self, pin, pin_name, param, value):
        '''
        Checks if it is possible to set pins frequency using 'set-pin' do
        command.
        '''
        clock_id = self.module_consts['DPLLS_CLOCK_ID']
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, clock_id, board_l, panel_l, package, pin.value)

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
                             [(DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0', 'frequency', int(1e5)),
                              (DPLL_PIN_TYPE.SYNCE_ETH_PORT, 'RCLK_0', 'frequency', int(130e6))])
    def test_set_a_single_pin_fail(self, pin, pin_name, param, value):
        '''
        Checks if we fail correctly trying to set incorrect pin frequency.
        '''
        clock_id = self.module_consts['DPLLS_CLOCK_ID']
        board_l = f'{pin_name}_brd'
        panel_l = f'{pin_name}_pnl'
        package = f'{pin_name}_pcg'

        _id = get_pin_id(TEST_MODULE, clock_id, board_l, panel_l, package, pin.value)

        with pytest.raises(NlError):
            set_pin(_id, {param: value})


class TestTwoDPLLsOtherFirst(TestDPLL):
    '''
    Add a second module to the environment, which registers other DPLLs to make
    sure the references are not mixed together. Other driver first.
    '''
    DEV_ID = 0
    OTHER_DEV_ID = 2
    OTHER_DEV_PORTS = 2

    @classmethod
    def setup_class(cls):
        cls.module_consts = parse_header(Path(KSRC) / DPLL_CONSTS)
        with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
            f.write(f'{cls.OTHER_DEV_ID} {cls.OTHER_DEV_PORTS} 4')
        with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID} 1 4')

    @classmethod
    def teardown_class(cls):
        with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
            f.write(f'{cls.OTHER_DEV_ID}')
        with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID}')


class TestTwoDPLLsTestedFirst(TestDPLL):
    '''
    Add a second module to the environment, which registers other DPLLs to make
    sure the references are not mixed together. Tested driver first.
    '''
    DEV_ID = 0
    OTHER_DEV_ID = 2
    OTHER_DEV_PORTS = 2

    @classmethod
    def setup_class(cls):
        cls.module_consts = parse_header(Path(KSRC) / DPLL_CONSTS)
        with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID} 1 4')
        with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
            f.write(f'{cls.OTHER_DEV_ID} {cls.OTHER_DEV_PORTS} 4')

    @classmethod
    def teardown_class(cls):
        with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID}')
        with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
            f.write(f'{cls.OTHER_DEV_ID}')


class TestDPLLsNTF:
    MULTICAST_GROUP = 'monitor'
    DEV_ID = 0

    @classmethod
    def setup_class(cls):
        '''
        This test suite prepares the environment by arming the event tracking,
        loading the driver, changing pin, unloading the driver and gathering
        logs for further processing.
        '''
        cls.module_consts = parse_header(Path(KSRC) / DPLL_CONSTS)

        yfh = YnlFamilyHandler(ntf=cls.MULTICAST_GROUP)

        with open(NETDEVSIM_NEW_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID} 1 4')

        pin = DPLL_PIN_TYPE.GNSS
        clock_id = cls.module_consts['DPLLS_CLOCK_ID']
        board_l = f'{pin.name}_brd'
        panel_l = f'{pin.name}_pnl'
        package = f'{pin.name}_pcg'

        _id = get_pin_id(TEST_MODULE, clock_id, board_l, panel_l, package, pin.value)

        g_pin = get_pin(_id)

        # both DPLL's are handled the same in the test module
        first_dpll_id = g_pin['parent-device'][0]['parent-id']
        set_pin(_id, {"parent-device": {"parent-id": first_dpll_id, 'prio': 2}})

        with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
            f.write(f'{cls.DEV_ID}')

        yfh.ynl.check_ntf()
        cls.events = yfh.ynl.async_msg_queue

    @classmethod
    def teardown_class(cls):
        # Cleanup, in case something in the setup_class failed
        ls_devices = subprocess.run(['ls', NETDEVSIM_DEVICES], capture_output=True)
        if f'netdevsim{cls.DEV_ID}' in str(ls_devices.stdout):
            with open(NETDEVSIM_DEL_DEVICE, 'w') as f:
                f.write(f'{cls.DEV_ID}')

    @pytest.mark.parametrize(('event', 'count'), [('device-create-ntf', 2),
                                                  ('device-delete-ntf', 2),
                                                  ('pin-change-ntf', 1),
                                                  ('pin-create-ntf', 5),
                                                  ('pin-delete-ntf', 5)])
    def test_number_of_events(self, event, count):
        '''
        Checks if we are getting exact number of events that we expect to be
        gathered while monitoring the DPLL subsystem.
        '''
        f_events = filter(lambda i: i['name'] == event, self.events)
        assert len(list(f_events)) == count

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Driver-related behavior tests for Pause-based Flow Control.
"""

import errno

from lib.py import (
    EthtoolFamily,
    KsftFailEx,
    KsftNamedVariant,
    KsftSkipEx,
    NetDrvEpEnv,
    cmd,
    controllable_lp,
    defer,
    ethtool,
    ethtool_ret,
    forced_link_settings,
    ip,
    ksft_disruptive,
    ksft_eq,
    ksft_exit,
    ksft_in,
    ksft_not_in,
    ksft_pr,
    ksft_run,
    ksft_variants,
    onoff,
    require_controllable_lp,
    require_link_autoneg,
    wait_for_aneg,
    wait_for_link,
)

# Linkmodes to Pause params :
# Pause bit is set if rx == 1
# Asym_Pause bit is set if rx != tx
pauseparams_to_linkmodes = {
    0: {0: {"rx": 0, "tx": 0, "linkmodes": []},
        1: {"rx": 0, "tx": 1, "linkmodes": ["Asym_Pause"]}},
    1: {0: {"rx": 1, "tx": 0, "linkmodes": ["Pause", "Asym_Pause"]},
        1: {"rx": 1, "tx": 1, "linkmodes": ["Pause"]}},
}

pauseparams_variants = [
    KsftNamedVariant(f"RX {onoff(p['rx'])} TX {onoff(p['tx'])}", p)
    for by_tx in pauseparams_to_linkmodes.values() for p in by_tx.values()
]

def _ethtool_pause_use_to_linkmodes(use) -> list[str]:
    if use == "Symmetric":
        return ["Pause"]
    elif use == "Symmetric Receive-only":
        return ["Pause", "Asym_Pause"]
    elif use == "Transmit-only":
        return ["Asym_Pause"]
    else:
        return []

def pause_to_linkmodes(rx, tx) -> list[str]:
    """ Convert rx/tx pauseparams to the corresponding linkmodes
    """
    return pauseparams_to_linkmodes[rx][tx]["linkmodes"]

def set_local_pauseparams(cfg, rx, tx, aneg) -> int:
    """ set pauseparams : ethtool -A

    Raise an error if the return is not 0 or EOPNOTSUPP

    :param cfg: test config
    :param rx: rx pause enabled or disabled
    :param tx: tx pause enabled or disabled
    :param aneg: pause autoneg enabled or disabled
    :returns: return code of the ethtool command
    """
    rx_param = onoff(rx)
    tx_param = onoff(tx)
    aneg_param = onoff(aneg)

    ret, _ = ethtool_ret(f"-A {cfg.ifname} rx {rx_param} tx {tx_param}"
                         f"autoneg {aneg_param}",
                         is_get = False)

    return ret

def get_local_pauseparams(cfg) -> tuple[int, bool, bool, bool]:
    """ get pauseparams : ethtool -a

    Raise an error if the return is not 0 or EOPNOTSUPP

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command,
              - rx status,
              - tx status,
              - aneg status
    """
    return ethtool_ret(f"-a {cfg.ifname}", is_get=True)

def set_peer_pauseparams(cfg, rx, tx, aneg) -> int:
    """ set pauseparams : ethtool -A

    Raise an error if the return is not 0 or EOPNOTSUPP

    :param cfg: test config
    :param rx: rx pause enabled or disabled
    :param tx: tx pause enabled or disabled
    :param aneg: pause autoneg enabled or disabled
    :returns: return code of the ethtool command
    """
    rx_param = onoff(rx)
    tx_param = onoff(tx)
    aneg_param = onoff(aneg)

    ret, _ = ethtool_ret(
                         f"-A {cfg.remote_ifname} rx {rx_param} tx {tx_param} autoneg {aneg_param}",
                         is_get = False, host=cfg.remote)

    if ret != 0:
        raise KsftSkipEx(f"Can't set pauseparams on peer: {errno.errorcode.get(ret, ret)}")

    return ret

def get_peer_pauseparams(cfg) -> tuple[int, bool, bool, bool]:
    """ get pauseparams : ethtool -a

    Raise an error if the return is not 0 or EOPNOTSUPP

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command,
              - rx status,
              - tx status,
              - aneg status
    """
    return ethtool_ret(f"-a {cfg.remote_ifname}", is_get=True,
                       host=cfg.remote)

def get_local_pause_supported(cfg) -> tuple[int, list[str]]:
    """ get the supported linkmodes on the local device

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    ret, data = ethtool_ret(f"{cfg.ifname}")
    if ret != 0:
        raise KsftFailEx(f"ethtool {cfg.ifname} failed: {ret}")

    return ret, _ethtool_pause_use_to_linkmodes(data["supported-pause-frame-use"])

def get_local_pause_advertising(cfg) -> tuple[int, list[str]]:
    """ get the advertised linkmodes on the local device

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    ret, data = ethtool_ret(f"{cfg.ifname}")
    if ret != 0:
        raise KsftFailEx(f"ethtool {cfg.ifname} failed: {ret}")

    return ret, _ethtool_pause_use_to_linkmodes(data["advertised-pause-frame-use"])

def get_local_pause_lp_advertising(cfg) -> tuple[int, list[str]]:
    """ get the lp_advertised linkmodes on the local device

    Raise an error if the return is not 0 or EOPNOTSUPP
    Prints a warning if the local device doesn't report lp_advertising

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    ret, data = ethtool_ret(f"{cfg.ifname}")
    if ret != 0:
        raise KsftFailEx(f"ethtool {cfg.ifname} failed: {errno.errorcode.get(ret, ret)}")

    if "link-partner-advertised-pause-frame-use" in data:
        return ret, _ethtool_pause_use_to_linkmodes(data["link-partner-advertised-pause-frame-use"])
    else:
        ksft_pr(f"Warning: {cfg.ifname} does not report the LP's advertising")
        return errno.EOPNOTSUPP, None

def get_peer_pause_supported(cfg) -> tuple[int, list[str]]:
    """ get the supported linkmodes on the link partner

    returns ENODEV if there's no LP

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    ret, data = ethtool_ret(f"{cfg.remote_ifname}", host = cfg.remote)
    if ret != 0:
        raise KsftFailEx(f"ethtool {cfg.remote_ifname} failed: {errno.errorcode.get(ret, ret)}")

    return ret, _ethtool_pause_use_to_linkmodes(data["supported-pause-frame-use"])


def get_peer_pause_advertising(cfg) -> tuple[int, list[str]]:
    """ get the advertised linkmodes on the link partner

    returns ENODEV if there's no LP

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    ret, data = ethtool_ret(f"{cfg.remote_ifname}", host = cfg.remote)
    if ret != 0:
        raise KsftFailEx(f"ethtool {cfg.remote_ifname} failed: {errno.errorcode.get(ret, ret)}")

    return ret, _ethtool_pause_use_to_linkmodes(data["advertised-pause-frame-use"])

def get_peer_pause_lp_advertising(cfg) -> tuple[int, list[str]]:
    """ get the lp_advertised linkmodes on the link partner

    Raise an error if the return is not 0 or EOPNOTSUPP
    returns ENODEV if there's no LP
    Prints a warning if LP is present but doesn't report lp_advertising

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    ret, data = ethtool_ret(f"{cfg.remote_ifname}", host = cfg.remote)
    if ret != 0:
        raise KsftFailEx(f"ethtool {cfg.remote_ifname} failed: {errno.errorcode.get(ret, ret)}")

    if "link-partner-advertised-pause-frame-use" in data:
        return ret, _ethtool_pause_use_to_linkmodes(data["link-partner-advertised-pause-frame-use"])
    else:
        ksft_pr(f"Warning: {cfg.remote_ifname} does not report the LP's advertising")
        return errno.EOPNOTSUPP, None

def require_pause_supported_anyof(cfg, linkmodes) -> None:
    """ Checks if the local device supports pause at all, by looking at the
        supported bitfield. Raises a skip it's not supported.

        :param cfg: test config
    """
    ret, _ = get_local_pauseparams(cfg)
    if ret != 0:
        raise KsftSkipEx("device doesn't allow getting pauseparams")

    _, pause_support = get_local_pause_supported(cfg)
    for lm in linkmodes:
        if lm in pause_support:
            return

    raise KsftSkipEx(f"Local device doesn't support any of {linkmodes}")

def require_pause_supported_allof(cfg, linkmodes) -> None:
    """ Checks if the local device supports the passed linkmodes

    :param cfg: test config
    :param linkmodes: modes to test
    """
    ret, _ = get_local_pauseparams(cfg)
    if ret != 0:
        raise KsftSkipEx("device doesn't allow getting pauseparams")

    _, pause_support = get_local_pause_supported(cfg)
    for lm in linkmodes:
        if lm not in pause_support:
            raise KsftSkipEx(f"Local device doesn't support {lm}")

def require_peer_pause_supported_anyof(cfg, linkmodes) -> None:
    """ Checks if the remote device supports the passed linkmodes
    """

    ret, _ = get_peer_pauseparams(cfg)
    if ret != 0:
        raise KsftSkipEx("Remote device doesn't allow getting pauseparams")

    _, pause_support = get_peer_pause_supported(cfg)
    for lm in linkmodes:
        if lm in pause_support:
            return

    raise KsftSkipEx(f"Local device doesn't support any of {linkmodes}")

def require_peer_pause_supported_allof(cfg, linkmodes) -> None:
    """ Checks if the remote device supports the passed linkmodes
    """

    ret, _ = get_peer_pauseparams(cfg)
    if ret != 0:
        raise KsftSkipEx("Remote device doesn't allow getting pauseparams")

    _, pause_support = get_peer_pause_supported(cfg)
    for lm in linkmodes:
        if lm not in pause_support:
            raise KsftSkipEx(f"Remote device doesn't support {lm}")

def supported_pauseparams(cfg) -> tuple[int, int]:
    """ The rx/tx params covering every mode the local device supports """
    _, pause_support = get_local_pause_supported(cfg)
    if "Pause" in pause_support:
        return 1, 1
    if "Asym_Pause" in pause_support:
        return 0, 1

    raise KsftSkipEx("Local device doesn't support pause")

def set_local_pause_autoneg(cfg, aneg) -> int:
    """ Enable or disable pause autoneg """
    ret, _ = ethtool_ret(f"-A {cfg.ifname} autoneg {onoff(aneg)}",
                         is_get=False)
    return ret

def check_local_pauseparams(cfg, aneg, rx, tx) -> None:
    """ Validates that the pauseparams reported from ethtool -a are
        exactly the 3 passed parameters.
    """
    ret, params = get_local_pauseparams(cfg)
    ksft_eq(ret, 0)
    if ret != 0:
        return

    ksft_eq(params["autonegotiate"], bool(aneg), "pause autoneg")
    ksft_eq(params["rx"], bool(rx), "rx pause")
    ksft_eq(params["tx"], bool(tx), "tx pause")

def check_local_advertising(cfg, linkmodes) -> None:
    """ Verify that the local device advertises exactly the pause modes
        passed as parameters
    """
    _, adv = get_local_pause_advertising(cfg)
    ksft_eq(adv, linkmodes, "advertised pause modes")

def check_local_lp_advertising(cfg, linkmodes) -> None:
    """ Verify that the local device reports exactly the lp_advertised pause
        modes passed as parameters
    """

    ret, adv = get_local_pause_lp_advertising(cfg)
    if ret == 0:
        ksft_eq(adv, linkmodes, "link partner advertised pause modes")

def expect_pauseparams_set(ret, linkmodes, supported, note) -> None:
    """ Whether ethtool -A had to work or to be refused, given what the local
        device supports

    :param ret: return value from ethtool -A
    :param linkmodes: Pause / Asym modes corresponding to the set pause params
    :param supported: Supported Pause / Asym
    :param note: Message to print
    """
    # If supported is empty, ethtool -A must return -EOPNOTSUPP
    if not supported :
        ksft_eq(ret, errno.EOPNOTSUPP, note)
    elif set(linkmodes).issubset(set(supported)) :
        # The configured pauseparams are supposed to be supported,
        #ethtool -A must have worked.
        ksft_eq(ret, 0, note)
    else :
        # We tried to configure parameters that aren't supporteed,
        # ethtool -A must have failed.
        ksft_in(ret, (errno.EOPNOTSUPP, errno.EINVAL), note)

def pause_setup(cfg) -> None:
    """ The starting conditions every test here counts on, restored on exit:
        - both ports admin up
        - link autoneg on on both sides ifsupported
        - link actually up (carrier on)

    :param cfg: test config
    """

    # Get init pause parameters
    ret, params = ethtool_ret(f"-a {cfg.ifname}")
    if ret == 0:
        defer(cmd, f"ethtool -A {cfg.ifname} rx {onoff(params['rx'])} "
                   f"tx {onoff(params['tx'])} "
                   f"autoneg {onoff(params['autonegotiate'])}", fail=False)

    # Get init link parameters
    link = ethtool(f"{cfg.ifname}", json=True)[0]
    if link["auto-negotiation"]:
        defer(cmd, f"ethtool -s {cfg.ifname} autoneg on", fail=False)
    elif "speed" in link and "duplex" in link:
        defer(cmd, f"ethtool -s {cfg.ifname} autoneg off speed {link['speed']} "
                   f"duplex {link['duplex'].lower()}", fail=False)

    # Local interface admin up, link aneg on
    ip(f"link set {cfg.ifname} up")
    if link["supports-auto-negotiation"] and not link["auto-negotiation"]:
        ethtool(f"-s {cfg.ifname} autoneg on")

    # Get remote pause params
    ret, params = ethtool_ret(f"-a {cfg.remote_ifname}", host=cfg.remote)
    if ret == 0:
        defer(cmd, f"ethtool -A {cfg.remote_ifname} rx {onoff(params['rx'])} "
                   f"tx {onoff(params['tx'])} "
                   f"autoneg {onoff(params['autonegotiate'])}",
              fail=False, host=cfg.remote)

    # Get remote link params
    link = ethtool(f"{cfg.remote_ifname}", json=True, host=cfg.remote)[0]
    if link["auto-negotiation"]:
        defer(cmd, f"ethtool -s {cfg.remote_ifname} autoneg on", fail=False,
              host=cfg.remote)
    elif "speed" in link and "duplex" in link:
        defer(cmd, f"ethtool -s {cfg.remote_ifname} autoneg off "
                   f"speed {link['speed']} duplex {link['duplex'].lower()}",
              fail=False, host=cfg.remote)

    # Remote interface admin up, link aneg on
    ip(f"link set {cfg.remote_ifname} up", host=cfg.remote)
    if link["supports-auto-negotiation"] and not link["auto-negotiation"]:
        ethtool(f"-s {cfg.remote_ifname} autoneg on", host=cfg.remote)

    # Wait for link to become up on both ends
    if not wait_for_link(cfg):
        raise KsftFailEx("No link before the test")

# Pause support : Supported linkmodes vs ability to set/get pauseparams
@ksft_variants(pauseparams_variants)
@ksft_disruptive
def pause_test_support(cfg, pauseparams) -> None:
    """ Verify that the supported linkmodes Pause and Asym_Pause match the
        ability to configure the rx and tx pauseparams.

    Drivers are expected to reject pauseparams they don't support, and
    accept the ones they support. The supported modes are exposed by
    the MAC to the PHY layer through phylink mac_capabilities MAC_SYM_PAUSE
    and MAC_ASYM_PAUSE, or through phylib directly with the
    phy_support_sym_pause() and phy_support_asym_pause() helpers.

    The expectation is for drivers to refuse setting pauseparams that don't
    match the Pause and Asym_Pause bits in the supported linkmodes with a
    -EOPNOTSUPP return value. Unsupported pause params must be rejected.

    Failing this test likely means the MAC driver doesn't implement the
    set/get_pauseparam, but still sets flow control as supported through
    phylink mac_capabilities or phylib's pause API. Conversely, the MAC driver
    may have omitted to indicate its supported Pause modes. Finally, the PHY
    driver may incorrectly override the Pause and Asym_Pause bits in its
    supported fields.

    The sequence runs with link autoneg on, then with the link forced
    (ethtool -s ethX autoneg off): the pause params are accepted or rejected
    the same way in both cases, and both with pause autoneg off and on.
    """
    rx = onoff(pauseparams["rx"])
    tx = onoff(pauseparams["tx"])
    linkmodes = pauseparams["linkmodes"]

    pause_setup(cfg)

    forced = forced_link_settings(cfg)
    _, supported = get_local_pause_supported(cfg)

    # We check that what we can configure in the pause params matches what we
    # support under various contditions : Link aneg on/off, pause aneg on/off
    ret, _ = ethtool_ret(f"-s {cfg.ifname} autoneg on", is_get = False)
    if ret != 0:
        ksft_pr("link autoneg on refused, not tested")
    else:
        ret, _ = ethtool_ret(f"-A {cfg.ifname} rx {rx} tx {tx} autoneg off",
                             is_get = False)
        expect_pauseparams_set(ret, linkmodes, supported, "link autoneg on")

        ret, _ = ethtool_ret(f"-A {cfg.ifname} rx {rx} tx {tx} autoneg on",
                             is_get = False)
        expect_pauseparams_set(ret, linkmodes, supported, "link autoneg on")

    if not forced:
        ksft_pr("link speed unknown, the forced link is not tested")
        return

    ret, _ = ethtool_ret(f"-s {cfg.ifname} autoneg off {forced}", is_get = False)
    if ret != 0:
        ksft_pr(f"link autoneg off {forced} refused, not tested")
        return

    ret, _ = ethtool_ret(f"-A {cfg.ifname} rx {rx} tx {tx} autoneg off",
                         is_get = False)
    expect_pauseparams_set(ret, linkmodes, supported, "link autoneg off")

    ret, _ = ethtool_ret(f"-A {cfg.ifname} rx {rx} tx {tx} autoneg on",
                         is_get = False)
    expect_pauseparams_set(ret, linkmodes, supported, "link autoneg off")

@ksft_variants(pauseparams_variants)
@ksft_disruptive
def pause_advertising_test(cfg, pauseparams) -> None:
    """Pause advertisement

    Validate that changing pause params through the ETHTOOL_MSG_PAUSE command
    translates to a change in the advertised pause params, and that these
    parameters are correct w.r.t the supported pause params and requested pause
    params.

    This exercises the .set_pauseparam() ethtool ops for MAC configuration,
    as well as the reconfiguration of the PHY's advertising and negotiation.

    On non-phylink MACs, the MAC should call phy_set_sym_pause() to update the
    PHY's advertising, and restart a negotiation with phy_start_aneg() if
    need be. Failure to do so will result in the wrong advertising parameters.

    On phylink-enabled MACs, phylink deals with the PHY reconfiguration provided
    the MAC driver calls phylink_ethtool_set_pauseparam().

    Failing this test likely means that the PHY driver is not correctly
    advertising pause settings, either due to the MAC not triggering a PHY
    reconfiguration, a misconfiguration of the advertising registers by the PHY,
    or by mis-handling the phydev->advertising bitmap in the PHY driver directly.

    The validation is made by looking at the advertised modes locally, as well
    as what the peer's 'lp_advertising' values report.
    """

    require_pause_supported_allof(cfg, pauseparams["linkmodes"])
    pause_setup(cfg)
    lp = controllable_lp(cfg)

    tx = pauseparams["tx"]
    rx = pauseparams["rx"]
    adv = pauseparams["linkmodes"]
    not_adv = [ l for l in ["Pause", "Asym_Pause"] if l not in adv]

    # It's OK to skip here, we're already validating the EOPNOTSUPP behaviour
    # the pause_test_support test.
    ret = set_local_pauseparams(cfg, rx, tx, True)
    if ret == errno.EOPNOTSUPP:
        raise KsftSkipEx(f"RX {rx} TX {tx} not supported")

    # Wait for link parameters to re-negotiate and link to come back up. It must
    # come back up, otherwise that means changing pauseparams can bring the
    # link down.
    ret = wait_for_aneg(cfg)
    ksft_eq(ret, True)

    _, linkmodes = get_local_pause_advertising(cfg)
    for mode in adv:
        ksft_in(mode, linkmodes,
                f"rx {rx} tx {tx} aneg on must advertise {adv}")

    for mode in not_adv:
        ksft_not_in(mode, linkmodes,
                    f"rx {rx} tx {tx} aneg on must not advertise {not_adv}")

    if not lp:
        return

    returncode, remote_linkmodes = get_peer_pause_lp_advertising(cfg)
    if returncode == errno.EOPNOTSUPP:
        return

    for mode in adv:
        ksft_in(mode, remote_linkmodes, f"PHY does not advertise {adv}")

    for mode in not_adv:
        ksft_not_in(mode, remote_linkmodes,
                    f"PHY incorrectly advertises {not_adv}")


# Pause autonegotiation resolution : Resolved pause settings vs configured
# pauseparams on local device and link partner
@ksft_variants([
    # We advertise nothing, all off
    KsftNamedVariant("local rx off tx off, remote rx off tx off",
        {"rx": 0, "tx": 0, "lp_rx": 0, "lp_tx": 0, "neg_rx": 0, "neg_tx": 0}),

    # We advertise nothing, all off
    KsftNamedVariant("local rx off tx off, remote rx off tx on",
        {"rx": 0, "tx": 0, "lp_rx": 0, "lp_tx": 1, "neg_rx": 0, "neg_tx": 0}),

    # We advertise nothing, all off
    KsftNamedVariant("local rx off tx off, remote rx on tx off",
        {"rx": 0, "tx": 0, "lp_rx": 1, "lp_tx": 0, "neg_rx": 0, "neg_tx": 0}),

    # We advertise nothing, all off
    KsftNamedVariant("local rx off tx off, remote rx on tx on",
        {"rx": 0, "tx": 0, "lp_rx": 1, "lp_tx": 1, "neg_rx": 0, "neg_tx": 0}),

    # LP advertises nothing, all off
    KsftNamedVariant("local rx off tx on, remote rx off tx off",
        {"rx": 0, "tx": 1, "lp_rx": 0, "lp_tx": 0, "neg_rx": 0, "neg_tx": 0}),

    # We advertise Asym, LP advertises Asym, all off
    KsftNamedVariant("local rx off tx on, remote rx off tx on",
        {"rx": 0, "tx": 1, "lp_rx": 0, "lp_tx": 1, "neg_rx": 0, "neg_tx": 0}),

    # We advertise Asym, LP advertises Pause + Asym, tx on
    KsftNamedVariant("local rx off tx on, remote rx on tx off",
        {"rx": 0, "tx": 1, "lp_rx": 1, "lp_tx": 0, "neg_rx": 0, "neg_tx": 1}),

    # Tricky case :
    # We advertise Asym, LP advertises Pause, resolves to all off
    KsftNamedVariant("local rx off tx on, remote rx on tx on",
        {"rx": 0, "tx": 1, "lp_rx": 1, "lp_tx": 1, "neg_rx": 0, "neg_tx": 0}),

    # LP advertises nothing, all off
    KsftNamedVariant("local rx on tx off, remote rx off tx off",
        {"rx": 1, "tx": 0, "lp_rx": 0, "lp_tx": 0, "neg_rx": 0, "neg_tx": 0}),

    # We advertise Pause + Asym , LP advertises Asym, rx on
    KsftNamedVariant("local rx on tx off, remote rx off tx on",
        {"rx": 1, "tx": 0, "lp_rx": 0, "lp_tx": 1, "neg_rx": 1, "neg_tx": 0}),

    # Also tricky: Only rx enabled on both ends, but we negotiate rx/tx
    # We advertise Pause + Asym, LP advertises Pause + Asym, all on
    KsftNamedVariant("local rx on tx off, remote rx on tx off",
        {"rx": 1, "tx": 0, "lp_rx": 1, "lp_tx": 0, "neg_rx": 1, "neg_tx": 1}),

    # We advertise Pause + Asym, LP advertises Pause, all on
    KsftNamedVariant("local rx on tx off, remote rx on tx on",
        {"rx": 1, "tx": 0, "lp_rx": 1, "lp_tx": 1, "neg_rx": 1, "neg_tx": 1}),

    # LP advertises nothing, all off
    KsftNamedVariant("local rx on tx on, remote rx off tx off",
        {"rx": 1, "tx": 1, "lp_rx": 0, "lp_tx": 0, "neg_rx": 0, "neg_tx": 0}),

    # Tricky case :
    # We advertise Pause, LP advertises Asym, resolves to all off
    KsftNamedVariant("local rx on tx on, remote rx off tx on",
        {"rx": 1, "tx": 1, "lp_rx": 0, "lp_tx": 1, "neg_rx": 0, "neg_tx": 0}),

    # We advertise Pause, LP advertises Pause + Asym, all on
    KsftNamedVariant("local rx on tx on, remote rx on tx off",
        {"rx": 1, "tx": 1, "lp_rx": 1, "lp_tx": 0, "neg_rx": 1, "neg_tx": 1}),

    # We advertise Pause, LP advertises Pause, all on
    KsftNamedVariant("local rx on tx on, remote rx on tx on",
        {"rx": 1, "tx": 1, "lp_rx": 1, "lp_tx": 1, "neg_rx": 1, "neg_tx": 1}),
])
@ksft_disruptive
def pause_aneg_resolution(cfg, settings) -> None:
    """ Verify that rx and tx pause parameters are negotiated according to 802.3

    802.3 dictates the rules for pause negotiation, all 16 cases are tested, one
    for each combination of Pause and Asym_Pause advertising on the local device
    and the link-partner.

    This test also verifies that the peer resolved the parameters correctly,
    to ensure the negotiation is triggered correctly.

    Failing this test can happen if :
     - The MAC accepts the pause parameters but doesn't trigger a link
       renegotiation
     - that the PHY driver manually overwrites the Pause negotiation result
     - that the MAC driver ignores the Pause resolution and sets its own
       pause parameters regardless
    """
    expected_local_rx = settings["neg_rx"]
    expected_local_tx = settings["neg_tx"]

    required_local_linkmodes = pause_to_linkmodes(settings["rx"],
                                                  settings["tx"])
    required_remote_linkmodes = pause_to_linkmodes(settings["lp_rx"],
                                                   settings["lp_tx"])

    require_pause_supported_allof(cfg, required_local_linkmodes)
    require_peer_pause_supported_allof(cfg, required_remote_linkmodes)
    require_controllable_lp(cfg)
    pause_setup(cfg)

    # There's symmetry between local device and LP on pause negotiation:
    # - if local resolves all off or all on, LP must resolve the same
    # - if local resolves RX only, remote must resolve to TX only
    # - if local resolves TX only, remote must resolve to RX only
    if expected_local_rx == expected_local_tx:
        expected_lp_rx = expected_local_rx
        expected_lp_tx = expected_local_tx
    else:
        expected_lp_rx = expected_local_tx
        expected_lp_tx = expected_local_rx

    # Set pauseparams
    ret = set_local_pauseparams(cfg, settings["rx"], settings["tx"], True)
    if ret == errno.EOPNOTSUPP:
        raise KsftSkipEx(f"RX {settings['rx']} TX {settings['tx']} not supported")

    ksft_eq(wait_for_aneg(cfg), True)

    set_peer_pauseparams(cfg, settings["lp_rx"], settings["lp_tx"], True)

    # Wait for link to re-negotiate
    ret = wait_for_aneg(cfg)

    # Fail if it doesn't
    ksft_eq(ret, True)

    if get_local_pause_lp_advertising(cfg)[0] != 0:
        raise KsftSkipEx("Local device doesn't report the LP's advertising")

    ret, local_pauseparams = get_local_pauseparams(cfg)
    if ret != 0 or "negotiated" not in local_pauseparams:
        raise KsftSkipEx("Local device doesn't report the negotiated pause params")

    # check adv
    _, linkmodes = get_local_pause_advertising(cfg)
    for mode in required_local_linkmodes:
        ksft_in(mode, linkmodes,
                f"local rx {settings['rx']} tx {settings['tx']} must advertise "
                f"{required_local_linkmodes}")

    _, linkmodes = get_peer_pause_advertising(cfg)
    for mode in required_remote_linkmodes:
        ksft_in(mode, linkmodes,
                f"remote rx {settings['lp_rx']} tx {settings['lp_tx']} must advertise "
                f"{required_remote_linkmodes}")

    # check lp_adv if available
    _, linkmodes = get_local_pause_lp_advertising(cfg)
    for mode in required_remote_linkmodes:
        ksft_in(mode, linkmodes,
                f"local lp_adv must show the remote's {required_remote_linkmodes}")

    # check lp_adv on remote
    ret, linkmodes = get_peer_pause_lp_advertising(cfg)
    if ret == 0:
        for mode in required_local_linkmodes:
            ksft_in(mode, linkmodes,
                    f"remote lp_adv must show our {required_local_linkmodes}")

    # Check resolution
    _, local_pauseparams = get_local_pauseparams(cfg)
    ksft_eq(local_pauseparams["negotiated"]["rx"], expected_local_rx)
    ksft_eq(local_pauseparams["negotiated"]["tx"], expected_local_tx)

    ret, remote_pauseparams = get_peer_pauseparams(cfg)
    if ret == 0 and "negotiated" in remote_pauseparams:
        ksft_eq(remote_pauseparams["negotiated"]["rx"], expected_lp_rx)
        ksft_eq(remote_pauseparams["negotiated"]["tx"], expected_lp_tx)

# Pause autoneg enable/disable vs advertised linkmodes
@ksft_disruptive
def pause_autoneg_state_adv(cfg) -> None:
    """Validate that toggling pause advertising changes the advertised linkmodes

    When disabling pause autoneg, we enforce the pause params based on what user
    asks, instead of relying on the negociation process (which may not be what
    the user asked for). In forced pause settings, we don't advertise pause and
    asym_pause bits.

    Failing this test means that .set_pauseparam in the MAC driver doesn't
    forward to the PHY (in charge of advertising these bits) that we are in
    fixed pause mode.
    """

    require_pause_supported_anyof(cfg, ["Pause", "Asym_Pause"])
    require_controllable_lp(cfg)
    pause_setup(cfg)

    set_peer_pauseparams(cfg, True, True, True)
    ksft_eq(wait_for_aneg(cfg), True)

    rx, tx = supported_pauseparams(cfg)

    # Enable all possible pauseparams with pause autoneg
    ret = set_local_pauseparams(cfg, rx, tx, True)
    ksft_eq(ret, 0)
    ksft_eq(wait_for_aneg(cfg), True)

    # Make sure we advertise them
    ret, adv = get_local_pause_advertising(cfg)
    ksft_eq(ret, 0)
    ksft_eq(adv, pause_to_linkmodes(rx, tx))

    # Disable pause autoneg
    ret = set_local_pauseparams(cfg, rx, tx, False)
    ksft_eq(ret, 0)

    # This may trigger a link renegociation
    ksft_eq(wait_for_aneg(cfg), True)

    # We shouldn't be advertising anything anymore
    ret, adv = get_local_pause_advertising(cfg)
    ksft_eq(ret, 0)
    ksft_eq(adv, [])

    # Validate on the LP that we aren't advertising anything
    ret, adv = get_peer_pause_lp_advertising(cfg)
    if ret == errno.EOPNOTSUPP:
        return

    ksft_eq(ret, 0)
    ksft_eq(adv, [])

# Pause autoneg : Negotiated pause params vs fixed pause params
@ksft_disruptive
def pause_autoneg_state_params(cfg) -> None:
    """Validate the pause params when transitioning between fixed pause
       params and negotiated ones. The goal is to make sure that user
       intent on the RX and TX pause params are stored when user decides
       to use negotiated parameters instead. The main gotcha lies on the
       fact that when pause autoneg is used, the autoneg result may differ
       from the user intent.

    Failing this test means the MAC driver is overwriting the user intent
    when switching to forced pause.
    """

    require_pause_supported_allof(cfg, ["Pause", "Asym_Pause"])
    require_controllable_lp(cfg)
    require_peer_pause_supported_allof(cfg, ["Pause"])
    require_link_autoneg(cfg)
    pause_setup(cfg)

    # Set peer user intent to RX on TX on, with Pause autoneg on
    set_peer_pauseparams(cfg, 1, 1, True)
    ksft_eq(wait_for_aneg(cfg), True)

    # Set the local intent to RX on TX off with pause autoneg
    ksft_eq(set_local_pauseparams(cfg, 1, 0, True), 0)
    ksft_eq(wait_for_aneg(cfg), True)

    check_local_pauseparams(cfg, True, 1, 0)
    # Peer advertisiong Pause + Asym and us advertising Pause means we are
    # actually using RX on TX on here, which is different than the intent.
    check_local_advertising(cfg, ["Pause", "Asym_Pause"])
    check_local_lp_advertising(cfg, ["Pause"])

    # Disable pause autoneg
    ksft_eq(set_local_pause_autoneg(cfg, False), 0)
    ksft_eq(wait_for_aneg(cfg), True)

    # The pauseparams must still be what we configured before, and not the
    # previously negotiated ones
    check_local_pauseparams(cfg, False, 1, 0)

    # Re-enable autoneg
    ksft_eq(set_local_pause_autoneg(cfg, True), 0)
    ksft_eq(wait_for_aneg(cfg), True)

    check_local_pauseparams(cfg, True, 1, 0)
    # We must be advertising our intent again, and not RX on TX on, which would
    # be "Pause" only.
    check_local_advertising(cfg, ["Pause", "Asym_Pause"])
    check_local_lp_advertising(cfg, ["Pause"])

# Pause autoneg vs Link autoneg

@ksft_disruptive
def pause_autoneg_off_while_link_autoneg_on(cfg) -> None:
    """ Validate that when link autoneg is on but pause autoneg is off, we do
        not use negotiated pause parameters.

        - Skip if pause not supported.

        - Requirements :
        - Link partner: link up, link autoneg on, pause autoneg on,
          pause tx and rx on
        - Local device starting conditions : link on, link autoneg on,
          pause autoneg on, pause tx <on if supported> rx <on if supported>

        Failing this test means the MAC driver incorrectly accounts for the
        negotiated pause parameters even with pause aneg off, likely due to
        confusion between link autoneg and pause autoneg.
    """
    require_pause_supported_anyof(cfg, ["Pause", "Asym_Pause"])
    require_controllable_lp(cfg)
    require_peer_pause_supported_allof(cfg, ["Pause"])
    require_link_autoneg(cfg)
    pause_setup(cfg)

    rx, tx = supported_pauseparams(cfg)

    # Enable pause autoneg with all the locally supported modes enabled
    set_peer_pauseparams(cfg, 1, 1, True)
    ksft_eq(set_local_pauseparams(cfg, rx, tx, True), 0)
    ksft_eq(wait_for_aneg(cfg), True)

    # Disable Pause autoneg
    ksft_eq(set_local_pauseparams(cfg, rx, tx, False), 0)
    ksft_eq(wait_for_aneg(cfg), True)
    # Pause autoneg must read "disabled"
    check_local_pauseparams(cfg, False, rx, tx)

    set_peer_pauseparams(cfg, 0, 0, True)
    ksft_eq(wait_for_aneg(cfg, link_drop=True), True)

    ip(f"link set {cfg.remote_ifname} down", host=cfg.remote)
    ip(f"link set {cfg.remote_ifname} up", host=cfg.remote)
    ksft_eq(wait_for_aneg(cfg, link_drop=True), True)

    # Pause autoneg must still be off even after a link renegotiation
    check_local_pauseparams(cfg, False, rx, tx)

@ksft_disruptive
def pause_autoneg_link_autoneg(cfg) -> None:
    """Validate pause autoneg and link autoneg interactions. The link autoneg's
       admin status (i.e. do we autoneg link parameters or force them) must not
       impact the pause autoneg status. While link autoneg is disabled, we don't
       negotiate the pause params, however we must keep pause autoneg on as this
       is the user intent. When link autoneg is re-enabled, pause params must be
       derived from the negotiation.

    Both ends are forced at the speed and duplex the link runs at. A link that
    does not come back forced is a skip, not a verdict: 1000BASE-T resolves
    master/slave through autoneg and rarely links without it.
    """

    require_pause_supported_anyof(cfg, ["Pause", "Asym_Pause"])
    require_controllable_lp(cfg)
    require_peer_pause_supported_allof(cfg, ["Pause"])
    require_link_autoneg(cfg)
    pause_setup(cfg)

    rx, tx = supported_pauseparams(cfg)
    adv = pause_to_linkmodes(rx, tx)

    # Enable all possible pause modes and autoneg
    set_peer_pauseparams(cfg, 1, 1, True)
    ksft_eq(set_local_pauseparams(cfg, rx, tx, True), 0)
    ksft_eq(wait_for_aneg(cfg), True)

    check_local_pauseparams(cfg, True, rx, tx)
    check_local_advertising(cfg, adv)
    check_local_lp_advertising(cfg, ["Pause"])

    # Disable link autoneg, at the speed and duplex the link runs at
    forced = forced_link_settings(cfg)
    if not forced:
        raise KsftSkipEx("Can't tell what to force the link at")

    ret, _ = ethtool_ret(f"-s {cfg.remote_ifname} autoneg off {forced}",
                         is_get=False, host=cfg.remote)
    if ret != 0:
        raise KsftSkipEx(f"Can't force the peer's link at {forced}")

    ret, _ = ethtool_ret(f"-s {cfg.ifname} autoneg off {forced}",
                         is_get=False)
    if ret != 0:
        raise KsftSkipEx(f"Can't force the link at {forced}")

    if not wait_for_aneg(cfg):
        raise KsftSkipEx(f"No link when forced at {forced}")

    # We must have pause autoneg still enabled, even if we don't negotiate pause
    check_local_pauseparams(cfg, True, rx, tx)

    # Re-enable autoneg
    ethtool(f"-s {cfg.remote_ifname} autoneg on", host=cfg.remote)
    ethtool(f"-s {cfg.ifname} autoneg on")
    ksft_eq(wait_for_aneg(cfg), True)

    # Pause autoneg must still be on
    check_local_pauseparams(cfg, True, rx, tx)
    check_local_advertising(cfg, adv)
    check_local_lp_advertising(cfg, ["Pause"])

def main() -> None:
    """ The hardware pause tests, on the interface the env names """
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        ksft_run([pause_test_support,
                  pause_advertising_test,
                  pause_aneg_resolution,
                  pause_autoneg_state_adv,
                  pause_autoneg_state_params,
                  pause_autoneg_off_while_link_autoneg_on,
                  pause_autoneg_link_autoneg,
                  ],
                 args=(cfg, ))
    ksft_exit()

if __name__ == "__main__":
    main()

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


def main() -> None:
    """ The hardware pause tests, on the interface the env names """
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        ksft_run([pause_test_support,
                  pause_advertising_test,
                  ],
                 args=(cfg, ))
    ksft_exit()

if __name__ == "__main__":
    main()

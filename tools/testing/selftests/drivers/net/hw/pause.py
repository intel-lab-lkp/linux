#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Driver-related behavior tests for Pause-based Flow Control.

Requires a controllable link-partner, with configurable autoneg parameters
as well as support for Pause and Asym_Pause.
"""

from lib.py import ksft_run, ksft_exit, ksft_eq, ksft_ge, ksft_in, ksft_not_in, ksft_true
from lib.py import ksft_variants, KsftNamedVariant, KsftSkipEx, ksft_raises
from lib.py import defer, ethtool, CmdExitFailure
from lib.py import EthtoolFamily, NlError
from lib.py import NetDrvEpEnv
from lib.py import ip
import errno

# Linkmodes to Pause params :
# Pause bit is set if rx == 1
# Asym_Pause bit is set if rx != tx
pauseparams_to_linkmodes = [
    {"rx": 0, "tx": 0, "linkmodes": []},
    {"rx": 0, "tx": 1, "linkmodes": ["Asym_Pause"]},
    {"rx": 1, "tx": 0, "linkmodes": ["Pause", "Asym_Pause"]},
    {"rx": 1, "tx": 1, "linkmodes": ["Pause"]},
]

def pause_to_linkmodes(rx, tx) -> list[str]:
    """ Convert rx/tx pauseparams to the corresponding linkmodes
    """
    if rx:
        if tx:
            return pauseparams_to_linkmodes[3]["linkmodes"]
        else:
            return pauseparams_to_linkmodes[2]["linkmodes"]
    elif tx:
        return pauseparams_to_linkmodes[1]["linkmodes"]
    else:
        return pauseparams_to_linkmodes[0]["linkmodes"]

def set_local_pauseparams(cfg, rx, tx, aneg) -> int:
    """ set pauseparams : ethtool -A

    Raise an error if the return is not 0 or EOPNOTSUPP

    :param cfg: test config
    :param rx: rx pause enabled or disabled
    :param tx: tx pause enabled or disabled
    :param aneg: pause autoneg enabled or disabled
    :returns: return code of the ethtool command
    """
    #TODO
    raise KsftSkipEx("Not implemented")

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
    #TODO
    raise KsftSkipEx("Not implemented")

def set_peer_pauseparams(cfg, rx, tx, aneg) -> int:
    """ set pauseparams on the linkpartner

    Raise an error if the return is not 0 or EOPNOTSUPP
    returns ENODEV if there's no LP

    :param cfg: test config
    :param rx: rx pause enabled or disabled
    :param tx: tx pause enabled or disabled
    :param aneg: pause autoneg enabled or disabled
    :returns: return code of the ethtool command
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def get_peer_pauseparams(cfg) -> tuple[int, bool, bool, bool]:
    """ get pauseparams on the link partner

    Raise an error if the return is not 0 or EOPNOTSUPP
    returns ENODEV if there's no LP

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command,
              - rx status,
              - tx status,
              - aneg status
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def get_local_supported(cfg) -> tuple[int, list[str]]:
    """ get the supported linkmodes on the local device

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def get_local_advertising(cfg) -> tuple[int, list[str]]:
    """ get the advertised linkmodes on the local device

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def get_local_lp_advertising(cfg) -> tuple[int, list[str]]:
    """ get the lp_advertised linkmodes on the local device

    Raise an error if the return is not 0 or EOPNOTSUPP
    Prints a warning if the local device doesn't report lp_advertising

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def get_peer_supported(cfg) -> tuple[int, list[str]]:
    """ get the supported linkmodes on the link partner

    returns ENODEV if there's no LP

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def get_peer_advertising(cfg) -> tuple[int, list[str]]:
    """ get the advertised linkmodes on the link partner

    returns ENODEV if there's no LP

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def get_peer_lp_advertising(cfg) -> tuple[int, list[str]]:
    """ get the lp_advertised linkmodes on the link partner

    Raise an error if the return is not 0 or EOPNOTSUPP
    returns ENODEV if there's no LP
    Prints a warning if LP is present but doesn't report lp_advertising

    :param cfg: test config
    :returns: tuple containing :
              - return code of the ethtool command
              - list of linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def wait_for_link(cfg) -> bool:
    """ Wait for both ends of the link to be up.

    :param cfg: test config
    :returns: True if link is UP, False if timeout
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def require_controllable_lp(cfg) -> None:
    """ Checks that the link-partner is a real device we can control

    Raise a skip if the linkpartner is a netdevsim or veth. We need a real
    link-partner for some tests.

    :param cfg: test config
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def require_pause_supported(cfg) -> None:
    """ Checks if the local device supports pause at all, by looking at the
        supported bitfield. Raises a skip it's not supported.

        :param cfg: test config
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def require_supported(cfg, linkmodes) -> None:
    """ Checks if the local device supports the passed linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")

def require_peer_supported(cfg, linkmodes) -> None:
    """ Checks if the remote device supports the passed linkmodes
    """
    #TODO
    raise KsftSkipEx("Not implemented")


# Pause support : Supported linkmodes vs ability to set/get pauseparams
@ksft_variants([
    KsftNamedVariant("RX off TX off", pauseparams_to_linkmodes[0]),
    KsftNamedVariant("RX off TX on", pauseparams_to_linkmodes[1]),
    KsftNamedVariant("RX on TX off", pauseparams_to_linkmodes[2]),
    KsftNamedVariant("RX on TX on", pauseparams_to_linkmodes[3]),
])
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
    driver may incorrectly override the Pause and Asym_Pause bits in its supported
    fields.
    """

    rx = "on" if pauseparams["rx"] else "off"
    tx = "on" if pauseparams["tx"] else "off"
    linkmodes = pauseparams["linkmodes"]

    # Run ethtool {cfg.ifname} and extract the "Supported pause frame use"
    # Run ethtool -A {cfg.ifname} rx {rx} tx {tx} autoneg off, store the return code
    # Don't wait for the link to come back, this is only to validate local
    # settings
    #
    # If "supported" is empty, the return code must be EOPNOTSUPP
    # If "linkmodes" is a subset of "supported" then fail if the return code is
    # not zero.
    # If "linkmodes" is not a subset of "supported", then fail if the return code
    # is zero, the operation should have been rejected.

    # Run the same sequence again, but with pause autoneg on. The pass/fail
    # conditions remain the same.

    raise KsftSkipEx("Not implemented")

@ksft_variants([
    KsftNamedVariant("RX off TX off", pauseparams_to_linkmodes[0]),
    KsftNamedVariant("RX off TX on", pauseparams_to_linkmodes[1]),
    KsftNamedVariant("RX on TX off", pauseparams_to_linkmodes[2]),
    KsftNamedVariant("RX on TX on", pauseparams_to_linkmodes[3]),
])
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

    Failing this test likely means that the PHY driver is not correctly advertising
    pause settings, either due to the MAC not triggering a PHY reconfiguration,
    a misconfiguration of the advertising registers by the PHY, or by
    mis-handling the phydev->advertising bitmap in the PHY driver directly.

    The validation is made by looking at the advertised modes locally, as well as
    what the peer's 'lp_advertising' values report.

    initial state :
     - Link with LP established and UP
     - local device : link autoneg on
     - remote device : link autoneg on pause autoneg on
    """

    require_pause_supported(cfg)

    tx = pauseparams["tx"]
    rx = pauseparams["rx"]
    adv = pauseparams["linkmodes"]
    not_adv = {"Pause", "Asym_Pause"} - set(adv)

    # It's OK to skip here, we're already validating the EOPNOTSUPP behaviour
    # the pause_test_support test.
    ret = set_local_pauseparams(cfg, rx, tx, True)
    if ret == errno.EOPNOTSUPP:
        raise KsftSkipEx(f"RX {rx} TX {tx} not supported")

    # Wait for link parameters to re-negotiate and link to come back up. It must
    # come back up, otherwise that means changing pauseparams can bring the
    # link down.
    ret = wait_for_link(cfg)
    ksft_true(ret, "Link din't come back up after setting pauseparams")

    _, linkmodes = get_local_advertising(cfg)
    for mode in adv:
        ksft_in(mode, linkmodes, f"rx {rx} tx {tx} must advertise {adv}")

    for mode in not_adv:
        ksft_not_in(mode, linkmodes, f"rx {rx} tx {tx} must not advertise {not_adv}")

    # It's better to double-check on the LP that we are indeed correctly
    # advertising these modes, but if we don't have a controllable LP let's
    # just rely on what we say we're advertising
    returncode, remote_linkmodes = get_peer_lp_advertising(cfg)
    if returncode == errno.ENODEV:
        return

    for mode in adv:
        ksft_in(mode, remote_linkmodes, f"PHY does not advertise {adv}")

    for mode in not_adv:
        ksft_not_in(mode, remote_linkmodes, f"PHY incorrectly advertises {not_adv}")


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
def pause_aneg_resolution(cfg, settings) -> None:
    """ Verify that rx and tx pause parameters are negotiated according to 802.3

    802.3 dictates the rules for pause negotiation, all 16 cases are tested, one
    for each combination of Pause and Asym_Pause advertising on the local device
    and the link-partner.

    This test also verifies that the peer resolved the parameters correctly,
    to ensure the negotiation is triggered correctly.

    Failing this test can happen if :
     - The MAC accepts the pause parameters but doesn't trigger a link renegotiation
     - that the PHY driver manually overwrites the Pause negotiation result
     - that the MAC driver ignores the Pause resolution and sets its own
       pause parameters regardless

    Starting conditions :
      - link is established
      - peer device has link autoneg on, pause autoneg on
      - local device has link autoneg on, pause autoneg on
    """
    expected_local_rx = settings["neg_rx"]
    expected_local_tx = settings["neg_tx"]

    require_pause_supported(cfg)
    require_controllable_lp(cfg)

    required_local_linkmodes = pause_to_linkmodes(settings["rx"],
                                                  settings["tx"])
    required_remote_linkmodes = pause_to_linkmodes(settings["lp_rx"],
                                                   settings["lp_tx"])

    require_supported(cfg, required_local_linkmodes)
    require_peer_supported(cfg, required_remote_linkmodes)


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
    # Wait for link to re-negotiate
    # Fail if it doesn't
    # check adv
    # check lp_adv it available
    # check lp_adv on remote

    raise KsftSkipEx("Not implemented")

# Pause autoneg enable/disable vs advertised linkmodes
def pause_autoneg_state_adv(cfg) -> None:
    """Validate that toggling pause advertising changes the advertised linkmodes

    When disabling pause autoneg, we enforce the pause params based on what user
    asks, instead of relying on the negociation process (which may not be what
    the user asked for). In forced pause settings, we don't advertise pause and
    asym_pause bits.

    Failing this test means that .set_pauseparam in the MAC driver doesn't
    forward to the PHY (in charge of advertising these bits) that we are in
    fixed pause mode.

    - Local link is up, link autoneg enabled
    - Peer link is up, link autoneg enabled, pause autoneg enabled, rx on tx on.

    - Enable pause autoneg and pause tx and rx (or just rx or tx if Asym only):
      'ethtool -A ethX rx on tx on autoneg on'
    - Skip if returncode is EOPNOTSUPP
    - Fail if returncode is not 0
    - wait for link to re-establish, warn and skip if it doesn't

    - run 'ethtool ethX'
    - Fail if "Advertised pause frame use" is "No",

    - run 'ethtool -A ethX autoneg off'
    - Fail if the return isn't 0

    - run 'ethtool ethX'
    - Fail if "Advertised pause frame use" is not "No", 

    - On Link Partner, run :
    - ethtool ethY

    - Fail if "Link partner advertised pause frame use" is not "No".

    - Succeed otherwise
    """

    require_pause_supported(cfg)
    require_controllable_lp(cfg)

    raise KsftSkipEx("Not implemented")

# Pause autoneg : Negotiated pause params vs fixed pause params
def pause_autoneg_state_params(cfg) -> None:
    """Validate the pause params when transitioning between fixed pause
       params and negotiated ones. The goal is to make sure that user
       intent on the RX and TX pause params are stored when user decides
       to use negotiated parameters instead. The main gotcha lies on the
       fact that when pause autoneg is used, the autoneg result may differ
       from the user intent.

    Failing this test means the MAC driver is overwriting the user intent
    when switching to forced pause.

    - Initial conditions : Link is up and established, link autoneg is on,
    pause autoneg is on.
    - Peer link is up, link autoneg enabled, pause autoneg enabled, rx on tx on.

    - On local device, Set pause parameters to rx on tx off with autoneg :
    - ethtool -A ethX rx on tx off autoneg on
    - Skip if return is EOPNOTSUPP
    - Fail if return isn't 0
    - wait for link to re-establish, warn and skip if it doesn't

    - On Link Partner, Set pause parameters to rx on tx on with autoneg :
    - ethtool -A ethY rx on tx on autoneg on
    - Skip if return is EOPNOTSUPP
    - Fail if return isn't 0
    - wait for link to re-establish, warn and skip if it doesn't

    - On local device, show the current pause params:
    - ethtool -a ethX
    - Fail if results differ from :

      Pause parameters for <iface>:
      Autonegotiate: on
      RX:            on
      TX:            off

    - Read the current advertised and lp_advertised values
    - Fail if 'Advertised pause frame use' is not 'Pause + Asym_Pause'.
    - Fail if 'Link partner advertised pause frame use' is not 'Pause'.

    - Switch to fixed pause autoneg on local device:
    - run 'ethtool -A ethX autoneg off'

    - on local device, show the current pause params:
    - ethtool -a ethX
    - Fail if results differ from:

      Pause parameters for <iface>:
      Autonegotiate: off
      RX:            on
      TX:            off

    - Re-enable pause autonag on the local device:
    - run 'ethtool -A ethX autoneg on'
    - wait for link to re-establish, fail if it doesn't

    - on local device, show the current pause params:
    - ethtool -a ethX

    - Fail if results differ from :

      Pause parameters for <iface>:
      Autonegotiate: on
      RX:            on
      TX:            off

    - Read the current advertised and lp_advertised values
    - Fail if 'Advertised pause frame use' is not 'Pause + Asym_Pause'.
    - Fail if 'Link partner advertised pause frame use' is not 'Pause'.

    - Succeed otherwise
    """

    require_pause_supported(cfg)
    require_controllable_lp(cfg)

    raise KsftSkipEx("Not implemented")

# Pause autoneg vs Link autoneg

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
    # Disable pause autoneg, keep pause rx on tx on :
    # ethtool -A {cfg.ifname} rx <on if supp> tx <on if supp> autoneg off
    #
    # Disable pause on peer with pause aneg still on, we expect that not to
    # change the local device's forced pause values.
    # ethtool -A {cfg.remote_ifname} rx off tx off autoneg on
    # Flap the peer link
    # ip link set {cfg.remote_ifname} down
    # ip link set {cfg.remote_ifname} up
    #
    # wait for link to come back up, fail if it doesn't.
    #
    # Check the local pause parameters are unchanged :
    # ethtool -a {cfg.ifname}
    # fail if they are different than the ones set in the first step.

    require_pause_supported(cfg)
    require_controllable_lp(cfg)

    raise KsftSkipEx("Not implemented")

def pause_autoneg_link_autoneg(cfg) -> None:
    """Validate pause autoneg and link autoneg interactions. The link autoneg's
       admin status (i.e. do we autoneg link parameters or force them) must not
       impact the pause autoneg status. While link autoneg is disabled, we don't
       negotiate the pause params, however we must keep pause autoneg on as this
       is the user intent. When link autoneg is re-enabled, pause params must be
       derived from the negotiation.

    Failing this test means the MAC driver or the PHY driver incorrectly overwrites
    the pause autoneg admin status when link autoneg is changed.

    - Starting condition : Link up, link autoneg on
    - Peer link is up, link autoneg enabled, pause autoneg enabled, rx on tx on.

    - Set pauseparams to <All modes supported> and autoneg enabled :
    - run 'ethtool -A ethX rx <on if supported> tx <on if supported> autoneg on'
    - Skip if return is EOPNOTSUPP
    - wait for link to re-establish, warn and skip if it doesn't

    - Verify the pauseparams match the configured ones :
    - run 'ethtool -a ethX'
    - Fail if result isn't :
      Pause parameters for ethX:
      Autonegotiate:  on
      RX:             on
      TX:             on

    - Read the current advertised and lp_advertised values
    - Fail if 'Advertised pause frame use' is not 'Pause'.
    - Warn if no lp_adv is reported
    - Fail if 'Link partner advertised pause frame use' is not 'Pause'.

    - Disable link autoneg:
    - run 'ethtool -s ethX autoneg off'
    - Wait for the link to come back up, fail if it doesn't.

    - Verify the pauseparams :
    - run 'ethtool -a ethX'
    - Fail if result isn't :
      Pause parameters for ethX:
      Autonegotiate:  on
      RX:             on
      TX:             on

    - Re-enable link autoneg
    - run 'ethtool -s ethX autoneg on'
    - wait for link to re-establish, fail if it doesn't
    - Check the local pauseparams :
    - run 'ethtool -a ethX'

    - Fail if the result is not :
      Pause parameters for ethX:
      Autonegotiate:  on
      RX:             on
      TX:             on
    - Read the current advertised and lp_advertised values
    - Fail if 'Advertised pause frame use' is not 'Pause'.
    - Fail if 'Link partner advertised pause frame use' is not 'Pause'.
    """

    require_pause_supported(cfg)
    require_controllable_lp(cfg)

    raise KsftSkipEx("Not implemented")

def main() -> None:
    with NetDrvEpEnv(__file__) as cfg:
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

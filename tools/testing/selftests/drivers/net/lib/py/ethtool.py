# SPDX-License-Identifier: GPL-2.0

"""
Ethtool and link management helpers
"""

import errno
import json
import os
import time

from lib.py import cmd, ethtool

_strerrors = {os.strerror(e): e for e in errno.errorcode}

def ethtool_ret(command, is_get=True, host=None):
    """ Execute an ethtool command, returns the return code and JSON content

    :param command: the ethtool arguments
    :param is_get: Is the command a get or a set. Get commands return the loaded
                   JSON attributes
    :param host: The host on which to run the command on. None means local host.
    """
    json_flag = "--json" if is_get else ""
    cmd_res = cmd(f"ethtool {json_flag} {command}", fail=False, host=host)

    if cmd_res.ret != 0:
        # ethtool returns 1 upon error, not the netlink errcode. Try to get it
        # by parsing the stderr output, which looks like :
        # "netlink error: Operation not supported"
        for line in cmd_res.stderr.splitlines():
            err = _strerrors.get(line.rsplit(": ", 1)[-1].strip())
            if err:
                return err, None
        return cmd_res.ret, None

    # Not a get operation, we don't have any JSON output to parse
    if not is_get:
        return 0, None

    return 0, json.loads(cmd_res.stdout)[0]

def onoff(val) -> str:
    """ "on" or "off", the way ethtool spells a boolean """
    return "on" if val else "off"

def wait_for_aneg(cfg, link_drop=False, timeout=15) -> bool:
    """ Wait for a renegotiation to complete.

    :param cfg: test config
    :param link_drop: Set to true if the link HAS to flap.
    :returns: True if link is UP, False if timeout
    """
    deadline = time.monotonic() + timeout

    # Link has 2 seconds to come back up
    restart_by = time.monotonic() + 2

    # The link may still be up for a short while when we trigger an autoneg
    # restart, we need to wait for it to drop, then come back up again
    while time.monotonic() < deadline:
        if not ethtool(f"{cfg.ifname}", json=True)[0]["link-detected"]:
            return wait_for_link(cfg)

        if not link_drop and time.monotonic() > restart_by:
            return wait_for_link(cfg)

        time.sleep(0.1)

    return False

def wait_for_link_local(cfg, timeout=15) -> bool:
    """ Wait for the local link to be up.

    :param cfg: test config
    :returns: True if link is UP, False if timeout
    """
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        link = ethtool(f"{cfg.ifname}", json=True)[0]["link-detected"]
        if link:
            return True

        time.sleep(0.1)

    return False

def wait_for_link_remote(cfg, timeout=15) -> bool:
    """ Wait for the far end of the link to be up.

    :param cfg: test config
    :returns: True if link is UP, False if timeout
    """
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        link = ethtool(f"{cfg.remote_ifname}", json=True,
                       host=cfg.remote)[0]["link-detected"]
        if link:
            return True

        time.sleep(0.1)

    return False

def wait_for_link(cfg) -> bool:
    """ Wait for both ends of the link to be up.

    :param cfg: test config
    :returns: True if link is UP, False if timeout
    """
    if not wait_for_link_local(cfg):
        return False

    # Local link is UP, we shouldn't have to wait for a whole 8 seconds for
    # the remote to report link up, let's wait a bit less
    return wait_for_link_remote(cfg, timeout = 3)

def forced_link_settings(cfg) -> str:
    """ Returns a string to pass to ethtool -s with speed/duplex corresponding
        to the current settings.

        Note that some devices don't return duplex info, so assume full duplex
        in that case.

    :param cfg: test config
    """
    link = ethtool(f"{cfg.ifname}", json=True)[0]
    if "speed" not in link:
        return ""

    return f"speed {link['speed']} duplex {link.get('duplex', 'Full').lower()}"

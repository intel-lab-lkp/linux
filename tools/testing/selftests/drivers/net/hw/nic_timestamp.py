#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import datetime
import random
from lib.py import ksft_run, ksft_exit, ksft_ge, ksft_eq, KsftSkipEx
from lib.py import NetDrvEnv, EthtoolFamily, NlError

def _getHWTimeStampSupport(cfg):
    tsinfo = cfg.ethnl.tsinfo_get({'header': {'dev-name': cfg.ifname}})
    ctx = {}
    tx = tsinfo.get('tx-types', {})
    rx = tsinfo.get('rx-filters', {})

    bits = tx.get('bits', {})
    ctx['tx'] = bits.get('bit', [])
    bits = rx.get('bits', {})
    ctx['rx'] = bits.get('bit', [])
    return ctx

def _getHWTimeStampConfig(cfg):
    try:
        tscfg = cfg.ethnl.tsconfig_get({'header': {'dev-name': cfg.ifname}})
    except NlError as e:
        if e.error == errno.EOPNOTSUPP:
            raise KsftSkipEx("timestamping configuration is not supported via netlink") from e
        raise
    return tscfg

def _setHWTimeStampConfig(cfg, ts):
    ts['header'] = {'dev-name': cfg.ifname}
    try:
        res = cfg.ethnl.tsconfig_set(ts)
    except NlError as e:
	    if e.error == errno.EOPNOTSUPP:
	        raise KsftSkipEx("timestamping configuration is not supported via netlink") from e
	    raise
    return res

def test_hwtstamp_tx(cfg):
    orig_tscfg = _getHWTimeStampConfig(cfg)
    ts = _getHWTimeStampSupport(cfg)
    tx = ts['tx']
    for t in tx:
        tscfg = orig_tscfg
        tscfg['tx-types']['bits']['bit'] = [t]
        res = _setHWTimeStampConfig(cfg, tscfg)
        ksft_eq(res['tx-types']['bits']['bit'], [t])
    _setHWTimeStampConfig(cfg, orig_tscfg)

def test_hwtstamp_rx(cfg):
    orig_tscfg = _getHWTimeStampConfig(cfg)
    ts = _getHWTimeStampSupport(cfg)
    rx = ts['rx']
    for r in rx:
        tscfg = orig_tscfg
        tscfg['rx-filters']['bits']['bit'] = [r]
        res = _setHWTimeStampConfig(cfg, tscfg)
        if res is None:
            res = _getHWTimeStampConfig(cfg)
        if r['index'] == 0 or r['index'] == 1:
            ksft_eq(res['rx-filters']['bits']['bit'][0]['index'], r['index'])
        else:
            # the driver can fallback to some value which has higher coverage for timestamping
            ksft_ge(res['rx-filters']['bits']['bit'][0]['index'], r['index'])
    _setHWTimeStampConfig(cfg, orig_tscfg)

def main() -> None:
    with NetDrvEnv(__file__, nsim_test=False) as cfg:
        cfg.ethnl = EthtoolFamily()
        ksft_run([test_hwtstamp_tx, test_hwtstamp_rx], args=(cfg,))
        ksft_exit()

if __name__ == "__main__":
    main()

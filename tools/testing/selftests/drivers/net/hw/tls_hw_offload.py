#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Test kTLS hardware offload using a C helper binary."""

from collections import defaultdict

from lib.py import ksft_run, ksft_exit, ksft_pr, KsftSkipEx, ksft_true
from lib.py import ksft_variants, KsftNamedVariant
from lib.py import NetDrvEpEnv
from lib.py import cmd, bkg, wait_port_listen, rand_port


def check_tls_support(cfg):
    try:
        cmd("test -f /proc/net/tls_stat")
        cmd("test -f /proc/net/tls_stat", host=cfg.remote)
    except Exception as e:
        raise KsftSkipEx(f"kTLS not supported: {e}")


def read_tls_stats(host=None):
    stats = defaultdict(int)
    output = cmd("cat /proc/net/tls_stat", host=host)
    for line in output.stdout.strip().split('\n'):
        parts = line.split()
        if len(parts) == 2:
            stats[parts[0]] = int(parts[1])
    return stats


def stat_diff(before, after, key):
    return after[key] - before[key]


def check_path(before, after, direction, role, require_hw):
    """On the DUT, require HW offload; on the remote, HW or SW is fine."""
    dev = stat_diff(before, after, f'Tls{direction}Device')
    sw = stat_diff(before, after, f'Tls{direction}Sw')
    if require_hw:
        if dev < 1:
            ksft_pr(f"FAIL: {role} {direction}: HW offload not engaged "
                    f"(Device={dev}, Sw={sw})")
            return 1
    elif dev < 1 and sw < 1:
        ksft_pr(f"FAIL: {role} {direction}: no TLS activity "
                f"(Device={dev}, Sw={sw})")
        return 1
    return 0


def check_min(before, after, key, minimum, role):
    diff = stat_diff(before, after, key)
    if diff < minimum:
        ksft_pr(f"FAIL: {role} {key}: expected >= {minimum}, got {diff}")
        return 1
    return 0


def check_zero(before, after, key, role):
    diff = stat_diff(before, after, key)
    if diff > 0:
        ksft_pr(f"FAIL: {role} {key} increased by {diff}")
        return 1
    return 0


def verify_tls_counters(stats_before, stats_after, expected_rekeys,
                        tls_role, is_dut, burst=False):
    """Verify TLS counters on one side of the connection.

    tls_role: 'client' or 'server' (TLS role this side played).
    is_dut: True for the local DUT; requires HW offload counters.
    burst: burst mode - only the TLS client rotates its TX key; the TLS
           server only follows with an RX rotation on KeyUpdate receipt.
    """
    errors = 0
    role = 'DUT' if is_dut else 'Peer'

    # In burst mode only one direction carries TLS traffic per side
    # (TLS client sends, TLS server receives). Check HW offload only on
    # the active direction(s); require HW on the DUT's active direction.
    if burst:
        if tls_role == 'client':
            errors += check_path(stats_before, stats_after, 'Tx', role,
                                 require_hw=is_dut)
        else:
            errors += check_path(stats_before, stats_after, 'Rx', role,
                                 require_hw=is_dut)
    else:
        errors += check_path(stats_before, stats_after, 'Tx', role,
                             require_hw=is_dut)
        errors += check_path(stats_before, stats_after, 'Rx', role,
                             require_hw=is_dut)

    if expected_rekeys > 0:
        if burst:
            if tls_role == 'client':
                errors += check_min(stats_before, stats_after,
                                    'TlsTxRekeyOk', expected_rekeys, role)
                errors += check_zero(stats_before, stats_after,
                                     'TlsTxRekeyError', role)
            else:
                errors += check_min(stats_before, stats_after,
                                    'TlsRxRekeyOk', expected_rekeys, role)
                errors += check_min(stats_before, stats_after,
                                    'TlsRxRekeyReceived', expected_rekeys,
                                    role)
                errors += check_zero(stats_before, stats_after,
                                     'TlsRxRekeyError', role)
        else:
            errors += check_min(stats_before, stats_after,
                                'TlsTxRekeyOk', expected_rekeys, role)
            errors += check_min(stats_before, stats_after,
                                'TlsRxRekeyOk', expected_rekeys, role)
            if tls_role == 'server':
                errors += check_min(stats_before, stats_after,
                                    'TlsRxRekeyReceived', expected_rekeys,
                                    role)
            errors += check_zero(stats_before, stats_after,
                                 'TlsTxRekeyError', role)
            errors += check_zero(stats_before, stats_after,
                                 'TlsRxRekeyError', role)

    errors += check_zero(stats_before, stats_after, 'TlsDecryptError', role)

    return errors == 0


def run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0,
                 buffer_size=None, random_max=None, burst=False, zc=False,
                 dut_role="client", num_iterations=None):
    """Run the TLS offload test.

    dut_role: 'client' (default) - DUT runs the TLS client, remote the server.
              'server' - swap: DUT listens, remote connects. Used for burst_rx
              so the DUT's RX path is the one under rekey pressure.

    The DUT (local) is the kernel under test; the remote is just a traffic
    source/sink and may run any kernel without HW offload. Both sides run
    kTLS because TLS is pairwise, but verify_tls_counters() requires HW
    offload only on the DUT (is_dut=True); the peer may use SW kTLS.
    """
    port = rand_port()
    send_size = random_max or buffer_size

    if dut_role == "client":
        server_bin, server_host = cfg.bin_remote, cfg.remote
        client_bin, client_host = cfg.bin_local, None
        client_target = cfg.remote_addr_v['4']
    else:
        server_bin, server_host = cfg.bin_local, None
        client_bin, client_host = cfg.bin_remote, cfg.remote
        client_target = cfg.addr_v['4']

    server_cmd = f"{server_bin} server -p {port} -c {cipher} -v {tls_version}"
    if burst:
        server_cmd += " -B"
    if zc:
        server_cmd += " -Z"
    if send_size:
        server_cmd += f" -b {send_size}"

    client_cmd = (f"{client_bin} client -s {client_target} "
                  f"-p {port} -c {cipher} -v {tls_version}")
    if rekey:
        client_cmd += f" -k {rekey}"
    if burst:
        client_cmd += " -B"
    if num_iterations:
        client_cmd += f" -n {num_iterations}"
    if random_max:
        client_cmd += f" -r {random_max}"
    elif buffer_size:
        client_cmd += f" -b {buffer_size}"

    # Burst variants push hundreds of MB and perform many rekeys; the
    # default cmd() timeout (5s) is too short.
    cmd_timeout = 180 if burst else 5

    stats_before_local = read_tls_stats()
    stats_before_remote = read_tls_stats(host=cfg.remote)

    with bkg(server_cmd, host=server_host, exit_wait=True):
        wait_port_listen(port, host=server_host)
        cmd(client_cmd, host=client_host, timeout=cmd_timeout)

    stats_after_local = read_tls_stats()
    stats_after_remote = read_tls_stats(host=cfg.remote)

    dut_tls_role = dut_role
    peer_tls_role = 'server' if dut_role == 'client' else 'client'

    dut_ok = verify_tls_counters(stats_before_local, stats_after_local,
                                 rekey, dut_tls_role, is_dut=True,
                                 burst=burst)
    peer_ok = verify_tls_counters(stats_before_remote, stats_after_remote,
                                  rekey, peer_tls_role, is_dut=False,
                                  burst=burst)

    ksft_true(dut_ok, "DUT TLS counters verified")
    ksft_true(peer_ok, "Peer TLS counters verified")


@ksft_variants([
    KsftNamedVariant("tls13_aes128", "128", "1.3"),
    KsftNamedVariant("tls13_aes256", "256", "1.3"),
    KsftNamedVariant("tls12_aes128", "128", "1.2"),
    KsftNamedVariant("tls12_aes256", "256", "1.2"),
])
def test_tls_offload(cfg, cipher, tls_version):
    run_tls_test(cfg, cipher=cipher, tls_version=tls_version)


@ksft_variants([
    KsftNamedVariant("single", 1),
    KsftNamedVariant("multiple", 99),
    KsftNamedVariant("small_buf", 30, 512),
    KsftNamedVariant("large_buf", 10, 2097152),
    KsftNamedVariant("random_buf", 20, None, 8192),
])
def test_tls_offload_rekey(cfg, rekey, buffer_size=None, random_max=None):
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=rekey,
                 buffer_size=buffer_size, random_max=random_max)


@ksft_variants([
    KsftNamedVariant("burst_tx_rekey_every_1",        "client", False, 1,     50, 65536),
    KsftNamedVariant("burst_tx_rekey_every_1000",     "client", False, 1000,  3,  65536),
    KsftNamedVariant("burst_rx_rekey_every_10",       "server", False, 10,    20, 65536),
    KsftNamedVariant("burst_rx_rekey_every_10000",    "server", False, 10000, 1,  32768),
    KsftNamedVariant("burst_rx_zc_rekey_every_100",   "server", True,  100,   10, 65536),
    KsftNamedVariant("burst_rx_zc_rekey_every_20000", "server", True,  20000, 1,  16384),
])
def test_tls_offload_burst(cfg, dut_role, zc, interval, rekeys, buffer_size):
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=rekeys,
                 buffer_size=buffer_size, burst=True, zc=zc, dut_role=dut_role,
                 num_iterations=interval * (rekeys + 1))


def main() -> None:
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.bin_local = cfg.test_dir / "tls_hw_offload"
        if not cfg.bin_local.exists():
            raise KsftSkipEx(f"tls_hw_offload binary not found at {cfg.bin_local}")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)
        cfg.require_ipver("4")
        check_tls_support(cfg)

        ksft_run([test_tls_offload, test_tls_offload_rekey,
                  test_tls_offload_burst], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()

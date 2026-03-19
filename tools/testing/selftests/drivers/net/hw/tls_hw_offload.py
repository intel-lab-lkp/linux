#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Test kTLS hardware offload using a C helper binary."""

from lib.py import ksft_run, ksft_exit, ksft_pr, KsftSkipEx, ksft_true
from lib.py import NetDrvEpEnv
from lib.py import cmd, bkg, wait_port_listen, rand_port
import time


def check_tls_support(cfg):
    try:
        cmd("test -f /proc/net/tls_stat")
        cmd("test -f /proc/net/tls_stat", host=cfg.remote)
    except Exception as e:
        raise KsftSkipEx(f"kTLS not supported: {e}")


def read_tls_stats():
    stats = {}
    output = cmd("cat /proc/net/tls_stat")
    for line in output.stdout.strip().split('\n'):
        parts = line.split()
        if len(parts) == 2:
            stats[parts[0]] = int(parts[1])
    return stats


def verify_tls_counters(stats_before, stats_after, expected_rekeys, is_server):
    tx_device_diff = (stats_after.get('TlsTxDevice', 0) -
                      stats_before.get('TlsTxDevice', 0))
    rx_device_diff = (stats_after.get('TlsRxDevice', 0) -
                      stats_before.get('TlsRxDevice', 0))
    tx_sw_diff = (stats_after.get('TlsTxSw', 0) -
                  stats_before.get('TlsTxSw', 0))
    rx_sw_diff = (stats_after.get('TlsRxSw', 0) -
                  stats_before.get('TlsRxSw', 0))
    decrypt_err_diff = (stats_after.get('TlsDecryptError', 0) -
                        stats_before.get('TlsDecryptError', 0))

    used_tx_hw = tx_device_diff >= 1
    used_rx_hw = rx_device_diff >= 1
    used_tx_sw = tx_sw_diff >= 1
    used_rx_sw = rx_sw_diff >= 1

    errors = 0

    role = 'Server' if is_server else 'Client'
    ksft_pr(f"=== Counter Verification ({role}) ===")

    tx_dev_before = stats_before.get('TlsTxDevice', 0)
    tx_dev_after = stats_after.get('TlsTxDevice', 0)
    ksft_pr(f"TlsTxDevice: {tx_dev_before} -> {tx_dev_after} "
            f"(diff: {tx_device_diff})")

    tx_sw_before = stats_before.get('TlsTxSw', 0)
    tx_sw_after = stats_after.get('TlsTxSw', 0)
    ksft_pr(f"TlsTxSw: {tx_sw_before} -> {tx_sw_after} "
            f"(diff: {tx_sw_diff})")

    if used_tx_hw:
        ksft_pr("TX Path: HARDWARE OFFLOAD")
    elif used_tx_sw:
        ksft_pr("TX Path: SOFTWARE")
    else:
        ksft_pr("TX Path: FAIL (no TLS TX activity detected)")
        errors += 1

    rx_dev_before = stats_before.get('TlsRxDevice', 0)
    rx_dev_after = stats_after.get('TlsRxDevice', 0)
    ksft_pr(f"TlsRxDevice: {rx_dev_before} -> {rx_dev_after} "
            f"(diff: {rx_device_diff})")

    rx_sw_before = stats_before.get('TlsRxSw', 0)
    rx_sw_after = stats_after.get('TlsRxSw', 0)
    ksft_pr(f"TlsRxSw: {rx_sw_before} -> {rx_sw_after} "
            f"(diff: {rx_sw_diff})")

    if used_rx_hw:
        ksft_pr("RX Path: HARDWARE OFFLOAD")
    elif used_rx_sw:
        ksft_pr("RX Path: SOFTWARE")
    else:
        ksft_pr("RX Path: FAIL (no TLS RX activity detected)")
        errors += 1

    if expected_rekeys > 0:
        tx_rekey_diff = (stats_after.get('TlsTxRekeyOk', 0) -
                         stats_before.get('TlsTxRekeyOk', 0))
        rx_rekey_diff = (stats_after.get('TlsRxRekeyOk', 0) -
                         stats_before.get('TlsRxRekeyOk', 0))
        rx_rekey_recv_diff = (stats_after.get('TlsRxRekeyReceived', 0) -
                              stats_before.get('TlsRxRekeyReceived', 0))
        tx_rekey_err_diff = (stats_after.get('TlsTxRekeyError', 0) -
                             stats_before.get('TlsTxRekeyError', 0))
        rx_rekey_err_diff = (stats_after.get('TlsRxRekeyError', 0) -
                             stats_before.get('TlsRxRekeyError', 0))

        tx_rekey_before = stats_before.get('TlsTxRekeyOk', 0)
        tx_rekey_after = stats_after.get('TlsTxRekeyOk', 0)
        ksft_pr(f"TlsTxRekeyOk: {tx_rekey_before} -> {tx_rekey_after} "
                f"(diff: {tx_rekey_diff})")
        if tx_rekey_diff < expected_rekeys:
            ksft_pr(f"FAIL: Expected >= {expected_rekeys} TX rekeys")
            errors += 1

        rx_rekey_before = stats_before.get('TlsRxRekeyOk', 0)
        rx_rekey_after = stats_after.get('TlsRxRekeyOk', 0)
        ksft_pr(f"TlsRxRekeyOk: {rx_rekey_before} -> {rx_rekey_after} "
                f"(diff: {rx_rekey_diff})")
        if rx_rekey_diff < expected_rekeys:
            ksft_pr(f"FAIL: Expected >= {expected_rekeys} RX rekeys")
            errors += 1

        if is_server:
            rx_recv_before = stats_before.get('TlsRxRekeyReceived', 0)
            rx_recv_after = stats_after.get('TlsRxRekeyReceived', 0)
            ksft_pr(f"TlsRxRekeyReceived: {rx_recv_before} -> "
                    f"{rx_recv_after} (diff: {rx_rekey_recv_diff})")
            if rx_rekey_recv_diff < expected_rekeys:
                ksft_pr(f"FAIL: Expected >= {expected_rekeys} "
                        f"KeyUpdate messages")
                errors += 1

        if tx_rekey_err_diff > 0:
            ksft_pr(f"ERROR: TlsTxRekeyError increased by "
                    f"{tx_rekey_err_diff}")
            errors += 1
        if rx_rekey_err_diff > 0:
            ksft_pr(f"ERROR: TlsRxRekeyError increased by "
                    f"{rx_rekey_err_diff}")
            errors += 1

    if decrypt_err_diff > 0:
        ksft_pr(f"ERROR: TlsDecryptError increased by {decrypt_err_diff}")
        errors += 1

    ksft_pr(f"=== Verification {'PASSED' if errors == 0 else 'FAILED'} ===\n")
    return errors == 0


def run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0, buffer_size=None, random_max=None):
    port = rand_port()

    server_cmd = f"{cfg.bin_remote} server -p {port} -c {cipher} -v {tls_version}"
    if rekey > 0:
        server_cmd += f" --rekey={rekey}"
    if random_max:
        server_cmd += f" -r {random_max}"
    elif buffer_size:
        server_cmd += f" -b {buffer_size}"

    client_cmd = (f"{cfg.bin_local} client -s {cfg.remote_addr_v['4']} "
                  f"-p {port} -c {cipher} -v {tls_version}")
    if rekey > 0:
        client_cmd += f" --rekey={rekey}"
    if random_max:
        client_cmd += f" -r {random_max}"
    elif buffer_size:
        client_cmd += f" -b {buffer_size}"

    test_desc = f"cipher={cipher}, version={tls_version}, rekey={rekey}"
    if random_max:
        test_desc += f", random_size=1-{random_max}"
    elif buffer_size:
        test_desc += f", buffer={buffer_size}"
    ksft_pr(f"Starting TLS test: {test_desc}")

    stats_before_local = read_tls_stats()
    stats_before_remote = read_tls_stats_remote(cfg)

    with bkg(server_cmd, host=cfg.remote, exit_wait=True):
        wait_port_listen(port, host=cfg.remote)
        time.sleep(0.5)

        ksft_pr("Running client...")
        result = cmd(client_cmd, fail=False)
        time.sleep(1)

    stats_after_local = read_tls_stats()
    stats_after_remote = read_tls_stats_remote(cfg)

    ksft_pr("\n=== Client Side Verification ===")
    client_ok = verify_tls_counters(stats_before_local, stats_after_local, rekey, False)

    ksft_pr("\n=== Server Side Verification ===")
    server_ok = verify_tls_counters(stats_before_remote, stats_after_remote, rekey, True)

    ksft_true(result.ret == 0, "Client completed successfully")
    ksft_true(client_ok, "Client TLS counters verified")
    ksft_true(server_ok, "Server TLS counters verified")


def read_tls_stats_remote(cfg):
    stats = {}
    output = cmd("cat /proc/net/tls_stat", host=cfg.remote)
    for line in output.stdout.strip().split('\n'):
        parts = line.split()
        if len(parts) == 2:
            stats[parts[0]] = int(parts[1])
    return stats


def test_tls_offload_basic(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0)


def test_tls_offload_aes256(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="256", tls_version="1.3", rekey=0)


def test_tls_offload_tls12(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.2", rekey=0)


def test_tls_offload_tls12_aes256(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="256", tls_version="1.2", rekey=0)


def test_tls_offload_rekey(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=1)


def test_tls_offload_rekey_multiple(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=99)


def test_tls_offload_small_records(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=30, buffer_size=512)


def test_tls_offload_large_records(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=10, buffer_size=2097152)


def test_tls_offload_random_sizes(cfg):
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=20, random_max=8192)


def main() -> None:
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        cfg.bin_local = cfg.test_dir / "tls_hw_offload"
        if not cfg.bin_local.exists():
            raise KsftSkipEx(f"tls_hw_offload binary not found at {cfg.bin_local}")
        cfg.bin_remote = cfg.remote.deploy(cfg.bin_local)

        ksft_run([
            test_tls_offload_basic,
            test_tls_offload_aes256,
            test_tls_offload_tls12,
            test_tls_offload_tls12_aes256,
            test_tls_offload_rekey,
            test_tls_offload_rekey_multiple,
            test_tls_offload_small_records,
            test_tls_offload_large_records,
            test_tls_offload_random_sizes,
        ], args=(cfg, ))
    ksft_exit()


if __name__ == "__main__":
    main()

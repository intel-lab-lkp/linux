#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
TLS Hardware Offload Test

This test verifies kTLS hardware offload functionality between two endpoints
using the existing driver test framework (NetDrvEpEnv).

The test uses a C helper binary (tls_hw_offload)
to perform the actual TLS operations with hardcoded keys (no TLS handshake).

For rekey testing, proper TLS KeyUpdate handshake messages are sent via
sendmsg/recvmsg with TLS_SET_RECORD_TYPE/TLS_GET_RECORD_TYPE.

The test verifies TLS counters from /proc/net/tls_stat:
  - TlsTxDevice/TlsRxDevice: HW offload was used
  - TlsTxRekeyOk/TlsRxRekeyOk: Rekey operations succeeded (TLS 1.3 only)
  - TlsRxRekeyReceived: KeyUpdate messages received (server)
  - TlsDecryptError: No decryption errors occurred

Note: This test requires actual hardware with TLS offload support when run
in HW mode. It will not trigger hardware offload on loopback or veth pairs.
"""

from lib.py import ksft_run, ksft_exit, ksft_pr, KsftSkipEx, ksft_true
from lib.py import NetDrvEpEnv
from lib.py import cmd, bkg, wait_port_listen, rand_port
import time


def check_tls_support(cfg):
    """Check if kTLS is supported on both local and remote."""
    # Check if /proc/net/tls_stat exists
    try:
        cmd("test -f /proc/net/tls_stat")
        cmd("test -f /proc/net/tls_stat", host=cfg.remote)
    except Exception as e:
        raise KsftSkipEx(f"kTLS not supported: {e}")


def read_tls_stats():
    """Read TLS statistics from /proc/net/tls_stat."""
    stats = {}
    output = cmd("cat /proc/net/tls_stat")
    for line in output.stdout.strip().split('\n'):
        parts = line.split()
        if len(parts) == 2:
            stats[parts[0]] = int(parts[1])
    return stats


def verify_tls_counters(stats_before, stats_after, expected_rekeys, is_server):
    """
    Verify TLS counters after test completion.
    Returns True on success, False on failure.
    """
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

    # Check rekey counters if rekeys were expected
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

    # Check for decrypt errors
    if decrypt_err_diff > 0:
        ksft_pr(f"ERROR: TlsDecryptError increased by {decrypt_err_diff}")
        errors += 1

    ksft_pr(f"=== Verification {'PASSED' if errors == 0 else 'FAILED'} ===\n")
    return errors == 0


def run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0, buffer_size=None, random_max=None):
    """
    Run TLS hardware offload test using the C binary.

    Args:
        cfg: NetDrvEpEnv configuration
        cipher: "128" or "256" for AES-GCM key size
        tls_version: "1.2" or "1.3"
        rekey: Number of rekeys to perform (0 = no rekey, TLS 1.3 only)
        buffer_size: Fixed buffer size in bytes (default: 16384)
        random_max: Use random buffer sizes from 1 to random_max (overrides buffer_size)
    """
    port = rand_port()

    # Build server command
    server_cmd = f"{cfg.bin_remote} server -p {port} -c {cipher} -v {tls_version}"
    if rekey > 0:
        server_cmd += f" --rekey={rekey}"
    if random_max:
        server_cmd += f" -r {random_max}"
    elif buffer_size:
        server_cmd += f" -b {buffer_size}"

    # Build client command
    client_cmd = (f"{cfg.bin_local} client -s {cfg.remote_addr_v['4']} "
                  f"-p {port} -c {cipher} -v {tls_version}")
    if rekey > 0:
        client_cmd += f" --rekey={rekey}"
    if random_max:
        client_cmd += f" -r {random_max}"
    elif buffer_size:
        client_cmd += f" -b {buffer_size}"

    # Build test description
    test_desc = f"cipher={cipher}, version={tls_version}, rekey={rekey}"
    if random_max:
        test_desc += f", random_size=1-{random_max}"
    elif buffer_size:
        test_desc += f", buffer={buffer_size}"
    ksft_pr(f"Starting TLS test: {test_desc}")

    # Read stats before test
    stats_before_local = read_tls_stats()
    stats_before_remote = read_tls_stats_remote(cfg)

    # Run server in background on remote
    with bkg(server_cmd, host=cfg.remote, exit_wait=True):
        # Wait for server to be ready
        wait_port_listen(port, host=cfg.remote)
        time.sleep(0.5)  # Extra time for server setup

        # Run client
        ksft_pr("Running client...")
        result = cmd(client_cmd, fail=False)

        # Give server time to finish
        time.sleep(1)

    # Read stats after test
    stats_after_local = read_tls_stats()
    stats_after_remote = read_tls_stats_remote(cfg)

    # Verify client side (local)
    ksft_pr("\n=== Client Side Verification ===")
    client_ok = verify_tls_counters(stats_before_local, stats_after_local, rekey, False)

    # Verify server side (remote)
    ksft_pr("\n=== Server Side Verification ===")
    server_ok = verify_tls_counters(stats_before_remote, stats_after_remote, rekey, True)

    # Check that client exited successfully
    ksft_true(result.ret == 0, "Client completed successfully")
    ksft_true(client_ok, "Client TLS counters verified")
    ksft_true(server_ok, "Server TLS counters verified")


def read_tls_stats_remote(cfg):
    """Read TLS statistics from remote endpoint."""
    stats = {}
    output = cmd("cat /proc/net/tls_stat", host=cfg.remote)
    for line in output.stdout.strip().split('\n'):
        parts = line.split()
        if len(parts) == 2:
            stats[parts[0]] = int(parts[1])
    return stats


def test_tls_offload_basic(cfg):
    """Test basic TLS 1.3 hardware offload with AES-GCM-128 (no rekey)."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0)


def test_tls_offload_aes256(cfg):
    """Test TLS 1.3 hardware offload with AES-GCM-256 (no rekey)."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="256", tls_version="1.3", rekey=0)


def test_tls_offload_tls12(cfg):
    """Test TLS 1.2 hardware offload with AES-GCM-128 (no rekey)."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.2", rekey=0)


def test_tls_offload_tls12_aes256(cfg):
    """Test TLS 1.2 hardware offload with AES-GCM-256 (no rekey)."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="256", tls_version="1.2", rekey=0)


def test_tls_offload_rekey(cfg):
    """Test TLS 1.3 hardware offload with rekey."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=1)


def test_tls_offload_rekey_multiple(cfg):
    """Test TLS 1.3 hardware offload with multiple rekeys."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=3)


def test_tls_offload_small_records(cfg):
    """Test TLS 1.3 with small record size (512 bytes)."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0, buffer_size=512)


def test_tls_offload_large_records(cfg):
    """Test TLS 1.3 with large record size (32KB)."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0, buffer_size=32768)


def test_tls_offload_random_sizes(cfg):
    """Test TLS 1.3 with random record sizes (1-8192 bytes)."""
    cfg.require_ipver("4")
    check_tls_support(cfg)
    run_tls_test(cfg, cipher="128", tls_version="1.3", rekey=0, random_max=8192)


def main() -> None:
    with NetDrvEpEnv(__file__, nsim_test=False) as cfg:
        # Deploy the C binary to both local and remote
        # The binary is built in the same directory as this test
        cfg.bin_local = cfg.test_dir / "tls_hw_offload"

        # Check if binary exists
        if not cfg.bin_local.exists():
            raise KsftSkipEx(
                f"tls_hw_offload binary not found at {cfg.bin_local}. "
                "Please build it first: make -C "
                "tools/testing/selftests/drivers/net/hw tls_hw_offload")

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


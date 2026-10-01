.. SPDX-License-Identifier: GPL-2.0

==========
xskxceiver
==========

The AF_XDP engine in ``net/lib/xsk`` is built as ``net/lib/xskxceiver``.
The generic ``net/test_xsk.sh`` test runs SKB and DRV modes over veth.

Generic veth test
=================

Two ``xskxceiver`` processes own the TX and RX veth interfaces. The TX
process listens on a TCP control port and the RX process connects. The
shell wrapper starts a fresh process pair for each mode and case. A small
control channel reports readiness, packet progress, and aborts. For example::

  make -C tools/testing/selftests/net/lib
  cd tools/testing/selftests/net
  sudo ./test_xsk.sh

The shell wrapper creates the veth pair. ``-m skb|drv`` selects a mode,
``-t`` selects a test by the name or number shown by ``xskxceiver -l``,
and ``-p N`` selects a fixed control port instead of the default random
port. The generic frame format is unchanged.

Hardware zero-copy test
=======================

``test_xsk_case_defs.h`` defines the cases for both ``xskxceiver`` and
``drivers/net/hw/xsk.py``, and the DUT directions in which the Python
runner runs each of them. The runner reads this list and owns the TAP
results.

It starts one ``xskxceiver`` process on each host for each case. The DUT
runs in ZC mode; the remote runs in SKB mode and does not need zero-copy.
RX and TX cases are named separately in TAP. The hardware runner skips a
DUT that does not advertise AF_XDP zero-copy. The DUT endpoint binds with
``XDP_ZEROCOPY``, which fails instead of falling back to copy mode, so an
advertised device that cannot bind or run a case fails it.

The remote XSK endpoint listens on a TCP control port and the DUT connects.
The listener prints a line on stderr once it listens, so ``xsk.py`` starts
the DUT endpoint without polling the remote for the port. The small
per-case channel reports readiness, packet progress, and aborts. It has no
capabilities or verdict handshake. ``xsk.py`` chooses the case, sets the
timeout, and reports the result from both process exit codes.

Both XSK endpoints generate and validate raw IPv4/UDP frames with one fixed
UDP port, so ntuple rules can steer the test flow. They do not use AF_INET
sockets. On DUT RX cases ``xsk.py`` reserves the last RX queue outside RSS
and steers test traffic to it. On DUT TX cases it steers traffic to queue 0
on the remote receiver. The XDP programs redirect every packet, so the
tested link must carry no other traffic; SSH and the control channel go
to the ``REMOTE_ARGS`` host, which has to be reached over another link.
Each case sets up only what its direction needs and undoes it when the
case ends, so a setup failure skips only the cases that need that setup.
Each endpoint detaches its XDP program and restores any changed ring sizes
and the MTU before it exits. The SKB-mode remote endpoint only raises its
MTU when a case needs a larger one, as changing the MTU can reset the NIC.
The runner restores the RSS table, ntuple setting, and huge-page count.
After an endpoint fails or is killed, the runner also detaches the XDP
program and restores the MTU. The runner does not change channel counts.
A one-channel DUT skips RX cases because it has no queue to reserve
outside RSS.

Setup
-----

Build the engine on the DUT::

  make -C tools/testing/selftests/net/lib

A top-level selftests build with ``TARGETS=drivers/net/hw`` includes
``net/lib`` as well. The binary is linked statically when the static
libraries are installed; ``XSK_STATIC=0`` or ``XSK_STATIC=1`` overrides the
probe.

Configure the usual driver test variables in
``tools/testing/selftests/drivers/net/hw/net.config`` or the environment::

  NETIF=eth0
  LOCAL_V4=192.0.2.1
  REMOTE_V4=192.0.2.2
  REMOTE_TYPE=ssh
  REMOTE_ARGS=user@remote.example.com
  XSK_REMOTE_BIN=/path/to/remote/xskxceiver
  XSK_REMOTE_SUDO=1

Run as root on the DUT::

  cd tools/testing/selftests/drivers/net/hw
  sudo ./xsk.py -t test_xsk.rx_send_receive

``./xsk.py -l`` lists the case names. Omit ``-t`` to run the full hardware
matrix. Set the variables in ``net.config`` when using ``sudo`` so they are
available to the test process.

``./xsk.py -b`` runs the same cases as ``test_xsk_busy_poll`` instead, with
the DUT endpoint busy polling as in the busy-poll pass of ``test_xsk.sh``.
Each case then sets ``napi_defer_hard_irqs`` and ``gro_flush_timeout`` on
the DUT to the values ``test_xsk.sh`` uses on veth and restores them when
it ends. The remote endpoint does not busy poll.

Each case starts its remote endpoint over SSH, and a DUT TX case also adds
and removes a flow steering rule on the remote. SSH connection sharing for
the remote host (``ControlMaster``, ``ControlPath`` and ``ControlPersist``
in ssh_config(5)) avoids a new SSH login for each remote command. With
``sudo``, that is root's SSH configuration.

The remote host needs AF_XDP in SKB mode and BPF. ``xsk.py`` copies the
DUT's ``xskxceiver`` binary to the remote, so both hosts must have
compatible architectures. ``XSK_REMOTE_BIN`` chooses a fixed destination
path for that copy. Set ``XSK_REMOTE_DEPLOY=0`` to use a binary built on
the remote from the same test case definitions instead. ``XSK_REMOTE_SUDO=1``
runs the remote endpoint and setup commands through passwordless ``sudo -n``;
omit it when SSH logs in as root. The DUT needs ntuple and RSS support for
RX cases, while the remote needs ntuple support for DUT TX cases. The test
uses ``NetDrvEpEnv`` and the networking selftest Python helpers for setup
and rollback.

The DUT uses the SSH host name to connect to the remote control listener.
``XSK_UDP_PORT`` changes the fixed test UDP port (default 42567). The remote
listens on all addresses for control.

The hardware list covers baseline traffic, 2K frames, poll, headroom,
invalid TX descriptors, TX invalid-descriptor statistics, metadata, 9K,
and unaligned traffic. For ``TOO_MANY_FRAGS``, the runner reads the DUT's
``xdp-zc-max-segs`` value and passes it to both endpoint processes so the
SKB peer validates the same packet stream without a capabilities exchange.

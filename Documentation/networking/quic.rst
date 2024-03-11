.. SPDX-License-Identifier: GPL-2.0

=================
Linux Kernel QUIC
=================

Introduction
============

This is an implementation of the QUIC protocol as defined in RFC9000. QUIC
is an UDP-Based Multiplexed and Secure Transport protocol, and it provides
applications with flow-controlled streams for structured communication,
low-latency connection establishment, and network path migration. QUIC
includes security measures that ensure confidentiality, integrity, and
availability in a range of deployment circumstances.

This implementation of QUIC in the kernel space enables users to utilize
the QUIC protocol through common socket APIs in user space. Additionally,
kernel subsystems like SMB and NFS can seamlessly operate over the QUIC
protocol after handshake using net/handshake APIs.

This implementation offers fundamental support for the following RFCs:

- RFC9000 - QUIC: A UDP-Based Multiplexed and Secure Transport
- RFC9001 - Using TLS to Secure QUIC
- RFC9002 - QUIC Loss Detection and Congestion Control
- RFC9221 - An Unreliable Datagram Extension to QUIC
- RFC9287 - Greasing the QUIC Bit
- RFC9368 - Compatible Version Negotiation for QUIC
- RFC9369 - QUIC Version 2
- Handshake APIs for tlshd Use - NFS/SMB over QUIC

Implementation
==============

The central idea is to implement QUIC within the kernel, incorporating an
userspace handshake approach.

Only the processing and creation of raw TLS Handshake Messages, facilitated
by a tls library like gnutls, take place in userspace. These messages are
exchanged through sendmsg/recvmsg() mechanisms, with cryptographic details
carried in the control message (cmsg).

The entirety of QUIC protocol, excluding TLS Handshake Messages processing
and creation, resides in the kernel. Instead of utilizing a User Level
Protocol (ULP) layer, it establishes a socket of IPPROTO_QUIC type (similar
to IPPROTO_MPTCP) operating over UDP tunnels.

Kernel consumers can initiate a handshake request from kernel to userspace
via the existing net/handshake netlink. The userspace component, tlshd from
ktls-utils, manages the QUIC handshake request processing.

- Handshake Architecture:

      +------+  +------+
      | APP1 |  | APP2 | ...
      +------+  +------+
      +-------------------------------------------------+
      |                libquic (ktls-utils)             |<--------------+
      |      {quic_handshake_server/client/param()}     |               |
      +-------------------------------------------------+      +---------------------+
        {send/recvmsg()}         {set/getsockopt()}            | tlshd (ktls-utils)  |
        [CMSG handshake_info]    [SOCKOPT_CRYPTO_SECRET]       +---------------------+
                                 [SOCKOPT_TRANSPORT_PARAM_EXT]
              | ^                            | ^                        | ^
  Userspace   | |                            | |                        | |
  ------------|-|----------------------------|-|------------------------|-|--------------
  Kernel      | |                            | |                        | |
              v |                            v |                        v |
      +--------------------------------------------------+         +-------------+
      |  socket (IPPRTOTO_QUIC)  |       protocol        |<----+   | handshake   |
      +--------------------------------------------------+     |   | netlink APIs|
      | inqueue | outqueue | cong | path | connection_id |     |   +-------------+
      +--------------------------------------------------+     |      |      |
      |   packet   |   frame   |   crypto   |   pnmap    |     |   +-----+ +-----+
      +--------------------------------------------------+     |   |     | |     |
      |         input           |       output           |     |---| SMB | | NFS | ...
      +--------------------------------------------------+     |   |     | |     |
      |                   UDP tunnels                    |     |   +-----+ +--+--+
      +--------------------------------------------------+     +--------------|

- Post Handshake Architecture:

      +------+  +------+
      | APP1 |  | APP2 | ...
      +------+  +------+
        {send/recvmsg()}         {set/getsockopt()}
        [CMSG stream_info]       [SOCKOPT_KEY_UPDATE]
                                 [SOCKOPT_CONNECTION_MIGRATION]
                                 [SOCKOPT_STREAM_OPEN/RESET/STOP_SENDING]
                                 [...]
              | ^                            | ^
  Userspace   | |                            | |
  ------------|-|----------------------------|-|----------------
  Kernel      | |                            | |
              v |                            v |
      +--------------------------------------------------+
      |  socket (IPPRTOTO_QUIC)  |       protocol        |<----+ {kernel_send/recvmsg()}
      +--------------------------------------------------+     | {kernel_set/getsockopt()}
      | inqueue | outqueue | cong | path | connection_id |     |
      +--------------------------------------------------+     |
      |   packet   |   frame   |   crypto   |   pnmap    |     |   +-----+ +-----+
      +--------------------------------------------------+     |   |     | |     |
      |         input           |       output           |     |---| SMB | | NFS | ...
      +--------------------------------------------------+     |   |     | |     |
      |                   UDP tunnels                    |     |   +-----+ +--+--+
      +--------------------------------------------------+     +--------------|

Usage
=====

This implementation supports a mapping of QUIC into sockets APIs. Similar
to TCP and SCTP, a typical Server and Client use the following system call
sequence to communicate:

       Client                    Server
    ------------------------------------------------------------------
    sockfd = socket(IPPROTO_QUIC)      listenfd = socket(IPPROTO_QUIC)
    bind(sockfd)                       bind(listenfd)
                                       listen(listenfd)
    connect(sockfd)
    quic_client_handshake(sockfd)
                                       sockfd = accecpt(listenfd)
                                       quic_server_handshake(sockfd, cert)

    sendmsg(sockfd)                    recvmsg(sockfd)
    close(sockfd)                      close(sockfd)
                                       close(listenfd)

Please note that quic_client_handshake() and quic_server_handshake() functions
are currently sourced from libquic in the github lxin/quic repository, and might
be integrated into ktls-utils in the future. These functions are responsible for
receiving and processing the raw TLS handshake messages until the completion of
the handshake process.

For utilization by kernel consumers, it is essential to have the tlshd service
(from ktls-utils) installed and running in userspace. This service receives
and manages kernel handshake requests for kernel sockets. In kernel, the APIs
closely resemble those used in userspace:

       Client                    Server
    ------------------------------------------------------------------------
    __sock_create(IPPROTO_QUIC, &sock)  __sock_create(IPPROTO_QUIC, &sock)
    kernel_bind(sock)                   kernel_bind(sock)
                                        kernel_listen(sock)
    kernel_connect(sock)
    tls_client_hello_x509(args:{sock})
                                        kernel_accept(sock, &newsock)
                                        tls_server_hello_x509(args:{newsock})

    kernel_sendmsg(sock)                kernel_recvmsg(newsock)
    sock_release(sock)                  sock_release(newsock)
                                        sock_release(sock)

Please be aware that tls_client_hello_x509() and tls_server_hello_x509() are
APIs from net/handshake/. They are employed to dispatch the handshake request
to the userspace tlshd service and subsequently block until the handshake
process is completed.

The QUIC module is currently labeled as "EXPERIMENTAL".

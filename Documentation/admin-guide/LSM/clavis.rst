.. SPDX-License-Identifier: GPL-2.0

======
Clavis
======

Clavis is a Linux Security Module that provides mandatory access control to
system kernel keys (i.e. builtin, secondary, machine and platform). These
restrictions will prohibit keys from being used for validation. Upon boot, the
Clavis LSM is provided a key id as a boot parameter.  This single key is then
used as the root of trust for any access control modifications made going
forward. Access control updates must be signed and validated by this key.

Clavis has its own keyring.  All ACL updates are applied through this keyring.
The update must be signed by the single root of trust key.

When enabled, all system keys are prohibited from being used until an ACL is
added for them.

On UEFI platforms, the root of trust key shall survive a kexec. Trying to
defeat or change it from the command line is not allowed.  The original boot
parameter is stored in UEFI and will always be referenced following a kexec.

The Clavis LSM contains a system keyring call .clavis.  It contains a single
asymmetric key that is used to validate anything added to it.  This key can
be added during boot and must be a preexisting system kernel key.  If the
``clavis=`` boot parameter is not used, any asymmetric key the user owns
can be added to enable the LSM.

The only user space components are OpenSSL and the keyctl utility. A new
key type call ``clavis_key_acl`` is used for ACL updates. Any number of signed
``clavis_key_acl`` entries may be added to the .clavis keyring. The
``clavis_key_acl`` contains the subject key identifier along with the allowed
usage type for the key.

The format is as follows:

.. code-block:: console

  XX:YYYYYYYYYYY

  XX - Single byte of the key type
	VERIFYING_MODULE_SIGNATURE            00
	VERIFYING_FIRMWARE_SIGNATURE          01
	VERIFYING_KEXEC_PE_SIGNATURE          02
	VERIFYING_KEY_SIGNATURE               03
	VERIFYING_KEY_SELF_SIGNATURE          04
	VERIFYING_UNSPECIFIED_SIGNATURE       05
  :  - ASCII colon
  YY - Even number of hexadecimal characters representing the key id

The ``clavis_key_acl`` must be S/MIME signed by the sole asymmetric key contained
within the .clavis keyring.

In the future if new features are added, new key types could be created.

Usage Examples
==============

How to create a signing key:
----------------------------

.. code-block:: bash

  cat <<EOF > clavis-lsm.genkey
  [ req ]
  default_bits = 4096
  distinguished_name = req_distinguished_name
  prompt = no
  string_mask = utf8only
  x509_extensions = v3_ca
  [ req_distinguished_name ]
  O = TEST
  CN = Clavis LSM key
  emailAddress = user@example.com
  [ v3_ca ]
  basicConstraints=CA:TRUE
  subjectKeyIdentifier=hash
  authorityKeyIdentifier=keyid:always,issuer
  keyUsage=digitalSignature
  EOF

  openssl req -new -x509 -utf8 -sha256 -days 3650 -batch \
        -config clavis-lsm.genkey -outform DER \
        -out clavis-lsm.x509 -keyout clavis-lsm.priv

How to get the Subject Key Identifier
-------------------------------------

.. code-block:: bash

  openssl x509 -in ./clavis-lsm.x509 -inform der \
        -ext subjectKeyIdentifier  -nocert \
        | tail -n +2 | cut -f2 -d '='| tr -d ':'
  4a00ab9f35c9dc3aed7c225d22bafcbd9285e1e8

How to enroll the signing key into the MOK
------------------------------------------

The key must now be added to the machine or platform keyrings.  This
indicates the key was added by the system owner. For kernels booted through
shim, a first-stage UEFI boot loader, a key may be added to the machine keyring
by doing:

.. code-block:: bash

  mokutil --import ./clavis-lsm.x509

and then rebooting and enrolling the key through MokManager.

How to enable the Clavis LSM
----------------------------

Add the key id to the ``clavis=`` boot parameter.  With the example above the
key id is the subject key identifier: 4a00ab9f35c9dc3aed7c225d22bafcbd9285e1e8

Add the following boot parameter:

.. code-block:: console

  clavis=4a00ab9f35c9dc3aed7c225d22bafcbd9285e1e8

After booting there will be a single key contained in the .clavis keyring:

.. code-block:: bash

  keyctl show %:.clavis
  Keyring
    254954913 ----swrv      0     0  keyring: .clavis
    301905375 ---lswrv      0     0   \_ asymmetric: TEST: Clavis LSM key: 4a00ab9f35c9dc3aed7c225d22bafcbd9285e1e8

The original ``clavis=`` boot parameter will persist across any kexec. Changing it or
removing it has no effect.


How to sign an entry to be added to the .clavis keyring:
--------------------------------------------------------

In this example we have 3 keys in the machine keyring.  Our Clavis LSM key, a
key we want to use for kernel verification and a key we want to use for module
verification.

.. code-block:: bash

  keyctl show %:.machine
  Keyring
    999488265 ---lswrv      0     0  keyring: .machine
    912608009 ---lswrv      0     0   \_ asymmetric: TEST: Module Key: 17eb8c5bf766364be094c577625213700add9471
    646229664 ---lswrv      0     0   \_ asymmetric: TEST: Kernel Key: b360d113c848ace3f1e6a80060b43d1206f0487d
   1073737099 ---lswrv      0     0   \_ asymmetric: TEST: Clavis LSM key: 4a00ab9f35c9dc3aed7c225d22bafcbd9285e1e8

To update the .clavis kerying ACL list, first create a file containing the
key usage type followed by a colon and the key id that we want to allow to
validate that usage.  In the first example we are saying key
17eb8c5bf766364be094c577625213700add9471 is allowed to validate kernel modules.
In the second example we are saying key b360d113c848ace3f1e6a80060b43d1206f0487d
is allowed to validate signed kernels.

.. code-block:: bash

  echo "00:17eb8c5bf766364be094c577625213700add9471" > module-acl.txt
  echo "02:b360d113c848ace3f1e6a80060b43d1206f0487d" > kernel-acl.txt

Now both these files must be signed by the key contained in the .clavis keyring:

.. code-block:: bash

  openssl smime -sign -signer clavis-lsm.x509 -inkey clavis-lsm.priv -in module-acl.txt \
        -out module-acl.pkcs7 -binary -outform DER -nodetach -noattr

  openssl smime -sign -signer clavis-lsm.x509 -inkey clavis-lsm.priv -in kernel-acl.txt \
        -out kernel-acl.pkcs7 -binary -outform DER -nodetach -noattr

Afterwards the ACL list in the clavis keyring can be updated:

.. code-block:: bash

  keyctl padd clavis_key_acl "" %:.clavis < module-acl.pkcs7
  keyctl padd clavis_key_acl "" %:.clavis < kernel-acl.pkcs7

  keyctl show %:.clavis

  Keyring
    254954913 ----swrv      0     0  keyring: .clavis
    301905375 ---lswrv      0     0   \_ asymmetric: TEST: Clavis LSM key: 4a00ab9f35c9dc3aed7c225d22bafcbd9285e1e8
   1013065475 --alswrv      0     0   \_ clavis_key_acl: 02:b360d113c848ace3f1e6a80060b43d1206f0487d
    445581284 --alswrv      0     0   \_ clavis_key_acl: 00:17eb8c5bf766364be094c577625213700add9471

Now the 17eb8c5bf766364be094c577625213700add9471 key can be used for
validating kernel modules and the b360d113c848ace3f1e6a80060b43d1206f0487d
key can be used to validate signed kernels.

.. SPDX-License-Identifier: GPL-2.0
.. include:: ../../disclaimer-zh_CN.rst

:Original: Documentation/security/keys/ecryptfs.rst

:翻译:

 赵硕 Shuo Zhao <zhaoshuo@cqsoftware.com.cn>

==========================
eCryptfs文件系统的加密密钥
==========================

eCryptfs是一种堆叠式文件系统，它可以在文件级别上实现透明的加密与解密。每个文件都会
使用一个随机生成的文件加密密钥（FEK，File Encryption Key）来进行加密。

每个FEK又会通过一个文件加密密钥加密密钥（FEKEK, File Encryption Key Encryption Key）
进行加密，这个过程可能发生在内核空间或用户空间的守护进程“ecryptfsd”中。在内核空间中
FEK的加密和解密操作由内核的CryptoAPI直接执行，使用一个由用户输入的口令派生出的 FEKEK。
在用户空间中，该操作由守护进程“ecryptfsd”完成，并借助外部库以支持更多机制，例如公钥
加密、PKCS#11，以及基于TPM（可信平台模块）的操作。

为了存储解密 FEK 所需的信息，eCryptfs 定义了一种称为认证令牌（authentication token）的
数据结构。目前，这种令牌可以存储在内核的“user”类型密钥中，由用户空间的实用工具mount.ecryptfs
（属于 ecryptfs-utils 软件包）插入到用户的会话密钥环中。

为了与eCryptfs文件系统配合使用，“encrypted” 密钥类型被扩展，引入了新的格式 “ecryptfs”。
该新格式的加密密钥在其载荷（payload）中保存了一个认证令牌，其中包含一个由内核随机生成的
FEKEK，并且该FEKEK又受父主密钥（parent master key）保护。

为了防止已知明文攻击（known-plaintext attack），通过命令‘keyctl print’或‘keyctl pipe’
获得的数据块并不包含完整的认证令牌（其内容是众所周知的），而只包含经过加密的 FEKEK。

eCryptfs文件系统能够切实受益于加密密钥的使用，因为管理员可以在系统启动时，在受控环境
中通过“trusted”密钥解封后安全地生成所需密钥并挂载文件系统。此外，由于密钥只在内核层
以明文形式存在，因此可以避免被恶意软件窃取或攻击的风险。

Usage::

   keyctl add encrypted name "new ecryptfs key-type:master-key-name keylen" ring
   keyctl add encrypted name "load hex_blob" ring
   keyctl update keyid "update key-type:master-key-name"

Where::

        name:= '<16 hexadecimal characters>'
        key-type:= 'trusted' | 'user'
        keylen:= 64

使用eCryptfs文件系统时加密密钥示例：

创建一个长度为64字节的加密密钥“1000100010001000”，格式为‘ecryptfs’，并使用之前加载的用户
密钥“test”保存它::

    $ keyctl add encrypted 1000100010001000 "new ecryptfs user:test 64" @u
    19184530

    $ keyctl print 19184530
    ecryptfs user:test 64 490045d4bfe48c99f0d465fbbbb79e7500da954178e2de0697
    dd85091f5450a0511219e9f7cd70dcd498038181466f78ac8d4c19504fcc72402bfc41c2
    f253a41b7507ccaa4b2b03fff19a69d1cc0b16e71746473f023a95488b6edfd86f7fdd40
    9d292e4bacded1258880122dd553a661

    $ keyctl pipe 19184530 > ecryptfs.blob

使用创建的加密密钥“1000100010001000”将eCryptfs文件系统挂载到‘/secret’目录::

    $ mount -i -t ecryptfs -oecryptfs_sig=1000100010001000,\
      ecryptfs_cipher=aes,ecryptfs_key_bytes=32 /secret /secret

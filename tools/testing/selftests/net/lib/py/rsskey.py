# SPDX-License-Identifier: GPL-2.0

"""
Helpers to check the quality of an RSS key.

The Toeplitz hash is linear over GF(2): the hash is the XOR of the 32 bit key
windows selected by the set bits of the input, and hardware indexes the
indirection table with the low order bits of the hash. The windows belonging
to the q lowest bits of a header field therefore form a Toeplitz matrix, and
when that matrix is singular the flows of a burst differing only in those
bits, consecutive ephemeral ports typically, can not reach all of the 2 ** q
entries of the table. A key drawn uniformly at random is singular for a given
field and a given q with probability 1/2.

netdev_rss_key_fill() generates keys that are non singular for every field of
the hash input and every q up to RSS_KEY_QMAX.
"""

# Matches NETDEV_RSS_KEY_QMAX, that is up to 256 entries of the table.
RSS_KEY_QMAX = 8


def rss_key_bit(buf, bit):
    """Bit @bit of @buf, counting from the most significant bit of byte 0."""
    return (buf[bit // 8] >> (7 - bit % 8)) & 1


def rss_key_assign_bit(buf, bit, value):
    mask = 0x80 >> (bit % 8)

    if value:
        buf[bit // 8] |= mask
    else:
        buf[bit // 8] &= ~mask


def rss_key_window(key, bit):
    """The 32 key bits starting at @bit, what input bit @bit contributes."""
    value = 0

    for i in range(32):
        value = (value << 1) | rss_key_bit(key, bit + i)

    return value


def rss_key_toeplitz(key, inp, nbits):
    """The Toeplitz hash of the @nbits long input @inp under @key."""
    value = 0

    for i in range(nbits):
        if rss_key_bit(inp, i):
            value ^= rss_key_window(key, i)

    return value


def rss_key_full_rank(key, lsb, q):
    """Do the q low order bits of the field at @lsb reach all 2 ** q entries?

    Gaussian elimination over GF(2) on the q windows involved, reduced to
    their q low order bits, which are the ones indexing the table.
    """
    basis = {}

    for j in range(q):
        vector = rss_key_window(key, lsb - j) & ((1 << q) - 1)

        while vector:
            low = vector & -vector
            if low not in basis:
                basis[low] = vector
                break
            vector ^= basis[low]

        if not vector:
            return False

    return True


def rss_key_layout(fields, ipv6):
    """Describe the hash input built from @fields, as ethtool -n reports it.

    @fields is the flow hash configuration, "sdfn" for a 4-tuple or "sd" for
    a 2-tuple. Returns the list of (name, position of the least significant
    bit) and the length of the input in bits, or None if the layout involves
    something this does not know how to place.
    """
    addr_bits = 128 if ipv6 else 32
    known = (("s", "saddr", addr_bits),
             ("d", "daddr", addr_bits),
             ("f", "sport", 16),
             ("n", "dport", 16))

    if set(fields) - {flag for flag, _, _ in known}:
        return None, 0

    layout = []
    nbits = 0

    for flag, name, width in known:
        if flag not in fields:
            continue
        nbits += width
        layout.append((name, nbits - 1))

    return layout, nbits

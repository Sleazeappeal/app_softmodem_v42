# SPDX-License-Identifier: GPL-2.0-only
"""Shared payload generator for the softmodem test server and the modem-side client.

The same (kind, length, seed) always yields the same bytes, so both sides can
check a transfer without sending the reference data.
"""
import random
import zlib

TEXT = ("\x1b[1;33mSoftmodem test\x1b[0m  The quick brown fox jumps over the lazy dog. "
        "0123456789 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~\r\n")

KINDS = ("text", "ascii", "binary", "zeros", "mixed")


def payload(kind, n, seed):
    """text:   repeated line with ANSI codes (very compressible)
    ascii:  random printable characters (somewhat compressible)
    binary: random 0x00-0xff, incompressible, contains V.42bis escape bytes
    zeros:  all 0x00, long runs hit the maximum string length
    mixed:  256-byte blocks alternating text/binary, forces mode switches"""
    r = random.Random(f"{kind}:{n}:{seed}")
    if kind == "text":
        return (TEXT * (n // len(TEXT) + 1)).encode("latin-1")[:n]
    if kind == "ascii":
        return bytes(r.randrange(32, 127) for _ in range(n))
    if kind == "binary":
        return r.randbytes(n)
    if kind == "zeros":
        return bytes(n)
    if kind == "mixed":
        out = bytearray()
        text = TEXT.encode("latin-1") * 4
        while len(out) < n:
            out += text[:256] if (len(out) // 256) % 2 == 0 else r.randbytes(256)
        return bytes(out[:n])
    raise ValueError(f"unknown kind {kind}")


def crc(data):
    return f"{zlib.crc32(data) & 0xffffffff:08x}"


def first_diff(a, b):
    """Offset of the first differing byte and the number of differing bytes."""
    n = min(len(a), len(b))
    diffs = [i for i in range(n) if a[i] != b[i]]
    first = diffs[0] if diffs else (n if len(a) != len(b) else -1)
    return first, len(diffs) + abs(len(a) - len(b))

#!/usr/bin/env python3
"""Patches the WiiCompiled Quest app's classes.dex so it survives Lepton.

SDLClipboardHandler's constructor registers a clipboard listener. Lepton's Android image has no
clipboard service, the lookup returns null and the game process dies with a NullPointerException
before it draws a frame. The ten bytes of that call (iget-object + invoke-virtual
addPrimaryClipChangedListener) become Dalvik nops, then the dex header's SHA-1 signature and
Adler-32 checksum are recomputed. The same classes.dex ships in v1.3 and v1.3.1.

usage: dexpatch.py classes.dex.in classes.dex.out
"""
import hashlib
import struct
import sys
import zlib

CALL = bytes.fromhex('54209406' '6e204a00' '2000')


def patch(data: bytes) -> bytes:
    data = bytearray(data)
    hits = [i for i in range(len(data) - len(CALL)) if data[i:i + len(CALL)] == CALL]
    if len(hits) != 1:
        raise ValueError(f'expected one clipboard listener call in classes.dex, found {len(hits)}')
    off = hits[0]
    data[off:off + len(CALL)] = bytes(len(CALL))
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into('<I', data, 8, zlib.adler32(bytes(data[12:])) & 0xffffffff)
    return bytes(data)


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    open(dst, 'wb').write(patch(open(src, 'rb').read()))
    print(f'{dst}: clipboard listener call replaced with nops')

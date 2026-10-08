#!/usr/bin/env python3
"""Turns the upstream WiiCompiled Quest APK into the Steam Frame APK.

The APK carries the game kit the headset builds your game against (assets/game_kit). This swaps in
the kit files that build.sh made from the Frame-patched sources, gives the kit a new fingerprint
(so the app rebuilds a game made with the stock kit instead of loading it), applies the classes.dex
clipboard fix (dexpatch.py), writes a zipalign-equivalent archive and signs it with APK Signature
Scheme v2. Needs Python 3 and the openssl command only: no Java, apksigner or zipalign. An RSA
signing key is generated on first use.

usage: make-apk.py build  --upstream in.apk --kit DIR --signing DIR --out out.apk
       make-apk.py verify apk
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dexpatch  # noqa: E402

KIT = 'assets/game_kit/'
SIG_V2 = 0x7109871a
RSA_PKCS1_SHA256 = 0x0103
ALIGN_PAGE = 16384  # uncompressed native libraries are mapped straight out of the APK


def read_zip(path):
    """Every entry of the archive, in central directory order, with its raw (still compressed) data."""
    entries = []
    with zipfile.ZipFile(path) as z, open(path, 'rb') as f:
        for info in z.infolist():
            f.seek(info.header_offset)
            head = f.read(30)
            assert head[:4] == b'PK\x03\x04', info.filename
            nlen, xlen = struct.unpack_from('<HH', head, 26)
            f.seek(info.header_offset + 30 + nlen + xlen)
            entries.append({'info': info, 'raw': f.read(info.compress_size)})
    return entries


def replace(entry, content):
    """New content for an entry, compressed the way the original was."""
    info = entry['info']
    if info.compress_type == zipfile.ZIP_STORED:
        raw = content
    elif info.compress_type == zipfile.ZIP_DEFLATED:
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        raw = c.compress(content) + c.flush()
    else:
        raise ValueError(f'{info.filename}: compression method {info.compress_type} is not handled')
    info.CRC = zlib.crc32(content) & 0xffffffff
    info.file_size = len(content)
    info.compress_size = len(raw)
    entry['raw'] = raw


def dos_time(info):
    y, mo, d, h, mi, s = info.date_time
    return (h << 11 | mi << 5 | s // 2), ((y - 1980) << 9 | mo << 5 | d)


def write_entries(entries):
    """Local headers plus data, each uncompressed entry aligned as zipalign would; returns the bytes
    and the local header offset of every entry."""
    out = bytearray()
    offsets = []
    for e in entries:
        info = e['info']
        name = info.filename.encode('utf-8')
        extra = b''
        if info.compress_type == zipfile.ZIP_STORED:
            align = ALIGN_PAGE if info.filename.endswith('.so') else 4
            data_start = len(out) + 30 + len(name)
            pad = (-data_start) % align
            if 0 < pad < 4:
                pad += align
            if pad:
                extra = struct.pack('<HH', 0xd935, pad - 4) + bytes(pad - 4)  # zipalign's own extra id
        flags = (info.flag_bits & ~0x08) | 0x800  # no data descriptor; names are UTF-8
        t, d = dos_time(info)
        offsets.append(len(out))
        out += struct.pack('<IHHHHHIIIHH', 0x04034b50, 20, flags, info.compress_type, t, d, info.CRC,
                           info.compress_size, info.file_size, len(name), len(extra))
        out += name + extra + e['raw']
    return bytes(out), offsets


def central_directory(entries, offsets):
    cd = bytearray()
    for e, off in zip(entries, offsets):
        info = e['info']
        name = info.filename.encode('utf-8')
        flags = (info.flag_bits & ~0x08) | 0x800
        t, d = dos_time(info)
        cd += struct.pack('<IHHHHHHIIIHHHHHII', 0x02014b50, info.create_version, 20, flags, info.compress_type,
                          t, d, info.CRC, info.compress_size, info.file_size, len(name), 0, 0, 0,
                          info.internal_attr, info.external_attr, off)
        cd += name
    return bytes(cd)


def eocd(count, cd_size, cd_offset):
    return struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, count, count, cd_size, cd_offset, 0)


def v2_digest(sections):
    chunks = []
    for sec in sections:
        for i in range(0, len(sec), 1 << 20):
            c = sec[i:i + (1 << 20)]
            chunks.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(chunks)) + b''.join(chunks)).digest()


def lp(b):
    return struct.pack('<I', len(b)) + b


def openssl(args, stdin=b''):
    return subprocess.run(['openssl', *args], input=stdin, check=True, capture_output=True).stdout


def ensure_key(signing):
    key, cert = os.path.join(signing, 'key.pem'), os.path.join(signing, 'cert.der')
    if os.path.isfile(key) and os.path.isfile(cert):
        return key, cert
    os.makedirs(signing, exist_ok=True)
    pem = os.path.join(signing, 'cert.pem')
    openssl(['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', pem, '-days', '10950',
             '-subj', '/CN=WiiCompiled VR Steam Frame build'])
    openssl(['x509', '-in', pem, '-outform', 'DER', '-out', cert])
    os.chmod(key, 0o600)
    print(f'generated a signing key in {signing}; keep it, or later builds will not install over this one')
    return key, cert


def sign(entries_bytes, cd, eocd_bytes, key_path, cert_path):
    """The APK Signing Block: one v2 signer, RSA PKCS#1 v1.5 with SHA-256, through openssl."""
    cert = open(cert_path, 'rb').read()
    # The digest covers the EOCD with the central directory offset pointing at the signing block.
    eocd_for_digest = bytearray(eocd_bytes)
    struct.pack_into('<I', eocd_for_digest, 16, len(entries_bytes))
    digest = v2_digest([entries_bytes, cd, bytes(eocd_for_digest)])
    signed_data = lp(lp(struct.pack('<I', RSA_PKCS1_SHA256) + lp(digest))) + lp(lp(cert)) + lp(b'')
    signature = openssl(['dgst', '-sha256', '-sign', key_path], signed_data)
    pub = openssl(['pkey', '-in', key_path, '-pubout', '-outform', 'DER'])
    signer = lp(signed_data) + lp(lp(struct.pack('<I', RSA_PKCS1_SHA256) + lp(signature))) + lp(pub)
    value = lp(lp(signer))
    pair = struct.pack('<Q', 4 + len(value)) + struct.pack('<I', SIG_V2) + value
    size = len(pair) + 8 + 16
    return struct.pack('<Q', size) + pair + struct.pack('<Q', size) + b'APK Sig Block 42'


def kit_fingerprint(kit_json, replaced):
    """A new kit identity: the upstream one plus what this build put in its place."""
    h = hashlib.sha256((kit_json['fingerprint'] + '\n').encode())
    for name in sorted(replaced):
        h.update(f'{name} {hashlib.sha256(replaced[name]).hexdigest()}\n'.encode())
    return h.hexdigest()


def build(args):
    entries = read_zip(args.upstream)
    by_name = {e['info'].filename: e for e in entries}

    replaced = {}
    for root, _, files in os.walk(args.kit):
        for f in files:
            path = os.path.join(root, f)
            name = KIT + os.path.relpath(path, args.kit).replace(os.sep, '/')
            if name == KIT + 'kit.json':
                continue
            if name not in by_name:
                sys.exit(f'{name}: the upstream APK has no such kit file')
            replaced[name] = open(path, 'rb').read()
    if not replaced:
        sys.exit(f'{args.kit}: no kit files to put into the APK')

    with zipfile.ZipFile(args.upstream) as z:
        kit_json = json.loads(z.read(KIT + 'kit.json'))
        dex = z.read('classes.dex')
    upstream_fp = kit_json['fingerprint']
    kit_json['fingerprint'] = kit_fingerprint(kit_json, replaced)
    kit_json['steamFrame'] = {'upstreamKitFingerprint': upstream_fp,
                              'replaced': sorted(n[len(KIT):] for n in replaced)}
    replace(by_name[KIT + 'kit.json'], (json.dumps(kit_json, indent=2) + '\n').encode())
    for name, content in replaced.items():
        replace(by_name[name], content)
    replace(by_name['classes.dex'], dexpatch.patch(dex))

    key, cert = ensure_key(args.signing)
    entries_bytes, offsets = write_entries(entries)
    cd = central_directory(entries, offsets)
    block = sign(entries_bytes, cd, eocd(len(entries), len(cd), 0), key, cert)
    tail = eocd(len(entries), len(cd), len(entries_bytes) + len(block))
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(entries_bytes + block + cd + tail)
    print(f'{args.out}: kit fingerprint {kit_json["fingerprint"][:16]}... (upstream {upstream_fp[:16]}...), '
          f'{len(replaced)} kit files replaced, classes.dex patched, signed')
    verify(args.out)


def verify(path):
    """Checks the archive, the alignment and the v2 signature, as the package installer will."""
    data = open(path, 'rb').read()
    with zipfile.ZipFile(path) as z:
        bad = z.testzip()
        if bad:
            sys.exit(f'{path}: {bad} is corrupt')
        for info in z.infolist():
            if info.compress_type != zipfile.ZIP_STORED:
                continue
            nlen, xlen = struct.unpack_from('<HH', data, info.header_offset + 26)
            start = info.header_offset + 30 + nlen + xlen
            align = ALIGN_PAGE if info.filename.endswith('.so') else 4
            if start % align:
                sys.exit(f'{path}: {info.filename} is not {align}-byte aligned')
        count = len(z.infolist())
        kit = json.loads(z.read(KIT + 'kit.json'))
    e = data.rfind(b'PK\x05\x06')
    cd_size, cd_off = struct.unpack_from('<II', data, e + 12)
    cd, tail = data[cd_off:cd_off + cd_size], bytearray(data[e:])
    if data[cd_off - 16:cd_off] != b'APK Sig Block 42':
        sys.exit(f'{path}: no APK signing block')
    size, = struct.unpack_from('<Q', data, cd_off - 24)
    block_start = cd_off - size - 8
    p, found = block_start + 8, None
    while p < cd_off - 24:
        n, ident = struct.unpack_from('<QI', data, p)
        if ident == SIG_V2:
            found = data[p + 12:p + 8 + n]
        p += 8 + n
    if found is None:
        sys.exit(f'{path}: no v2 signature in the signing block')
    # value = lp(signers); signers = lp(signer); signer = lp(signed_data) lp(signatures) lp(public key)
    signers = found[4:4 + struct.unpack_from('<I', found)[0]]
    signer = signers[4:4 + struct.unpack_from('<I', signers)[0]]
    sd_len, = struct.unpack_from('<I', signer)
    signed_data = signer[4:4 + sd_len]
    q = 4 + sd_len
    sigs_len, = struct.unpack_from('<I', signer, q)
    sig = signer[q + 4:q + 4 + sigs_len]
    algo, = struct.unpack_from('<I', sig, 4)
    sig_len, = struct.unpack_from('<I', sig, 8)
    signature = sig[12:12 + sig_len]
    q += 4 + sigs_len
    pub_len, = struct.unpack_from('<I', signer, q)
    pub = signer[q + 4:q + 4 + pub_len]
    digests = signed_data[4:4 + struct.unpack_from('<I', signed_data)[0]]
    # one record: length, algorithm, digest length, digest
    digest = digests[12:12 + struct.unpack_from('<I', digests, 8)[0]]
    struct.pack_into('<I', tail, 16, block_start)
    if digest != v2_digest([data[:block_start], cd, bytes(tail)]):
        sys.exit(f'{path}: the signed digest does not match the archive')
    if algo != RSA_PKCS1_SHA256:
        sys.exit(f'{path}: signature algorithm {algo:#x} is not one this checker knows')
    with tempfile.TemporaryDirectory() as t:
        open(f'{t}/pub.der', 'wb').write(pub)
        open(f'{t}/sig', 'wb').write(signature)
        r = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', f'{t}/pub.der', '-keyform', 'DER',
                            '-signature', f'{t}/sig'], input=signed_data, capture_output=True)
    if r.returncode != 0:
        sys.exit(f'{path}: the v2 signature does not verify: {r.stdout.decode().strip()} {r.stderr.decode().strip()}')
    print(f'{path}: {count} entries, aligned, v2 signature valid; kit fingerprint {kit["fingerprint"][:16]}...'
          + (f', Steam Frame kit files: {", ".join(kit["steamFrame"]["replaced"])}' if 'steamFrame' in kit else ''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--upstream', required=True, help='the stock WiiCompiled-VR-Quest3-v1.3.1.apk')
    b.add_argument('--kit', required=True, help='directory with the objects/, libs/ and include/ that build.sh made')
    b.add_argument('--signing', required=True, help='directory holding key.pem and cert.der (created if missing)')
    b.add_argument('--out', required=True)
    v = sub.add_parser('verify')
    v.add_argument('apk')
    args = ap.parse_args()
    build(args) if args.cmd == 'build' else verify(args.apk)


if __name__ == '__main__':
    main()

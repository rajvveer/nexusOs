#!/usr/bin/env python3
"""UEFI Secure Boot enrollment + Authenticode signing, hand-built (no sbsign/virt-fw-vars/pip).

Produces, from the NexusOS PK/KEK/db key set (uefi/secureboot.py), a pre-enrolled OVMF variable store
(edk2 authenticated-variable NVRAM format) and an Authenticode-signed BOOTX64.EFI, so the firmware -> loader
link of the secure chain can be tested under QEMU + OVMF (edk2-x86_64-secure-code.fd) in SecureBoot mode.

  enroll-varstore TEMPLATE.fd OUT.fd [--loader-hash BOOTX64.EFI]   PK/KEK/db enrolled; optionally the loader's
                                                                   sha256 added to db (hash enrollment)
  sign-pe IN.EFI OUT.EFI                                           Authenticode-sign a PE with the db key
  verify-pe SIGNED.EFI [TRUSTED.der]                               offline edk2-style Authenticode check

Keys come from $SECUREBOOT_KEYDIR (default %USERPROFILE%\\.nexusos\\keys\\secureboot); db cert from uefi/keys/db.der.
"""
import hashlib, os, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KEYDIR = Path(os.environ.get('SECUREBOOT_KEYDIR', str(Path.home() / '.nexusos' / 'keys' / 'secureboot')))

# GUIDs (mixed-endian on-disk)
EFI_GLOBAL_VARIABLE = bytes.fromhex('61dfe48bca93d211aa0d00e098032b8c')
EFI_IMAGE_SECURITY_DATABASE_GUID = bytes.fromhex('cbb219d73a3d9645a3bcdad00e67656f')
EFI_CERT_X509_GUID = bytes.fromhex('a159c0a5e494a74a87b5ab155c2bf072')       # a5c059a1-94e4-4aa7-87b5-ab155c2bf072
EFI_CERT_SHA256_GUID = bytes.fromhex('2616c4c14c509240aca941f936934328')     # c1c41626-504c-4092-aca9-41f936934328
NEXUS_OWNER = bytes.fromhex('4e584f53000000000000000000000001')             # NexusOS signature owner
AUTH_VARSTORE_GUID = bytes.fromhex('782cf3aa7b949a43a1802e144ec37792')
VAR_ADDED = 0x3f
ATTR_NV_BS_RT_AT = 0x27   # NV | BS | RT | TIME_BASED_AUTHENTICATED_WRITE_ACCESS


def utf16(name):
    return name.encode('utf-16-le') + b'\x00\x00'


def sig_list_x509(cert_der, owner=NEXUS_OWNER):
    sig = owner + cert_der                     # EFI_SIGNATURE_DATA
    body = EFI_CERT_X509_GUID + struct.pack('<III', 28 + len(sig), 0, len(sig)) + sig
    return body


def sig_list_sha256(digest, owner=NEXUS_OWNER):
    sig = owner + digest                       # 16 + 32
    return EFI_CERT_SHA256_GUID + struct.pack('<III', 28 + len(sig), 0, len(sig)) + sig


def var_entry(name, guid, data, attributes=ATTR_NV_BS_RT_AT):
    name_u = utf16(name)
    # AUTHENTICATED_VARIABLE_HEADER: StartId, State, Reserved, Attributes, MonotonicCount, TimeStamp(16),
    # PubKeyIndex, NameSize, DataSize, VendorGuid, Name, Data
    ts = b'\x00' * 16   # EFI_TIME zero (pre-enrolled store contents are not re-verified by the firmware)
    hdr = struct.pack('<HBBIQ', 0x55AA, VAR_ADDED, 0, attributes, 0) + ts + \
          struct.pack('<III', 0, len(name_u), len(data)) + guid
    entry = hdr + name_u + data
    return entry + b'\xff' * ((-len(entry)) % 4)   # 4-byte alignment, pad with the store's erase value


def enroll_varstore(template_fd, out_fd, loader_hash=None):
    d = bytearray(Path(template_fd).read_bytes())
    vs = d.find(AUTH_VARSTORE_GUID)
    assert vs >= 0, 'auth variable store not found in template'
    store_size = struct.unpack_from('<I', d, vs + 16)[0]
    start = vs + 28
    # the store must be empty (all 0xFF) after the header
    assert all(b == 0xFF for b in d[start:start + 64]), 'template variable store is not empty'
    pk = (HERE / 'keys/PK.der').read_bytes()
    kek = (HERE / 'keys/KEK.der').read_bytes()
    db_cert = (HERE / 'keys/db.der').read_bytes()
    db_data = sig_list_x509(db_cert)
    if loader_hash:
        digest = authenticode_hash(Path(loader_hash).read_bytes())[0]   # SB matches the Authenticode hash, not the file hash
        db_data += sig_list_sha256(digest)
    entries = (var_entry('PK', EFI_GLOBAL_VARIABLE, sig_list_x509(pk)) +
               var_entry('KEK', EFI_GLOBAL_VARIABLE, sig_list_x509(kek)) +
               var_entry('db', EFI_IMAGE_SECURITY_DATABASE_GUID, db_data))
    assert len(entries) <= store_size - 28 - 64, 'enrolled variables exceed the store'
    d[start:start + len(entries)] = entries
    Path(out_fd).write_bytes(d)
    print(f'[secureboot-efi] enrolled PK/KEK/db into {out_fd} ({len(entries)} bytes of variables'
          + (', + loader hash in db' if loader_hash else '') + ')')


# ---------------- Authenticode PE signing ----------------
def _load_key():
    from cryptography.hazmat.primitives import serialization
    return serialization.load_pem_private_key((KEYDIR / 'db_private.pem').read_bytes(), password=None)


def _der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def _tlv(tag, value):
    return bytes([tag]) + _der_len(len(value)) + value


def _oid(dotted):
    parts = [int(x) for x in dotted.split('.')]
    first = 40 * parts[0] + parts[1]
    out = [first]
    for p in parts[2:]:
        if p == 0:
            out.append(0); continue
        stack = []
        while p:
            stack.append(p & 0x7f); p >>= 7
        stack = stack[::-1]
        for k in range(len(stack) - 1):
            stack[k] |= 0x80
        out.extend(stack)
    return _tlv(0x06, bytes(out))


def _seq(*items): return _tlv(0x30, b''.join(items))
def _set(*items): return _tlv(0x31, b''.join(items))
def _int(n):
    b = n.to_bytes((n.bit_length() + 8) // 8, 'big') or b'\x00'
    return _tlv(0x02, b)
def _octet(b): return _tlv(0x04, b)
def _explicit(tag, b): return bytes([0xA0 | tag]) + _der_len(len(b)) + b

OID_SPC_INDIRECT_DATA = '1.3.6.1.4.1.311.2.1.4'
OID_SPC_PE_IMAGE_DATA = '1.3.6.1.4.1.311.2.1.15'
OID_SHA256 = '2.16.840.1.101.3.4.2.1'
OID_RSA = '1.2.840.113549.1.1.1'
OID_SHA256_RSA = '1.2.840.113549.1.1.11'
OID_PKCS7_SIGNED = '1.2.840.113549.1.7.2'
OID_CONTENT_TYPE = '1.2.840.113549.1.9.3'
OID_MESSAGE_DIGEST = '1.2.840.113549.1.9.4'


def authenticode_hash(pe):
    """SHA-256 over the PE, skipping the optional-header CheckSum (4 bytes), the Security directory entry (8 bytes)
    and the attribute-certificate table itself, per the Authenticode spec."""
    pe_off = struct.unpack_from('<I', pe, 0x3c)[0]
    opt = pe_off + 24
    magic = struct.unpack_from('<H', pe, opt)[0]
    checksum_off = opt + 64
    datadir_off = opt + (112 if magic == 0x20b else 96)   # PE32+ vs PE32
    sec_entry = datadir_off + 4 * 8                        # directory[4] = Certificate Table
    sec_rva, sec_size = struct.unpack_from('<II', pe, sec_entry)
    size_of_headers = struct.unpack_from('<I', pe, opt + 60)[0]
    h = hashlib.sha256()
    h.update(pe[:checksum_off])
    h.update(pe[checksum_off + 4:sec_entry])
    h.update(pe[sec_entry + 8:size_of_headers])
    # the rest of the file, excluding the certificate table region
    cert_start = sec_rva if sec_rva else len(pe)
    h.update(pe[size_of_headers:cert_start])
    return h.digest(), sec_entry, size_of_headers


def _der_header_size(der):
    """Size of the tag + length octets of one DER TLV."""
    return 2 if der[1] < 0x80 else 2 + (der[1] & 0x7f)


def _der_children(b, start=0, end=None):
    """The (tag, tlv_start, value_start, value_end) of each DER TLV in b[start:end]."""
    end = len(b) if end is None else end
    out = []
    while start < end:
        tag, n, j = b[start], b[start + 1], start + 2
        if n & 0x80:
            k = n & 0x7f; n = int.from_bytes(b[j:j + k], 'big'); j += k
        out.append((tag, start, j, j + n)); start = j + n
    return out


OID_SPC_SP_OPUS_INFO = '1.3.6.1.4.1.311.2.1.12'
OID_SPC_STATEMENT_TYPE = '1.3.6.1.4.1.311.2.1.11'
OID_SPC_INDIVIDUAL_SP_KEY_PURPOSE = '1.3.6.1.4.1.311.2.1.21'


def spc_indirect_data(digest):
    """SpcIndirectDataContent as signtool emits it for a PE image:
       SEQ { SpcAttributeTypeAndOptionalValue { SPC_PE_IMAGE_DATA, SpcPeImageData }, DigestInfo { sha256, digest } }
       SpcPeImageData = SEQ { flags BIT STRING (empty), file [0] SpcLink = [2] SpcString = [0] BMPSTRING "<<<Obsolete>>>" }"""
    obsolete = '<<<Obsolete>>>'.encode('utf-16-be')
    spc_string = bytes([0x80]) + _der_len(len(obsolete)) + obsolete        # SpcString.unicode [0] IMPLICIT BMPSTRING
    spc_link = _explicit(2, spc_string)                                     # SpcLink.file [2] EXPLICIT SpcString
    spc_pe = _seq(_tlv(0x03, b'\x00'), _explicit(0, spc_link))             # flags (no bits), file [0] SpcLink
    spc_attr = _seq(_oid(OID_SPC_PE_IMAGE_DATA), spc_pe)
    digest_info = _seq(_seq(_oid(OID_SHA256), _tlv(0x05, b'')), _octet(digest))
    return _seq(spc_attr, digest_info)


def sign_pe(in_efi, out_efi, signer_cert=None, signer_key=None, extra_certs=()):
    """Authenticode-sign a PE/COFF image in the format edk2 (DxeImageVerificationLib + CryptAuthenticode.c) and
    signtool agree on:
    - the file is zero-padded to 8 bytes so the attribute-certificate table is quadword aligned; the padding is
      part of the hashed image (the hash covers everything before the table);
    - messageDigest = SHA-256 of the SpcIndirectDataContent *contents* (without its SEQUENCE tag + length): that
      is what edk2's AuthenticodeVerify hands PKCS7_verify as the detached content;
    - signed attributes {contentType = SPC_INDIRECT_DATA, SpcStatementType, SpcSpOpusInfo, messageDigest}, DER-sorted,
      signed as the re-tagged SET (0x31);
    - SignedData version 1, digestAlgorithms {sha256 NULL}, contentInfo SPC_INDIRECT_DATA;
    - WIN_CERTIFICATE {dwLength incl. the 8-byte header, revision 0x0200, type 0x0002 PKCS_SIGNED_DATA}, 8-aligned.
    The signer is the db cert itself (edk2 verifies with X509_V_FLAG_PARTIAL_CHAIN), or a cert issued by a db cert
    (signer_cert/signer_key; extra_certs adds the issuer to `certificates`). Returns the Authenticode hash."""
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import padding
    from cryptography import x509
    pe = bytearray(Path(in_efi).read_bytes())
    _, sec_entry, _ = authenticode_hash(bytes(pe))
    if struct.unpack_from('<II', pe, sec_entry) != (0, 0):
        raise ValueError(f'{in_efi} already carries a certificate table')
    pe += b'\x00' * ((-len(pe)) % 8)
    digest = authenticode_hash(bytes(pe))[0]
    spc = spc_indirect_data(digest)
    msg_digest = hashlib.sha256(spc[_der_header_size(spc):]).digest()
    signed_attrs = sorted([
        _seq(_oid(OID_CONTENT_TYPE), _set(_oid(OID_SPC_INDIRECT_DATA))),
        _seq(_oid(OID_SPC_STATEMENT_TYPE), _set(_seq(_oid(OID_SPC_INDIVIDUAL_SP_KEY_PURPOSE)))),
        _seq(_oid(OID_SPC_SP_OPUS_INFO), _set(_seq())),
        _seq(_oid(OID_MESSAGE_DIGEST), _set(_octet(msg_digest))),
    ])
    key = serialization.load_pem_private_key(Path(signer_key).read_bytes(), password=None) if signer_key else _load_key()
    signature = key.sign(_set(*signed_attrs), padding.PKCS1v15(), hashes.SHA256())
    cert_der = Path(signer_cert).read_bytes() if signer_cert else (HERE / 'keys/db.der').read_bytes()
    cert = x509.load_der_x509_certificate(cert_der)
    sid = _seq(cert.issuer.public_bytes(), _int(cert.serial_number))
    attrs = b''.join(signed_attrs)
    signer_info = _seq(
        _int(1), sid,
        _seq(_oid(OID_SHA256), _tlv(0x05, b'')),
        bytes([0xA0]) + _der_len(len(attrs)) + attrs,                                      # [0] IMPLICIT SET OF
        _seq(_oid(OID_RSA), _tlv(0x05, b'')),
        _octet(signature),
    )
    certs = b''.join([cert_der] + [Path(c).read_bytes() for c in extra_certs])
    signed_data = _seq(
        _int(1),
        _set(_seq(_oid(OID_SHA256), _tlv(0x05, b''))),
        _seq(_oid(OID_SPC_INDIRECT_DATA), _explicit(0, spc)),
        bytes([0xA0]) + _der_len(len(certs)) + certs,                                      # [0] IMPLICIT certificates
        _set(signer_info),
    )
    content_info = _seq(_oid(OID_PKCS7_SIGNED), _explicit(0, signed_data))
    cert_len = 8 + len(content_info)
    wincert = struct.pack('<IHH', cert_len, 0x0200, 0x0002) + content_info + b'\x00' * ((-cert_len) % 8)
    out = bytearray(bytes(pe) + wincert)
    struct.pack_into('<II', out, sec_entry, len(pe), len(wincert))
    Path(out_efi).write_bytes(out)
    print(f'[secureboot-efi] Authenticode-signed {in_efi} -> {out_efi} (cert table {len(wincert)} bytes at {len(pe)})')
    return digest


def verify_pe(efi, trusted_der):
    """Offline check of a signed PE the way edk2 does it: the WIN_CERTIFICATE table, the WrapPkcs7Data offsets, the
    hash OID at the fixed offset 32 (HashPeImageByType), the image hash = last 32 bytes of the SpcIndirectDataContent
    contents, messageDigest over those contents, the RSA signature over the re-tagged signed attributes, and the
    signer being the trusted cert or issued by it."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import padding
    from cryptography import x509
    pe = Path(efi).read_bytes()
    digest, sec_entry, _ = authenticode_hash(pe)
    va, size = struct.unpack_from('<II', pe, sec_entry)
    assert va and va % 8 == 0 and va + size == len(pe), ('cert table placement', va, size, len(pe))
    length, revision, kind = struct.unpack_from('<IHH', pe, va)
    assert 8 <= length <= size and revision == 0x0200 and kind == 0x0002, (length, revision, kind)
    auth = pe[va + 8:va + length]
    assert auth[1] == 0x82 and auth[4] == 0x06 and auth[15] == 0xA0 and auth[16] == 0x82, 'edk2 WrapPkcs7Data offsets'
    sha256_oid = _oid(OID_SHA256)[2:]
    assert auth[32:32 + len(sha256_oid)] == sha256_oid, 'edk2 HashPeImageByType: sha256 OID not at +32'
    ci = _der_children(auth, *_der_children(auth)[0][2:])              # OID, [0]
    sd = _der_children(auth, *_der_children(auth, *ci[1][2:])[0][2:])  # version, digestAlgs, contentInfo, [0] certs, signerInfos
    eci = _der_children(auth, *sd[2][2:])
    assert auth[eci[0][1]:eci[0][3]] == _oid(OID_SPC_INDIRECT_DATA), 'contentInfo is not SPC_INDIRECT_DATA'
    spc = _der_children(auth, *eci[1][2:])[0]
    spc_contents = auth[spc[2]:spc[3]]
    assert spc_contents[-32:] == digest, 'image hash in SpcIndirectDataContent does not match the file'
    certs = [auth[c[1]:c[3]] for c in _der_children(auth, *sd[3][2:])]
    si = _der_children(auth, *_der_children(auth, *sd[4][2:])[0][2:])
    attrs = auth[si[3][2]:si[3][3]]
    sig = auth[si[5][2]:si[5][3]]
    items = [attrs[a[1]:a[3]] for a in _der_children(attrs)]
    assert items == sorted(items), 'signed attributes are not DER-sorted'
    md = None
    for a in _der_children(attrs):
        kids = _der_children(attrs, *a[2:])
        if attrs[kids[0][1]:kids[0][3]] == _oid(OID_MESSAGE_DIGEST):
            inner = _der_children(attrs, *kids[1][2:])[0]
            md = attrs[inner[2]:inner[3]]
    assert md == hashlib.sha256(spc_contents).digest(), 'messageDigest != sha256(SpcIndirectDataContent contents)'
    signer = None
    for c in certs:
        cert = x509.load_der_x509_certificate(c)
        try:
            cert.public_key().verify(sig, _set(*items), padding.PKCS1v15(), hashes.SHA256()); signer = cert
        except Exception:
            pass
    assert signer is not None, 'no certificate in the signature verifies the signed attributes'
    trusted = x509.load_der_x509_certificate(Path(trusted_der).read_bytes())
    if signer.fingerprint(hashes.SHA256()) != trusted.fingerprint(hashes.SHA256()):
        trusted.public_key().verify(signer.signature, signer.tbs_certificate_bytes, padding.PKCS1v15(),
                                    signer.signature_hash_algorithm)
    return spc_contents


def main(argv):
    if not argv:
        print(__doc__); return 2
    if argv[0] == 'enroll-varstore':
        enroll_varstore(argv[1], argv[2], loader_hash=(argv[argv.index('--loader-hash') + 1] if '--loader-hash' in argv else None))
    elif argv[0] == 'sign-pe':
        sign_pe(argv[1], argv[2])
    elif argv[0] == 'verify-pe':
        verify_pe(argv[1], argv[2] if len(argv) > 2 else HERE / 'keys/db.der'); print('[secureboot-efi] signature OK')
    else:
        print(__doc__); return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))

"""Minimal OpenPGP (RFC 4880) for test package repositories: an RSA-3072 signing key (v4 public key + user ID + positive
self-certification, SHA-512) and clear-signed InRelease files, using the `cryptography` module. Test use only: the key
is generated per test run (apt's sqv accepts RSA >= 2048 with SHA-512).

    key = TestKey.generate('NexusOS test repository <test@nexus.invalid>')
    key.public_packets()              -> binary keyring for /etc/apt/keyrings/x.gpg (Signed-By)
    key.clearsign(text)               -> InRelease text
"""
import base64
import hashlib
import struct
import time
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa, utils


def mpi(value):
    raw = value.to_bytes((value.bit_length() + 7) // 8, 'big')
    return struct.pack('>H', value.bit_length()) + raw


def packet(tag, body):
    """New-format packet header."""
    n = len(body)
    if n < 192: length = bytes([n])
    elif n < 8384: n2 = n - 192; length = bytes([(n2 >> 8) + 192, n2 & 0xFF])
    else: length = b'\xff' + struct.pack('>I', n)
    return bytes([0xC0 | tag]) + length + body


def subpacket(kind, data):
    n = len(data) + 1
    if n < 192: return bytes([n, kind]) + data
    n2 = n - 192; return bytes([(n2 >> 8) + 192, n2 & 0xFF, kind]) + data


def crc24(data):
    crc = 0xB704CE
    for b in data:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000: crc ^= 0x1864CFB
    return crc & 0xFFFFFF


class TestKey:
    def __init__(self, private, uid, created):
        self.private, self.uid, self.created = private, uid, created
        numbers = private.public_key().public_numbers()
        self.key_body = b'\x04' + struct.pack('>I', created) + b'\x01' + mpi(numbers.n) + mpi(numbers.e)
        self.fingerprint = hashlib.sha1(b'\x99' + struct.pack('>H', len(self.key_body)) + self.key_body).digest()
        self.key_id = self.fingerprint[-8:]

    @classmethod
    def generate(cls, uid, created=None):
        return cls(rsa.generate_private_key(public_exponent=65537, key_size=3072), uid, int(created or time.time()) - 60)

    def _signature(self, sig_type, hashed_data):
        # the self-certification (0x13) is dated at the key's creation, so it precedes every data signature (sqv needs a
        # binding valid at each signature's time, however late public_packets() is called)
        when = self.created if sig_type == 0x13 else int(time.time()) - 30
        hashed_sub = subpacket(2, struct.pack('>I', when)) + subpacket(33, b'\x04' + self.fingerprint)
        if sig_type == 0x13: hashed_sub += subpacket(27, b'\x03')
        head = bytes([4, sig_type, 1, 10]) + struct.pack('>H', len(hashed_sub)) + hashed_sub
        trailer = b'\x04\xff' + struct.pack('>I', len(head))
        digest = hashlib.sha512(hashed_data + head + trailer).digest()
        signature = self.private.sign(digest, padding.PKCS1v15(), utils.Prehashed(hashes.SHA512()))
        unhashed = subpacket(16, self.key_id)
        body = head + struct.pack('>H', len(unhashed)) + unhashed + digest[:2] + mpi(int.from_bytes(signature, 'big'))
        return packet(2, body)

    def public_packets(self):
        uid = self.uid.encode()
        key_packet = packet(6, self.key_body)
        hashed = b'\x99' + struct.pack('>H', len(self.key_body)) + self.key_body + b'\xb4' + struct.pack('>I', len(uid)) + uid
        return key_packet + packet(13, uid) + self._signature(0x13, hashed)

    def detach_sign(self, data):
        """Binary detached signature (type 0x00, binary document) over data, as sqv/gpgv verify it."""
        return self._signature(0x00, data)

    def save(self):
        """JSON text with the private key (PKCS#8 PEM), the user ID and the creation time (part of the fingerprint)."""
        import json
        from cryptography.hazmat.primitives import serialization
        pem = self.private.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        return json.dumps({'format': 'nexusos-openpgp-rsa-1', 'uid': self.uid, 'created': self.created, 'pem': pem.decode()}, indent=1)

    @classmethod
    def load(cls, text):
        import json
        from cryptography.hazmat.primitives import serialization
        data = json.loads(text)
        if data.get('format') != 'nexusos-openpgp-rsa-1': raise ValueError('not a NexusOS signing key file')
        return cls(serialization.load_pem_private_key(data['pem'].encode(), password=None), data['uid'], int(data['created']))

    def clearsign(self, text):
        """Clear-signed text document (signature type 0x01 over the canonical text)."""
        lines = text.split('\n')
        if lines and lines[-1] == '': lines.pop()
        canonical = '\r\n'.join(line.rstrip(' \t') for line in lines).encode('utf-8')
        sig = self._signature(0x01, canonical)
        armored = base64.b64encode(sig).decode()
        body = '\n'.join(armored[i:i + 64] for i in range(0, len(armored), 64))
        check = base64.b64encode(struct.pack('>I', crc24(sig))[1:]).decode()
        escaped = '\n'.join(('- ' + line) if line.startswith('-') else line for line in lines)
        return ('-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\n' + escaped +
                '\n-----BEGIN PGP SIGNATURE-----\n\n' + body + '\n=' + check + '\n-----END PGP SIGNATURE-----\n')

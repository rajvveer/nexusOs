"""Dump the certificate chain + TLS parameters a host serves (host-side check of what
NexusOS will see). usage: python -I chaindump.py host [host...]"""
import socket, ssl, sys
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import rsa, ec

def dump(host, port=443):
    ctx = ssl.create_default_context()
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.set_alpn_protocols(["http/1.1"])
    with socket.create_connection((host, port), timeout=10) as s:
        with ctx.wrap_socket(s, server_hostname=host) as ss:
            print("==", host, ss.version(), ss.cipher(), "alpn", ss.selected_alpn_protocol())
            chain = ss.get_verified_chain()
            for c in chain:
                cert = x509.load_der_x509_certificate(c.public_bytes(ssl._ssl.ENCODING_DER) if hasattr(c, "public_bytes") else c)
                k = cert.public_key()
                kt = ("RSA%d" % k.key_size) if isinstance(k, rsa.RSAPublicKey) else ("EC " + k.curve.name if isinstance(k, ec.EllipticCurvePublicKey) else type(k).__name__)
                print("   subj:", cert.subject.rfc4514_string())
                print("   iss :", cert.issuer.rfc4514_string())
                print("   key :", kt, " sig:", cert.signature_algorithm_oid._name, " valid:", cert.not_valid_before_utc.date(), "->", cert.not_valid_after_utc.date())

for h in sys.argv[1:]:
    try: dump(h)
    except Exception as e: print("==", h, "ERROR", type(e).__name__, e)

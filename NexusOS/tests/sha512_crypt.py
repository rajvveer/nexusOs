"""SHA-512-crypt ($6$, glibc / Drepper's specification) for test fixtures: sha512_crypt(password, salt[, rounds])."""
import hashlib

B64 = './0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'
ORDER = [(0, 21, 42), (22, 43, 1), (44, 2, 23), (3, 24, 45), (25, 46, 4), (47, 5, 26), (6, 27, 48), (28, 49, 7), (50, 8, 29), (9, 30, 51), (31, 52, 10),
         (53, 11, 32), (12, 33, 54), (34, 55, 13), (56, 14, 35), (15, 36, 57), (37, 58, 16), (59, 17, 38), (18, 39, 60), (40, 61, 19), (62, 20, 41)]


def sha512_crypt(password, salt, rounds=None):
    key, salt_bytes = password.encode(), salt.encode()[:16]
    b = hashlib.sha512(key + salt_bytes + key).digest()
    a = hashlib.sha512(key + salt_bytes)
    n = len(key)
    while n > 64: a.update(b); n -= 64
    a.update(b[:n])
    n = len(key)
    while n: a.update(b if n & 1 else key); n >>= 1
    a = a.digest()
    dp = hashlib.sha512(key * len(key)).digest()
    p = (dp * (len(key) // 64 + 1))[:len(key)]
    ds = hashlib.sha512(salt_bytes * (16 + a[0])).digest()
    s = ds[:len(salt_bytes)]
    for r in range(rounds or 5000):
        c = hashlib.sha512()
        c.update(p if r & 1 else a)
        if r % 3: c.update(s)
        if r % 7: c.update(p)
        c.update(a if r & 1 else p)
        a = c.digest()
    out = ''
    for i, j, k in ORDER:
        w = a[i] << 16 | a[j] << 8 | a[k]
        for _ in range(4): out += B64[w & 63]; w >>= 6
    w = a[63]
    for _ in range(2): out += B64[w & 63]; w >>= 6
    prefix = '$6$' + ('rounds=%d$' % rounds if rounds else '') + salt_bytes.decode()
    return prefix + '$' + out


if __name__ == '__main__':   # the specification's examples
    assert sha512_crypt('Hello world!', 'saltstring') == '$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1'
    assert sha512_crypt('Hello world!', 'saltstringsaltstring', 10000) == '$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/YTBmSK6H9qs/y3RnOaw5v.'
    print('sha512_crypt: specification vectors OK')

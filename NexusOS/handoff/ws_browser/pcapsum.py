"""Summarise TCP segments of a QEMU filter-dump pcap for one server port.
usage: python -I pcapsum.py <file.pcap> <server_port> [max_lines]"""
import sys, struct
data = open(sys.argv[1], "rb").read()
sport = int(sys.argv[2]); maxl = int(sys.argv[3]) if len(sys.argv) > 3 else 400
magic = struct.unpack("<I", data[:4])[0]
end = "<" if magic in (0xa1b2c3d4, 0xa1b23c4d) else ">"
off = 24; t0 = None; n = 0; isn = {}
while off + 16 <= len(data) and n < maxl:
    ts_s, ts_us, incl, orig = struct.unpack(end + "IIII", data[off:off+16]); off += 16
    pkt = data[off:off+incl]; off += incl
    t = ts_s + ts_us / 1e6
    if t0 is None: t0 = t
    if len(pkt) < 34 or pkt[12:14] != b"\x08\x00": continue
    ihl = (pkt[14] & 15) * 4
    if pkt[23] != 6: continue
    ip = pkt[14:14+ihl]; tot = struct.unpack(">H", ip[2:4])[0]
    src = ".".join(str(b) for b in ip[12:16]); dst = ".".join(str(b) for b in ip[16:20])
    tcp = pkt[14+ihl:14+tot]
    sp, dp, seq, ack, doff_flags, win = struct.unpack(">HHIIHH", tcp[:16])
    doff = (doff_flags >> 12) * 4; flags = doff_flags & 0x1ff
    if sp != sport and dp != sport: continue
    plen = len(tcp) - doff
    fl = "".join(c for c, b in zip("FSRPAU", [1, 2, 4, 8, 16, 32]) if flags & b)
    key = (src, sp)
    if flags & 2: isn[key] = seq
    rseq = seq - isn.get(key, seq); rack = ack - isn.get((dst, dp), ack)
    who = "S>" if sp == sport else "C>"
    print(f"{t - t0:8.3f} {who} {fl:5s} seq={rseq:<7d} ack={rack:<7d} len={plen:<5d} win={win}")
    n += 1

"""host_<g>.txt (tools/hostreplay, one command word per line) vs
rtl_<g>.bin (tb_geom_cpu's captured stream) must match word for word, and the
frames the RDP model drew from each (hostfb_/rtlfb_<g>.bin) byte for byte."""
import struct, sys
g = sys.argv[1]
host = [int(l, 16) for l in open('host_%s.txt' % g) if l.strip()]
raw = open('rtl_%s.bin' % g, 'rb').read()
rtl = list(struct.unpack('<%dI' % (len(raw) // 4), raw))
first = next((i for i, (a, b) in enumerate(zip(host, rtl)) if a != b), None)
hf, rf = open('hostfb_%s.bin' % g, 'rb').read(), open('rtlfb_%s.bin' % g, 'rb').read()
px = sum(1 for i in range(0, min(len(hf), len(rf)), 2) if hf[i:i+2] != rf[i:i+2])
bg = hf.count(hf[:2]) if hf else 0
print("%s: host %d words, rtl %d words, first diff %s, %d fb pixels differ" %
      (g, len(host), len(rtl), first, px))
if first is not None:
    print("  word %d: host %08x rtl %08x (context host %s)" %
          (first, host[first], rtl[first], ' '.join('%08x' % w for w in host[max(0, first - 4):first + 4])))
ok = len(host) == len(rtl) and first is None and px == 0 and len(hf) == len(rf) and len(hf) > 0
sys.exit(0 if ok else 1)

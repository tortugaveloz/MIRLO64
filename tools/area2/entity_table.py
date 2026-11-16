#!/usr/bin/env python3
# Compact "Fitter Resource Utilization by Entity": name, ALMs total(self), regs total(self), M10K, DSP, mem bits
import sys,re
f=sys.argv[1]; thr=float(sys.argv[2]) if len(sys.argv)>2 else 15
lines=open(f,errors='ignore').read().split('\n')
st=None
for i,l in enumerate(lines):
    if 'Fitter Resource Utilization by Entity' in l and l.startswith(';'): st=i
rows=[]
stack=[]
for l in lines[st+4:]:
    if not l.startswith(';'): break
    c=[x.strip() for x in l.split(';')[1:-1]]
    name=c[0]
    def tv(s):
        m=re.match(r'(-?[\d.]+) \((-?[\d.]+)\)',s); return (float(m.group(1)),float(m.group(2))) if m else (float(s),0)
    alm=tv(c[1]); reg=tv(c[7]); 
    rows.append((name,alm,reg,int(c[10]),int(c[11]),int(c[9])))
for n,a,r,m,d,b in rows:
    if a[0]>=thr or m>=4 or d>=1:
        print(f"{n[:46]:46s} ALM {a[0]:8.1f} ({a[1]:7.1f})  reg {r[0]:6.0f} ({r[1]:6.0f})  M10K {m:3d} DSP {d:2d}")

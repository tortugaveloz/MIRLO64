#!/usr/bin/env python3
# closure.py <output_files dir>...: the complete sta.summary (all 4 corner models): worst slack by kind and clock group
import os,re,sys
for d in sys.argv[1:]:
    f=d.rstrip('/')+'/'+os.environ.get('REV','mirlo')+'.sta.summary' if not d.endswith('.summary') else d
    t=open(f).read().split('\n\n')
    n=0; w={}
    for b in t:
        m=re.search(r"Type\s*:\s*(\S+ \S+ \S+ Model) (.*?) '(.*)'\nSlack\s*:\s*(\S+)",b)
        if not m: continue
        n+=1
        model,kind,clk,sl=m.groups(); sl=float(sl)
        c=re.search(r"general\[(\d)\]",clk)
        c='cpu' if c and c.group(1)=='5' else 'sys' if c and c.group(1)=='0' else 'sys2x' if c and c.group(1)=='1' else 'vid' if c and c.group(1)=='3' else clk.split('|')[-1][:14]
        k=(kind,c)
        if k not in w or sl<w[k][0]: w[k]=(sl,model)
    worst=min(w.items(),key=lambda x:x[1][0])
    su=' '.join(f"{c}:{v[0]:+.3f}" for (k,c),v in sorted(w.items()) if k=='Setup' and c in ('cpu','sys','sys2x'))
    ho=' '.join(f"{c}:{v[0]:+.3f}" for (k,c),v in sorted(w.items()) if k=='Hold' and c in ('cpu','sys','sys2x'))
    print(f"{d.split('/')[-2] if '/' in d else d}: {n} entries; worst {worst[1][0]:+.3f} ({worst[0][0]} {worst[0][1]}, {worst[1][1]}); setup {su}; hold {ho}")

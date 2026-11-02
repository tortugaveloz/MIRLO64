#!/bin/bash
# clos.sh <projects dir>...: lines of sta.summary, worst slack, the negative ones, ALMs
for d in "$@"; do f=$d/output_files/mirlo.sta.summary
echo "$(basename $d) lines=$(grep -c '^Slack' $f) min=$(grep '^Slack' $f | awk '{print $3}' | sort -g | head -1) neg=$(grep '^Slack' $f | awk '$3<0' | wc -l) $(grep -h 'Logic util' $d/output_files/mirlo.fit.summary)"; done

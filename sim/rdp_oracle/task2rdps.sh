#!/bin/sh
# A graphics task dump (sim/rsp's RSPT format) as a stream: its RDRAM as the task
# starts, and the RDP commands the RSP's reference model gives for it (through
# sim/rdp/rdp_task, built here). The RSP's own writes to RDRAM while it runs are
# not in the stream (the commands' textures and images are the CPU's, before it).
#   ./task2rdps.sh task_gfx_000.bin > frame.rdps
set -e
here=$(cd "$(dirname "$0")" && pwd)
[ -x "$here/build/rdp_task" ] || make -s -C "$here" build/rdp_task
dump=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
( cd "$tmp" && RDP_TRACE="$tmp/trace" "$here/build/rdp_task" -o "$tmp/frame" "$dump" > /dev/null )
echo "# $1: the RDP commands of the task"
echo "rdram file $dump 16"
sed -e 's/ px=[0-9]*$//' -e 's/^[0-9a-f][0-9a-f] [0-9]* /cmd /' "$tmp/trace.0"

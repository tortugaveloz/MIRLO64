#!/bin/sh
# Fetch the oracle (an independent pixel-exact software RDP) into build/oracle/
# at the repository's root, at a pinned commit, and apply oracle_hooks.patch
# (the LOD hooks rdpo reads). Downloaded code stays in that directory: nothing
# is built or run from inside it (the Makefile compiles it from here).
#   ./setup.sh [URL]    (default: the repository README.md names)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
dst="$root/build/oracle/src"
url=${1:-$(sed -n 's|.*<\(https://github.com/[^>]*\)>.*|\1.git|p' "$here/README.md" | head -1)}
rev=9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc
[ -n "$url" ] || { echo "setup.sh: no repository URL (README.md, or pass one)" >&2; exit 1; }
if [ ! -d "$dst/.git" ]; then
    mkdir -p "$root/build/oracle"
    git clone --quiet "$url" "$dst"
fi
git -C "$dst" fetch --quiet origin "$rev" 2>/dev/null || true
git -C "$dst" reset --quiet --hard "$rev"
git -C "$dst" apply "$here/oracle_hooks.patch"
echo "oracle at $dst ($rev), hooks applied"

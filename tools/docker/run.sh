#!/bin/sh
# Runs a command in the mirlo64-tools image with the repository mounted at the
# same path, as the calling user, so the files it writes are the user's:
#   tools/docker/run.sh make -C lang/n64rt
set -eu
R="$(cd "$(dirname "$0")/../.." && pwd)"
TTY=
[ -t 1 ] && TTY=-t              # a TTY only for a terminal: it turns a piped log's line ends into CR LF
# --pull never: a missing image is an error to build it, not a pull of a same-named one from a registry
exec docker run --rm $TTY --pull never --security-opt no-new-privileges -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$R:$R" -w "$R" mirlo64-tools "$@"

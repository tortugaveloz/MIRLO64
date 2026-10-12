#!/bin/bash
# The nightly build's runner, in a container (tools/ci/Containerfile), on a machine that has Quartus Prime Lite 25.1std.
#
#   tools/ci/runner.sh build                         builds the image
#   tools/ci/runner.sh register <token> [<name>]     registers the runner with the repository: <token> is a runner
#                                                    registration token (the repository's Settings > Actions > Runners >
#                                                    New self-hosted runner shows one)
#   tools/ci/runner.sh run                           runs it (in the foreground: until it is stopped)
#   tools/ci/runner.sh sh [<command>...]             a shell, or a command, in the same container (/src: this tree)
#
# QUARTUS_DIR is Quartus' directory, the one that has bin/ (default: ~/altera/25.1std/quartus); REPO the repository's
# URL (default: https://github.com/tortugaveloz/MIRLO64); ENGINE podman or docker. The container sees Quartus (read-only)
# and its own volume, mirlo-runner, and nothing else of the machine.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ENGINE=${ENGINE:-$(command -v podman >/dev/null && echo podman || echo docker)}
QUARTUS_DIR=${QUARTUS_DIR:-$HOME/altera/25.1std/quartus}
REPO=${REPO:-https://github.com/tortugaveloz/MIRLO64}
RUN="$ENGINE run --rm --name mirlo-runner --hostname mirlo-nightly -v $QUARTUS_DIR:/opt/quartus:ro -v mirlo-runner:/runner"
case "${1:-}" in
    build)    $ENGINE build -t mirlo-runner "$HERE" ;;
    register) $RUN mirlo-runner register "$REPO" "$2" "${3:-mirlo-nightly}" ;;
    run)      exec $RUN mirlo-runner run ;;
    sh)       shift; exec $ENGINE run --rm -it --hostname mirlo-nightly -v "$QUARTUS_DIR":/opt/quartus:ro -v "$HERE/../..":/src -w /src mirlo-runner "${@:-bash}" ;;
    *)        sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac

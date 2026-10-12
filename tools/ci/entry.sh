#!/bin/bash
# The runner container's entry (tools/ci/Containerfile): `register <repository's URL> <registration token> [<name>]` once,
# then `run`. Anything else is run as a command (a shell, a build by hand).
set -e
if [ ! -x /runner/config.sh ]; then cp -a /opt/actions-runner/. /runner/; fi
case "${1:-run}" in
    register)
        cd /runner
        ./config.sh --unattended --replace --url "$2" --token "$3" --name "${4:-mirlo-nightly}" --labels quartus --work _work
        ;;
    run)
        cd /runner
        exec ./run.sh
        ;;
    *)
        exec "$@"
        ;;
esac

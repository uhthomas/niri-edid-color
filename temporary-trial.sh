#!/bin/sh

# Run through pkexec after GammaStep is active. This script applies the KMS
# transform, leaves it visible briefly, then resets KMS and terminates precisely
# the GammaStep process supplied by the caller. The EXIT trap is the rollback.

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: temporary-trial.sh SECONDS GAMMASTEP_PID" >&2
    exit 2
fi

trial_seconds=$1
gamma_pid=$2

case "$trial_seconds" in
    ''|*[!0-9]*)
        echo "SECONDS must be an integer" >&2
        exit 2
        ;;
esac
if [ "$trial_seconds" -lt 5 ] || [ "$trial_seconds" -gt 120 ]; then
    echo "SECONDS must be between 5 and 120" >&2
    exit 2
fi

case "$gamma_pid" in
    ''|*[!0-9]*)
        echo "GAMMASTEP_PID must be an integer" >&2
        exit 2
        ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
helper="$script_dir/niri-edid-color"
applied=0

rollback()
{
    exit_status=$?
    trap - EXIT HUP INT TERM

    if [ "$applied" -eq 1 ]; then
        echo "trial complete: resetting KMS colour state" >&2
        if ! "$helper" reset --takeover; then
            echo "WARNING: automatic KMS reset failed; reboot clears the volatile state" >&2
            exit_status=1
        fi
    fi

    kill -TERM "$gamma_pid" 2>/dev/null || true
    exit "$exit_status"
}

trap rollback EXIT HUP INT TERM

"$helper" apply --takeover
applied=1
echo "temporary colour trial active for $trial_seconds seconds" >&2
sleep "$trial_seconds"


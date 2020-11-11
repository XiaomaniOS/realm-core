#!/bin/bash

set -o errexit
set -o pipefail

CURL=${CURL:=curl}
STITCH_PID_FILE=$1
RETRY_COUNT=${2:-36}

WAIT_COUNTER=0
until $CURL --output /dev/null --retry-connrefused --head --fail http://localhost:9090 --silent; do
    pgrep -F $STITCH_PID_FILE > /dev/null
    if [[ -f $STITCH_PID_FILE && $? != 0 ]]; then
        echo "Stitch $(< $STITCH_PID_FILE) is not running"
        exit 1
    fi

    WAIT_COUNTER=$(($WAIT_COUNTER + 1 ))
    if [[ $WAIT_COUNTER = $RETRY_COUNT ]]; then
        echo "Timed out waiting for stitch to start"
        exit 1
    fi

    sleep 5
done

#!/bin/bash
set -o errexit
set -o pipefail

BASE_PATH=$(cd $(dirname "$0"); pwd)

REALPATH=$BASE_PATH/realpath.sh

WORK_PATH=$($REALPATH $1)
cd $WORK_PATH
export PATH=$WORK_PATH/go/bin:$PATH
export GOROOT=$WORK_PATH/go

if ! (which go > /dev/null); then
    echo "Go installation not found in $WORK_PATH"
    exit 1
fi

MONGOD=$WORK_PATH/mongodb-binaries/bin/mongod
if [[ ! -f $MONGOD || ! -x $MONGOD ]]; then
    echo "MongoDB installation not found in $WORK_PATH"
    exit 1
fi

if [[ ! -d $WORK_PATH/mongodb-dbpath ]]; then
    echo "MongoDB dbpath not found in $WORK_PATH"
    exit 1
fi

function cleanup() {
    if [[ -f $WORK_PATH/stitch_server.pid ]]; then
        PIDS_TO_KILL="$(< $WORK_PATH/stitch_server.pid)"
    fi

    if [[ -f $WORK_PATH/mongod.pid ]]; then
        PIDS_TO_KILL="$(< $WORK_PATH/mongod.pid) $PIDS_TO_KILL"
    fi

    if [[ -n "$PIDS_TO_KILL" ]]; then
        echo "Killing $PIDS_TO_KILL"
        kill $PIDS_TO_KILL
        echo "Waiting for processes to exit"
        wait
    fi
}

trap "exit" INT TERM ERR
trap cleanup EXIT

echo "Starting mongodb"
ulimit -n 32000
[[ -f $WORK_PATH/mongodb-dbpath/mongod.pid ]] && rm $WORK_PATH/mongodb-path/mongod.pid
./mongodb-binaries/bin/mongod \
    --replSet rs \
    --bind_ip_all \
    --port 26000 \
    --logpath $WORK_PATH/mongodb-dbpath/mongod.log \
    --logappend \
    --dbpath $WORK_PATH/mongodb-dbpath/ \
    --pidfilepath $WORK_PATH/mongod.pid &

./mongodb-binaries/bin/mongo \
    --nodb \
    --eval 'assert.soon(function(x){try{var d = new Mongo("localhost:26000"); return true}catch(e){return false}}, "timed out connecting")' \
> /dev/null

export LD_LIBRARY_PATH=$WORK_PATH/baas/etc/dylib/lib:$LD_LIBRARY_PATH
export PATH=$WORK_PATH/baas_dep_binaries:$PATH

cd $WORK_PATH/baas
[[ -d tmp ]] || mkdir tmp

echo "Starting stitch app server"
[[ -f $WORK_PATH/stitch_server.pid ]] && rm $WORK_PATH/stitch_server.pid
$WORK_PATH/stitch_server \
    --configFile=etc/configs/test_config.json 2>&1 > $WORK_PATH/stitch_server.log &
echo $! > $WORK_PATH/stitch_server.pid

wait

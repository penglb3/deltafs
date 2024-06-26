#!/bin/bash
set -xe

rm -rf /tmp/deltafs_outputs
OMPI_COMM_WORLD_NODE_RANK=${OMPI_COMM_WORLD_NODE_RANK:-0}
N_THREADS=${3:-1}

BASE_IP=$1
BASE_PORT=10101
N_SERVERS=${2:-1}
LAST_PORT=$(($N_SERVERS + $BASE_PORT - 1))
PORT=$(($OMPI_COMM_WORLD_NODE_RANK + $BASE_PORT))

FORMAT="$BASE_IP:%g"

export DELTAFS_MetadataSrvAddrs=$(seq -s'&' -f "$FORMAT" $BASE_PORT $LAST_PORT)
export DELTAFS_NumOfMetadataSrvs=$N_SERVERS
export DELTAFS_NumOfThreads=$N_THREADS

mpirun -n $N_SERVERS \
numactl --all -C 0-$N_SERVERS -- \
deltafs-srvr -v=1 -logtostderr

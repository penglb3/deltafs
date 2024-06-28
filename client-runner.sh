#!/bin/bash
set -ex
RANK=${OMPI_COMM_WORLD_RANK:-0}
N_CLIENTS=${OMPI_COMM_WORLD_SIZE:-1}
N_META=${N_META:-1}
IP=10.10.1.7
BASE_PORT=10101
CLIENT_PER_META=$(( $N_CLIENTS / $N_META ))
PORT=$(( $BASE_PORT + $RANK / $CLIENT_PER_META ))
LAST_PORT=$(( $BASE_PORT + $N_META - 1 ))
export LD_PRELOAD=/usr/local/lib/libdeltafs-hook.so
export DELTAFS_NumOfMetadataSrvs=$N_META
FORMAT="$IP:%g"
# export DELTAFS_MetadataSrvAddrs=$IP:$PORT
ADDRS=$(seq -s'&' -f "$FORMAT" $BASE_PORT $LAST_PORT)
export DELTAFS_MetadataSrvAddrs="$ADDRS"
~/mdtest $@

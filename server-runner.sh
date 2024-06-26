#!/bin/bash
rm -rf /tmp/deltafs_outputs
OMPI_COMM_WORLD_NODE_RANK=${OMPI_COMM_WORLD_NODE_RANK:-0}
IP="10.10.1.7"
BASE_PORT=10101
PORT=$(($OMPI_COMM_WORLD_NODE_RANK + $BASE_PORT))
LAST_PORT=$(($OMPI_COMM_WORLD_SIZE + $BASE_PORT - 1))
FORMAT="$IP:%g"
export DELTAFS_MetadataSrvAddrs=$((seq -s'&' -f "$FORMAT" $BASE_PORT $LAST_PORT))
numactl --all -C $OMPI_COMM_WORLD_NODE_RANK -- deltafs-srvr -v=1 -logtostderr

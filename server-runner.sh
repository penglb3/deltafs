#!/bin/bash
rm -rf /tmp/deltafs_outputs*
rm -rf /tmp/deltafs_runs*
OMPI_COMM_WORLD_NODE_RANK=${OMPI_COMM_WORLD_NODE_RANK:-0}
IP=${1:-"10.10.1.7"}
BASE_PORT=${2:-"10101"}
PORT=$(($OMPI_COMM_WORLD_NODE_RANK + $BASE_PORT))
LAST_PORT=$(($OMPI_COMM_WORLD_SIZE + $BASE_PORT - 1))
FORMAT="$IP:%g"

export DELTAFS_NumOfMetadataSrvs=1
export DELTAFS_NumOfVirMetadataSrvs=1
export DELTAFS_InstanceId=0
export DELTAFS_NumOfThreads=${3:-0}
export DELTAFS_Outputs=/dev/shm/deltafs_outputs-$OMPI_COMM_WORLD_NODE_RANK
export DELTAFS_RunDir=/dev/shm/deltafs_runs-$OMPI_COMM_WORLD_NODE_RANK
export DELTAFS_MetadataSrvAddrs=$IP:$PORT
rm -rf $DELTAFS_Outputs
# numactl --all -C $OMPI_COMM_WORLD_NODE_RANK -- \
deltafs-srvr -v=1 -logtostderr

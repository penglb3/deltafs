#!/bin/bash
rm -rf /tmp/deltafs_outputs*
rm -rf /tmp/deltafs_runs*
OMPI_COMM_WORLD_NODE_RANK=${OMPI_COMM_WORLD_NODE_RANK:-0}
IP="10.10.1.7"
BASE_PORT=10101
PORT=$(($OMPI_COMM_WORLD_NODE_RANK + $BASE_PORT))
LAST_PORT=$(($OMPI_COMM_WORLD_SIZE + $BASE_PORT - 1))
FORMAT="$IP:%g"

export DELTAFS_NumOfMetadataSrvs=1
export DELTAFS_NumOfVirMetadataSrvs=1
export DELTAFS_InstanceId=0
export DELTAFS_NumOfThreads=16
export DELTAFS_Outputs=/tmp/deltafs_outputs-$OMPI_COMM_WORLD_NODE_RANK
export DELTAFS_RunDir=/tmp/deltafs_runs-$OMPI_COMM_WORLD_NODE_RANK
export DELTAFS_MetadataSrvAddrs=$IP:$PORT
numactl --all -C $OMPI_COMM_WORLD_NODE_RANK -- deltafs-srvr -v=1 -logtostderr

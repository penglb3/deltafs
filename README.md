# DeltaFS (that actually runs)
Original repo: https://github.com/pdlfs/deltafs

## Building
Tested under Ubuntu 22.04 with gcc 11.4.0

### Install Dependencies
I tend to use openmpi, you can also use mpich if you like.
```bash
sudo apt install gcc g++ make autoconf automake libtool pkg-config
sudo apt install cmake cmake-curses-gui checkinstall
sudo apt install libsnappy-dev libgflags-dev libgoogle-glog-dev
sudo apt install libopenmpi-dev libjson-c-dev
```

### Install libfabric
**DONT USE APT TO DO THIS** in Ubuntu 22.04, that one's version is too old. Compile from source.
```bash
wget https://github.com/ofiwg/libfabric/releases/download/v1.21.0/libfabric-1.21.0.tar.bz2
tar -xjf libfabric-1.21.0.tar.bz2
cd libfabric-1.21.0
./configure && make -j
sudo checkinstall
# or you can do `sudo make install` if you don't want to `apt install checkinstall`
```

### Install mercury RPC framework
```bash
# mercury wants this
sudo bash -c "echo 0 > /proc/sys/kernel/yama/ptrace_scope"
git clone --recurse-submodules https://github.com/mercury-hpc/mercury.git
# My version: https://github.com/mercury-hpc/mercury/tree/73df83f039971575cd04b6be58402bafe54da05a
cd mercury
mkdir build && cd build
ccmake -DMERCURY_USE_OFI=ON ..
make -j
sudo checkinstall -pkgname mercury
```

### Get DeltaFS source code and compile
```bash
# OF COURSE YOU ARE GOING TO USE MY FIXED VERSION.
git clone https://github.com/penglb3/deltafs.git
cd deltafs
mkdir build && cd build
ccmake -DDELTAFS_COMMON_INTREE=ON -DDELTAFS_MPI=ON -DPDLFS_MERCURY_RPC=ON -DPDLFS_GFLAGS=ON -DPDLFS_GLOG=ON -DPDLFS_SNAPPY=ON ..
make -j
```

## Running
Server side:
```bash
DELTAFS_MetadataSrvAddrs=127.0.0.1:10101 ./build/src/server/deltafs-srvr -v=1 -logtostderr
```

Client side:
```bash
DELTAFS_MetadataSrvAddrs=127.0.0.1:10101 DELTAFS_NumOfMetadataSrvs=1 ./build/src/cmds/deltafs-shell -v=1 -logtostderr
```

To run on multiple machines instead of local, change all IPs above to real IPs.

TODO: hooking syscalls so mdtest will run.

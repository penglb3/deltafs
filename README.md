# DeltaFS (that actually runs)
[Original repo](https://github.com/pdlfs/deltafs)

[My issue (rant) on the original](https://github.com/pdlfs/deltafs/issues/8)

This is my debugged and syscall-hooked version. Unlike the original, IT FKING WORKS.

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
# Use my version, or how are you going to run mdtest?
git clone https://github.com/penglb3/deltafs.git
cd deltafs
mkdir build && cd build
ccmake -DBUILD_SHARED_LIBS=ON -DDELTAFS_COMMON_INTREE=ON -DDELTAFS_MPI=ON -DPDLFS_MERCURY_RPC=ON -DPDLFS_GFLAGS=ON -DPDLFS_GLOG=ON -DPDLFS_SNAPPY=ON ..
make -j
```

## Running
### Local Testing
Server side:
```bash
DELTAFS_MetadataSrvAddrs=127.0.0.1:10101 ./build/src/server/deltafs-srvr -v=1 -logtostderr
```

Client side:
```bash
DELTAFS_MetadataSrvAddrs=127.0.0.1:10101 DELTAFS_NumOfMetadataSrvs=1 ./build/src/cmds/deltafs-shell -v=1 -logtostderr
```

To run on multiple machines instead of local, change all IPs above to real IPs.

### Run mdtest
I have added a hook to syscalls (`hook.c`) that will direct certain requests into DeltaFS logic, link the mdtest with it and you can test DeltaFS with mdtest.

I will assume that you have already downloaded and compiled ior/mdtest and omit its installation steps here.
If you haven't, you can get it from [ior official github site](https://github.com/hpc/ior).

Server side is the same as above. On client side you run:
```bash
mpirun -n 2 env DELTAFS_MetadataSrvAddrs=127.0.0.1:10101 DELTAFS_NumOfMetadataSrvs=1 LD_PRELOAD=./build/src/libdeltafs/libdeltafs-hook.so mdtest -d /dfs/mdtest -n 10
```

IMPORTANT NOTES:
- If you don't see `libdeltafs-hook.so`, check if you have turned on **`BUILD_SHARED_LIBS`** for DeltaFS.
- **Make sure to include the "/dfs" prefix in path.** It is the signal that the hook should hand this request to DeltaFS instead of your local FS.

Again, if you want to run it across multiple machines, replace the metadata server address with real IPs. 

TODO: For now the `libdeltafs-hook.so` is linking to `libdeltafs.so` with an absolute path. I'll see if I can fix it. 
But anyway, it will run mdtest now!

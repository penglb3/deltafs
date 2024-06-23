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
sudo apt install cmake ninja-build checkinstall
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
cmake -B build -DNA_USE_OFI=ON -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build
cd build
sudo checkinstall -pkgname mercury
```
If you see rpc related testings fail, it might be that your machine has more CPUs than the default handle max in testing code. 
In that case simply change the `HG_TEST_HANDLE_MAX` variable in `Testing/unit/hg/mercury_unit.c` to a bigger value, then all tests should pass.

### Get DeltaFS source code and compile
```bash
# Use my version, or how are you going to run mdtest?
git clone https://github.com/penglb3/deltafs.git
cd deltafs
cmake -B build -GNinja -DBUILD_SHARED_LIBS=ON -DDELTAFS_COMMON_INTREE=ON -DDELTAFS_MPI=ON -DPDLFS_MERCURY_RPC=ON -DPDLFS_GFLAGS=ON -DPDLFS_GLOG=ON -DPDLFS_SNAPPY=ON
cmake --build build
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

**IMPORTANT NOTES**:
- **Make sure to include the "/dfs" prefix in path.** It is the signal that the hook should hand this request to DeltaFS instead of your local FS.
- **Restart the server after each run and DO NOT USE `-i` parameter**. DeltaFS does NOT SUPPORT `rmdir` and `rename` (and their `unlink` checks for file type, very droll indeed) so the directory created can never be deleted. The hook pretends that these operations work but in fact they don't and never will, I do this just to make running mdtest easier. This also means **mdtest results for Dir Rename and Dir Remove ARE UNRELIABLE**.
- 
- If you don't see `libdeltafs-hook.so`, check if `BUILD_SHARED_LIBS` is turned on for DeltaFS.

Again, if you want to run it across multiple machines, replace the metadata server address with real IPs. 

TODO: For now the `libdeltafs-hook.so` is linking to `libdeltafs.so` with an absolute path. I'll see if I can fix it. 
But anyway, it will run mdtest now!

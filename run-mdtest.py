import os
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--hosts', type=str, help='host list containing slots')
parser.add_argument('--clients_per_meta', type=int, default=1, help='Clients per metadata server')
parser.add_argument('--num_meta', type=int, default=1, help='Number of metas')
parser.add_argument('--ip', type=str, default='10.10.1.7', help='metadata server ip')
parser.add_argument('--base_port', type=int, default=10101, help='metadata base port')
parser.add_argument('--mdtest_args', type=str, default='-d /dfs/mdtest -n 10 -u', help='mdtest args')
parser.add_argument("--extra_mpi_args", type=str, default='', 
                    help="Extra arguments to pass to mpirun, e.g. '--mca btl_tcp_if_include enp65s0f0np0'")

args = parser.parse_args()

hosts = args.hosts.split(',')
slots = [i for i in map(lambda x: int(x[x.find(':') + 1:]), hosts)]
cmd_list = []
host_idx = 0
cpu_start = 0
cpm = args.clients_per_meta
for i in range(args.num_meta):
    cpu_set = ','.join(str(j) for j in range(cpu_start, cpu_start + cpm))
    cmd = f'mpirun -np {args.clients_per_meta} --host {hosts[host_idx]} --cpu-list {cpu_set} '
    cmd += args.extra_mpi_args
    cmd += 'env LD_PRELOAD=/usr/local/lib/libdeltafs-hook.so DELTAFS_NumOfMetadataSrvs=1 '
    cmd += "DELTAFS_NumOfThreads=1 "
    cmd += f'DELTAFS_MetadataSrvAddrs={args.ip}:{args.base_port + i} DELTAFS_InstanceId={i} '
    # cmd += f'numactl --all -C {slots[host_idx] - cpm}-{slots[host_idx] - 1} -- '
    cmd += f'~/mdtest {args.mdtest_args}'
    cmd_list.append(cmd)
    print(cmd)
    cpu_start += cpm
    if cpu_start >= slots[host_idx]:
        host_idx += 1
        cpu_start = 0
os.system(' & '.join(cmd_list))

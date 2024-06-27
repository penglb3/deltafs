import os
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('--hosts', type=str, help='host list containing slots')
parser.add_argument('--clients_per_meta', type=int, default=1, help='Clients per metadata server')
parser.add_argument('--num_meta', type=int, default=1, help='Number of metas')
parser.add_argument('--ip', type=str, default='10.10.1.7', help='metadata server ip')
parser.add_argument('--base_port', type=int, default=10101, help='metadata base port')
parser.add_argument('--mdtest_args', type=str, default='-d /dfs/mdtest -n 10 -u', help='mdtest args')

args = parser.parse_args()

hosts = args.hosts.split(',')
slots = [i for i in map(lambda x: int(x[x.find(':'):]), hosts)]
cmd_list = []
host_idx = 0
for i in range(args.num_meta):
    cmd = f'mpirun -np {args.clients_per_meta} --host {hosts[host_idx]}'
    cmd += 'env LD_PRELOAD=/usr/local/lib/libdeltafs-hook.so DELTAFS_NumOfMetadataSrvs=1'
    cmd += f'DELTAFS_MetadataSrvAddrs={args.ip}:{args.base_port + i}'
    cmd += f'~/mdtest {args.mdtest_args}'
    cmd_list.append(cmd)
    slots[host_idx] -= args.clients_per_meta
    if slots[host_idx] <= 0:
        host_idx += 1
os.system(' & '.join(cmd_list))

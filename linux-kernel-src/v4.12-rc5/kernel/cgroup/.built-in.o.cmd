cmd_kernel/cgroup/built-in.o :=  ld -m elf_x86_64   -r -o kernel/cgroup/built-in.o kernel/cgroup/cgroup.o kernel/cgroup/namespace.o kernel/cgroup/cgroup-v1.o kernel/cgroup/freezer.o kernel/cgroup/cpuset.o 

cmd_arch/x86/events/built-in.o :=  ld -m elf_x86_64   -r -o arch/x86/events/built-in.o arch/x86/events/core.o arch/x86/events/amd/built-in.o arch/x86/events/msr.o arch/x86/events/intel/built-in.o 

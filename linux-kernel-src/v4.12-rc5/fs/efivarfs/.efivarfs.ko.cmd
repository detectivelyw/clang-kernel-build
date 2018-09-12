cmd_fs/efivarfs/efivarfs.ko := ld -r -m elf_x86_64 -T ./scripts/module-common.lds --build-id  -o fs/efivarfs/efivarfs.ko fs/efivarfs/efivarfs.o fs/efivarfs/efivarfs.mod.o ;  true

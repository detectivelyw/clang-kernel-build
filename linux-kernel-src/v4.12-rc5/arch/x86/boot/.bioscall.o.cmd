cmd_arch/x86/boot/bioscall.o := /home/detectivelyw/Documents/projects/clang-kernel-build/clang-kernel-build/third_party/llvm-build/Release+Asserts/bin//clang -Wp,-MD,arch/x86/boot/.bioscall.o.d  -nostdinc -isystem /home/detectivelyw/Documents/projects/clang-kernel-build/clang-kernel-build/third_party/llvm-build/Release+Asserts/lib/clang/7.0.0/include -I./arch/x86/include -I./arch/x86/include/generated/uapi -I./arch/x86/include/generated  -I./include -I./arch/x86/include/uapi -I./include/uapi -I./include/generated/uapi -include ./include/linux/kconfig.h -D__KERNEL__ -Qunused-arguments -m16 -g -Os -D__KERNEL__ -DDISABLE_BRANCH_PROFILING -Wall -Wstrict-prototypes -march=i386 -mregparm=3 -fno-strict-aliasing -fomit-frame-pointer -fno-pic -mno-mmx -mno-sse -ffreestanding -fno-stack-protector -D_SETUP -D__ASSEMBLY__ -DSVGA_MODE=NORMAL_VGA   -c -o arch/x86/boot/bioscall.o arch/x86/boot/bioscall.S

source_arch/x86/boot/bioscall.o := arch/x86/boot/bioscall.S

deps_arch/x86/boot/bioscall.o := \

arch/x86/boot/bioscall.o: $(deps_arch/x86/boot/bioscall.o)

$(deps_arch/x86/boot/bioscall.o):

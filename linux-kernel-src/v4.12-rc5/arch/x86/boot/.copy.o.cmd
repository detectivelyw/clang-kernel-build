cmd_arch/x86/boot/copy.o := /home/detectivelyw/Documents/projects/clang-kernel-build/clang-kernel-build/third_party/llvm-build/Release+Asserts/bin//clang -Wp,-MD,arch/x86/boot/.copy.o.d  -nostdinc -isystem /home/detectivelyw/Documents/projects/clang-kernel-build/clang-kernel-build/third_party/llvm-build/Release+Asserts/lib/clang/7.0.0/include -I./arch/x86/include -I./arch/x86/include/generated/uapi -I./arch/x86/include/generated  -I./include -I./arch/x86/include/uapi -I./include/uapi -I./include/generated/uapi -include ./include/linux/kconfig.h -D__KERNEL__ -Qunused-arguments -m16 -g -Os -D__KERNEL__ -DDISABLE_BRANCH_PROFILING -Wall -Wstrict-prototypes -march=i386 -mregparm=3 -fno-strict-aliasing -fomit-frame-pointer -fno-pic -mno-mmx -mno-sse -ffreestanding -fno-stack-protector -D_SETUP -D__ASSEMBLY__ -DSVGA_MODE=NORMAL_VGA   -c -o arch/x86/boot/copy.o arch/x86/boot/copy.S

source_arch/x86/boot/copy.o := arch/x86/boot/copy.S

deps_arch/x86/boot/copy.o := \
  include/linux/linkage.h \
  include/linux/compiler.h \
    $(wildcard include/config/sparse/rcu/pointer.h) \
    $(wildcard include/config/trace/branch/profiling.h) \
    $(wildcard include/config/profile/all/branches.h) \
    $(wildcard include/config/kasan.h) \
    $(wildcard include/config/enable/must/check.h) \
    $(wildcard include/config/enable/warn/deprecated.h) \
  include/linux/stringify.h \
  include/linux/export.h \
    $(wildcard include/config/have/underscore/symbol/prefix.h) \
    $(wildcard include/config/modules.h) \
    $(wildcard include/config/modversions.h) \
    $(wildcard include/config/module/rel/crcs.h) \
    $(wildcard include/config/trim/unused/ksyms.h) \
    $(wildcard include/config/unused/symbols.h) \
  arch/x86/include/asm/linkage.h \
    $(wildcard include/config/x86/32.h) \
    $(wildcard include/config/x86/64.h) \
    $(wildcard include/config/x86/alignment/16.h) \

arch/x86/boot/copy.o: $(deps_arch/x86/boot/copy.o)

$(deps_arch/x86/boot/copy.o):

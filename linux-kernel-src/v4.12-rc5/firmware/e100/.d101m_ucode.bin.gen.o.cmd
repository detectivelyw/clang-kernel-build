cmd_firmware/e100/d101m_ucode.bin.gen.o := /home/detectivelyw/Documents/projects/clang-kernel-build/clang-kernel-build/third_party/llvm-build/Release+Asserts/bin//clang -Wp,-MD,firmware/e100/.d101m_ucode.bin.gen.o.d  -nostdinc -isystem /home/detectivelyw/Documents/projects/clang-kernel-build/clang-kernel-build/third_party/llvm-build/Release+Asserts/lib/clang/7.0.0/include -I./arch/x86/include -I./arch/x86/include/generated/uapi -I./arch/x86/include/generated  -I./include -I./arch/x86/include/uapi -I./include/uapi -I./include/generated/uapi -include ./include/linux/kconfig.h -D__KERNEL__ -Qunused-arguments -D__ASSEMBLY__ -fno-PIE -m64 -DCONFIG_AS_CFI=1 -DCONFIG_AS_CFI_SIGNAL_FRAME=1 -DCONFIG_AS_CFI_SECTIONS=1 -DCONFIG_AS_FXSAVEQ=1 -DCONFIG_AS_SSSE3=1 -DCONFIG_AS_CRC32=1 -DCONFIG_AS_AVX=1 -DCONFIG_AS_AVX2=1 -DCONFIG_AS_AVX512=1 -DCONFIG_AS_SHA1_NI=1 -DCONFIG_AS_SHA256_NI=1 -no-integrated-as   -c -o firmware/e100/d101m_ucode.bin.gen.o firmware/e100/d101m_ucode.bin.gen.S

source_firmware/e100/d101m_ucode.bin.gen.o := firmware/e100/d101m_ucode.bin.gen.S

deps_firmware/e100/d101m_ucode.bin.gen.o := \

firmware/e100/d101m_ucode.bin.gen.o: $(deps_firmware/e100/d101m_ucode.bin.gen.o)

$(deps_firmware/e100/d101m_ucode.bin.gen.o):

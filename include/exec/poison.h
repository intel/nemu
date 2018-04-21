/* Poison identifiers that should not be used when building
   target independent device code.  */

#ifndef HW_POISON_H
#define HW_POISON_H
#ifdef __GNUC__

#pragma GCC poison TARGET_I386
#pragma GCC poison TARGET_X86_64
#pragma GCC poison TARGET_AARCH64
#pragma GCC poison TARGET_ARM
#pragma GCC poison TARGET_ABI32

#pragma GCC poison TARGET_HAS_BFLT
#pragma GCC poison TARGET_NAME
#pragma GCC poison TARGET_SUPPORTS_MTTCG
#pragma GCC poison TARGET_WORDS_BIGENDIAN
#pragma GCC poison BSWAP_NEEDED

#pragma GCC poison TARGET_LONG_BITS
#pragma GCC poison TARGET_FMT_lx
#pragma GCC poison TARGET_FMT_ld

#pragma GCC poison TARGET_PAGE_SIZE
#pragma GCC poison TARGET_PAGE_MASK
#pragma GCC poison TARGET_PAGE_BITS
#pragma GCC poison TARGET_PAGE_ALIGN

#pragma GCC poison CPUArchState

#pragma GCC poison CPU_INTERRUPT_HARD
#pragma GCC poison CPU_INTERRUPT_EXITTB
#pragma GCC poison CPU_INTERRUPT_HALT
#pragma GCC poison CPU_INTERRUPT_DEBUG
#pragma GCC poison CPU_INTERRUPT_TGT_EXT_0
#pragma GCC poison CPU_INTERRUPT_TGT_EXT_1
#pragma GCC poison CPU_INTERRUPT_TGT_EXT_2
#pragma GCC poison CPU_INTERRUPT_TGT_EXT_3
#pragma GCC poison CPU_INTERRUPT_TGT_EXT_4
#pragma GCC poison CPU_INTERRUPT_TGT_INT_0
#pragma GCC poison CPU_INTERRUPT_TGT_INT_1
#pragma GCC poison CPU_INTERRUPT_TGT_INT_2

#pragma GCC poison CONFIG_ARM_A64_DIS
#pragma GCC poison CONFIG_ARM_DIS
#pragma GCC poison CONFIG_I386_DIS

#pragma GCC poison CONFIG_LINUX_USER
#pragma GCC poison CONFIG_VHOST_NET
#pragma GCC poison CONFIG_KVM
#pragma GCC poison CONFIG_SOFTMMU

#endif
#endif

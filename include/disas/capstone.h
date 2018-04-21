#ifndef QEMU_CAPSTONE_H
#define QEMU_CAPSTONE_H 1


/* Just enough to allow backends to init without ifdefs.  */

#define CS_ARCH_ARM     -1
#define CS_ARCH_ARM64   -1
#define CS_ARCH_X86     -1
#define CS_ARCH_SYSZ    -1

#define CS_MODE_LITTLE_ENDIAN    0
#define CS_MODE_BIG_ENDIAN       0
#define CS_MODE_ARM              0
#define CS_MODE_16               0
#define CS_MODE_32               0
#define CS_MODE_64               0
#define CS_MODE_THUMB            0
#define CS_MODE_MCLASS           0
#define CS_MODE_V8               0
#define CS_MODE_MICRO            0
#define CS_MODE_V9               0

#endif /* QEMU_CAPSTONE_H */

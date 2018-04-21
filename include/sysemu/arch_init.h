#ifndef QEMU_ARCH_INIT_H
#define QEMU_ARCH_INIT_H

#include "qapi/qapi-types-misc.h"

enum {
    QEMU_ARCH_ALL = -1,
    QEMU_ARCH_ARM = (1 << 1),
    QEMU_ARCH_I386 = (1 << 3),
};

extern const uint32_t arch_type;

int kvm_available(void);

CpuDefinitionInfoList *arch_query_cpu_definitions(Error **errp);
CpuModelExpansionInfo *arch_query_cpu_model_expansion(CpuModelExpansionType type,
                                                      CpuModelInfo *mode,
                                                      Error **errp);
CpuModelCompareInfo *arch_query_cpu_model_comparison(CpuModelInfo *modela,
                                                     CpuModelInfo *modelb,
                                                     Error **errp);
CpuModelBaselineInfo *arch_query_cpu_model_baseline(CpuModelInfo *modela,
                                                    CpuModelInfo *modelb,
                                                    Error **errp);

#endif

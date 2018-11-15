/*
 * QEMU Light weight PCI Host Bridge Emulation
 *
 * Copyright (C) 2016 Intel Corporation.
 *
 * Author:
 *  Chao Peng <chao.p.peng@linux.intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_PCI_LITE_H
#define HW_PCI_LITE_H

#include "hw/hw.h"
#include "qemu/range.h"
#include "hw/pci/pcie_host.h"

#define TYPE_PCI_LITE_HOST      "pci-lite-host"
#define TYPE_PCI_LITE_DEVICE    "pci-lite-device"

#define PCI_LITE_HOST(obj) \
    OBJECT_CHECK(PCILiteHost, (obj), TYPE_PCI_LITE_HOST)

#define PCI_LITE_NUM_IRQS       4

typedef struct PCILiteHost {
    /*< private >*/
    PCIExpressHost parent_obj;
    /*< public >*/

    Range pci_hole;
    Range pci_hole64;
    qemu_irq irq[PCI_LITE_NUM_IRQS];
    uint64_t pci_hole64_size;
} PCILiteHost;


PCIHostState *pci_lite_init(MemoryRegion *address_space_mem,
                            MemoryRegion *address_space_io,
                            MemoryRegion *pci_address_space);

void pci_fw_cfg_add(FWCfgState *fw_cfg, PCIHostState *pci_host);

#endif /* HW_PCI_LITE_H */

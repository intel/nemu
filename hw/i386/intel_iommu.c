/*
 * QEMU emulation of an Intel IOMMU (VT-d)
 *   (DMA Remapping device)
 *
 * Copyright (C) 2013 Knut Omang, Oracle <knut.omang@oracle.com>
 * Copyright (C) 2014 Le Tan, <tamlokveer@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "exec/address-spaces.h"
#include "intel_iommu_internal.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/i386/pc.h"
#include "hw/i386/apic-msidef.h"
#include "hw/boards.h"
#include "hw/i386/x86-iommu.h"
#include "hw/pci-host/q35.h"
#include "sysemu/kvm.h"
#include "hw/i386/apic_internal.h"
#include "kvm_i386.h"
#include "trace.h"

static void vtd_define_quad(IntelIOMMUState *s, hwaddr addr, uint64_t val,
                            uint64_t wmask, uint64_t w1cmask)
{
    stq_le_p(&s->csr[addr], val);
    stq_le_p(&s->wmask[addr], wmask);
    stq_le_p(&s->w1cmask[addr], w1cmask);
}

static void vtd_define_quad_wo(IntelIOMMUState *s, hwaddr addr, uint64_t mask)
{
    stq_le_p(&s->womask[addr], mask);
}

static void vtd_define_long(IntelIOMMUState *s, hwaddr addr, uint32_t val,
                            uint32_t wmask, uint32_t w1cmask)
{
    stl_le_p(&s->csr[addr], val);
    stl_le_p(&s->wmask[addr], wmask);
    stl_le_p(&s->w1cmask[addr], w1cmask);
}

static void vtd_define_long_wo(IntelIOMMUState *s, hwaddr addr, uint32_t mask)
{
    stl_le_p(&s->womask[addr], mask);
}

/* "External" get/set operations */
static void vtd_set_quad(IntelIOMMUState *s, hwaddr addr, uint64_t val)
{
    uint64_t oldval = ldq_le_p(&s->csr[addr]);
    uint64_t wmask = ldq_le_p(&s->wmask[addr]);
    uint64_t w1cmask = ldq_le_p(&s->w1cmask[addr]);
    stq_le_p(&s->csr[addr],
             ((oldval & ~wmask) | (val & wmask)) & ~(w1cmask & val));
}

static void vtd_set_long(IntelIOMMUState *s, hwaddr addr, uint32_t val)
{
    uint32_t oldval = ldl_le_p(&s->csr[addr]);
    uint32_t wmask = ldl_le_p(&s->wmask[addr]);
    uint32_t w1cmask = ldl_le_p(&s->w1cmask[addr]);
    stl_le_p(&s->csr[addr],
             ((oldval & ~wmask) | (val & wmask)) & ~(w1cmask & val));
}

static uint64_t vtd_get_quad(IntelIOMMUState *s, hwaddr addr)
{
    uint64_t val = ldq_le_p(&s->csr[addr]);
    uint64_t womask = ldq_le_p(&s->womask[addr]);
    return val & ~womask;
}

static uint32_t vtd_get_long(IntelIOMMUState *s, hwaddr addr)
{
    uint32_t val = ldl_le_p(&s->csr[addr]);
    uint32_t womask = ldl_le_p(&s->womask[addr]);
    return val & ~womask;
}

/* "Internal" get/set operations */
static uint64_t vtd_get_quad_raw(IntelIOMMUState *s, hwaddr addr)
{
    return ldq_le_p(&s->csr[addr]);
}

static uint32_t vtd_get_long_raw(IntelIOMMUState *s, hwaddr addr)
{
    return ldl_le_p(&s->csr[addr]);
}

static void vtd_set_quad_raw(IntelIOMMUState *s, hwaddr addr, uint64_t val)
{
    stq_le_p(&s->csr[addr], val);
}

static uint32_t vtd_set_clear_mask_long(IntelIOMMUState *s, hwaddr addr,
                                        uint32_t clear, uint32_t mask)
{
    uint32_t new_val = (ldl_le_p(&s->csr[addr]) & ~clear) | mask;
    stl_le_p(&s->csr[addr], new_val);
    return new_val;
}

static uint64_t vtd_set_clear_mask_quad(IntelIOMMUState *s, hwaddr addr,
                                        uint64_t clear, uint64_t mask)
{
    uint64_t new_val = (ldq_le_p(&s->csr[addr]) & ~clear) | mask;
    stq_le_p(&s->csr[addr], new_val);
    return new_val;
}

/* GHashTable functions */
static gboolean vtd_uint64_equal(gconstpointer v1, gconstpointer v2)
{
    return *((const uint64_t *)v1) == *((const uint64_t *)v2);
}

static guint vtd_uint64_hash(gconstpointer v)
{
    return (guint)*(const uint64_t *)v;
}

static gboolean vtd_hash_remove_by_domain(gpointer key, gpointer value,
                                          gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    uint16_t domain_id = *(uint16_t *)user_data;
    return entry->domain_id == domain_id;
}

/* The shift of an addr for a certain level of paging structure */
static inline uint32_t vtd_slpt_level_shift(uint32_t level)
{
    assert(level != 0);
    return VTD_PAGE_SHIFT_4K + (level - 1) * VTD_SL_LEVEL_BITS;
}

static inline uint64_t vtd_slpt_level_page_mask(uint32_t level)
{
    return ~((1ULL << vtd_slpt_level_shift(level)) - 1);
}

static gboolean vtd_hash_remove_by_page(gpointer key, gpointer value,
                                        gpointer user_data)
{
    VTDIOTLBEntry *entry = (VTDIOTLBEntry *)value;
    VTDIOTLBPageInvInfo *info = (VTDIOTLBPageInvInfo *)user_data;
    uint64_t gfn = (info->addr >> VTD_PAGE_SHIFT_4K) & info->mask;
    uint64_t gfn_tlb = (info->addr & entry->mask) >> VTD_PAGE_SHIFT_4K;
    return (entry->domain_id == info->domain_id) &&
            (((entry->gfn & info->mask) == gfn) ||
             (entry->gfn == gfn_tlb));
}

/* Reset all the gen of VTDAddressSpace to zero and set the gen of
 * IntelIOMMUState to 1.
 */
static void vtd_reset_context_cache(IntelIOMMUState *s)
{
    VTDAddressSpace *vtd_as;
    VTDBus *vtd_bus;
    GHashTableIter bus_it;
    uint32_t devfn_it;

    trace_vtd_context_cache_reset();

    g_hash_table_iter_init(&bus_it, s->vtd_as_by_busptr);

    while (g_hash_table_iter_next (&bus_it, NULL, (void**)&vtd_bus)) {
        for (devfn_it = 0; devfn_it < PCI_DEVFN_MAX; ++devfn_it) {
            vtd_as = vtd_bus->dev_as[devfn_it];
            if (!vtd_as) {
                continue;
            }
            vtd_as->context_cache_entry.context_cache_gen = 0;
        }
    }
    s->context_cache_gen = 1;
}

static void vtd_reset_iotlb(IntelIOMMUState *s)
{
    assert(s->iotlb);
    g_hash_table_remove_all(s->iotlb);
}

static uint64_t vtd_get_iotlb_key(uint64_t gfn, uint16_t source_id,
                                  uint32_t level)
{
    return gfn | ((uint64_t)(source_id) << VTD_IOTLB_SID_SHIFT) |
           ((uint64_t)(level) << VTD_IOTLB_LVL_SHIFT);
}

static uint64_t vtd_get_iotlb_gfn(hwaddr addr, uint32_t level)
{
    return (addr & vtd_slpt_level_page_mask(level)) >> VTD_PAGE_SHIFT_4K;
}

static VTDIOTLBEntry *vtd_lookup_iotlb(IntelIOMMUState *s, uint16_t source_id,
                                       hwaddr addr)
{
    VTDIOTLBEntry *entry;
    uint64_t key;
    int level;

    for (level = VTD_SL_PT_LEVEL; level < VTD_SL_PML4_LEVEL; level++) {
        key = vtd_get_iotlb_key(vtd_get_iotlb_gfn(addr, level),
                                source_id, level);
        entry = g_hash_table_lookup(s->iotlb, &key);
        if (entry) {
            goto out;
        }
    }

out:
    return entry;
}

static void vtd_update_iotlb(IntelIOMMUState *s, uint16_t source_id,
                             uint16_t domain_id, hwaddr addr, uint64_t slpte,
                             uint8_t access_flags, uint32_t level)
{
    VTDIOTLBEntry *entry = g_malloc(sizeof(*entry));
    uint64_t *key = g_malloc(sizeof(*key));
    uint64_t gfn = vtd_get_iotlb_gfn(addr, level);

    trace_vtd_iotlb_page_update(source_id, addr, slpte, domain_id);
    if (g_hash_table_size(s->iotlb) >= VTD_IOTLB_MAX_SIZE) {
        trace_vtd_iotlb_reset("iotlb exceeds size limit");
        vtd_reset_iotlb(s);
    }

    entry->gfn = gfn;
    entry->domain_id = domain_id;
    entry->slpte = slpte;
    entry->access_flags = access_flags;
    entry->mask = vtd_slpt_level_page_mask(level);
    *key = vtd_get_iotlb_key(gfn, source_id, level);
    g_hash_table_replace(s->iotlb, key, entry);
}

/* Given the reg addr of both the message data and address, generate an
 * interrupt via MSI.
 */
static void vtd_generate_interrupt(IntelIOMMUState *s, hwaddr mesg_addr_reg,
                                   hwaddr mesg_data_reg)
{
    MSIMessage msi;

    assert(mesg_data_reg < DMAR_REG_SIZE);
    assert(mesg_addr_reg < DMAR_REG_SIZE);

    msi.address = vtd_get_long_raw(s, mesg_addr_reg);
    msi.data = vtd_get_long_raw(s, mesg_data_reg);

    trace_vtd_irq_generate(msi.address, msi.data);

    apic_get_class()->send_msi(&msi);
}

/* Generate a fault event to software via MSI if conditions are met.
 * Notice that the value of FSTS_REG being passed to it should be the one
 * before any update.
 */
static void vtd_generate_fault_event(IntelIOMMUState *s, uint32_t pre_fsts)
{
    if (pre_fsts & VTD_FSTS_PPF || pre_fsts & VTD_FSTS_PFO ||
        pre_fsts & VTD_FSTS_IQE) {
        trace_vtd_err("There are previous interrupt conditions "
                      "to be serviced by software, fault event "
                      "is not generated.");
        return;
    }
    vtd_set_clear_mask_long(s, DMAR_FECTL_REG, 0, VTD_FECTL_IP);
    if (vtd_get_long_raw(s, DMAR_FECTL_REG) & VTD_FECTL_IM) {
        trace_vtd_err("Interrupt Mask set, irq is not generated.");
    } else {
        vtd_generate_interrupt(s, DMAR_FEADDR_REG, DMAR_FEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
    }
}

/* Check if the Fault (F) field of the Fault Recording Register referenced by
 * @index is Set.
 */
static bool vtd_is_frcd_set(IntelIOMMUState *s, uint16_t index)
{
    /* Each reg is 128-bit */
    hwaddr addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);
    addr += 8; /* Access the high 64-bit half */

    assert(index < DMAR_FRCD_REG_NR);

    return vtd_get_quad_raw(s, addr) & VTD_FRCD_F;
}

/* Update the PPF field of Fault Status Register.
 * Should be called whenever change the F field of any fault recording
 * registers.
 */
static void vtd_update_fsts_ppf(IntelIOMMUState *s)
{
    uint32_t i;
    uint32_t ppf_mask = 0;

    for (i = 0; i < DMAR_FRCD_REG_NR; i++) {
        if (vtd_is_frcd_set(s, i)) {
            ppf_mask = VTD_FSTS_PPF;
            break;
        }
    }
    vtd_set_clear_mask_long(s, DMAR_FSTS_REG, VTD_FSTS_PPF, ppf_mask);
    trace_vtd_fsts_ppf(!!ppf_mask);
}

static void vtd_set_frcd_and_update_ppf(IntelIOMMUState *s, uint16_t index)
{
    /* Each reg is 128-bit */
    hwaddr addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);
    addr += 8; /* Access the high 64-bit half */

    assert(index < DMAR_FRCD_REG_NR);

    vtd_set_clear_mask_quad(s, addr, 0, VTD_FRCD_F);
    vtd_update_fsts_ppf(s);
}

/* Must not update F field now, should be done later */
static void vtd_record_frcd(IntelIOMMUState *s, uint16_t index,
                            uint16_t source_id, hwaddr addr,
                            VTDFaultReason fault, bool is_write)
{
    uint64_t hi = 0, lo;
    hwaddr frcd_reg_addr = DMAR_FRCD_REG_OFFSET + (((uint64_t)index) << 4);

    assert(index < DMAR_FRCD_REG_NR);

    lo = VTD_FRCD_FI(addr);
    hi = VTD_FRCD_SID(source_id) | VTD_FRCD_FR(fault);
    if (!is_write) {
        hi |= VTD_FRCD_T;
    }
    vtd_set_quad_raw(s, frcd_reg_addr, lo);
    vtd_set_quad_raw(s, frcd_reg_addr + 8, hi);

    trace_vtd_frr_new(index, hi, lo);
}

/* Try to collapse multiple pending faults from the same requester */
static bool vtd_try_collapse_fault(IntelIOMMUState *s, uint16_t source_id)
{
    uint32_t i;
    uint64_t frcd_reg;
    hwaddr addr = DMAR_FRCD_REG_OFFSET + 8; /* The high 64-bit half */

    for (i = 0; i < DMAR_FRCD_REG_NR; i++) {
        frcd_reg = vtd_get_quad_raw(s, addr);
        if ((frcd_reg & VTD_FRCD_F) &&
            ((frcd_reg & VTD_FRCD_SID_MASK) == source_id)) {
            return true;
        }
        addr += 16; /* 128-bit for each */
    }
    return false;
}

/* Log and report an DMAR (address translation) fault to software */
static void vtd_report_dmar_fault(IntelIOMMUState *s, uint16_t source_id,
                                  hwaddr addr, VTDFaultReason fault,
                                  bool is_write)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);

    assert(fault < VTD_FR_MAX);

    if (fault == VTD_FR_RESERVED_ERR) {
        /* This is not a normal fault reason case. Drop it. */
        return;
    }

    trace_vtd_dmar_fault(source_id, fault, addr, is_write);

    if (fsts_reg & VTD_FSTS_PFO) {
        trace_vtd_err("New fault is not recorded due to "
                      "Primary Fault Overflow.");
        return;
    }

    if (vtd_try_collapse_fault(s, source_id)) {
        trace_vtd_err("New fault is not recorded due to "
                      "compression of faults.");
        return;
    }

    if (vtd_is_frcd_set(s, s->next_frcd_reg)) {
        trace_vtd_err("Next Fault Recording Reg is used, "
                      "new fault is not recorded, set PFO field.");
        vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_PFO);
        return;
    }

    vtd_record_frcd(s, s->next_frcd_reg, source_id, addr, fault, is_write);

    if (fsts_reg & VTD_FSTS_PPF) {
        trace_vtd_err("There are pending faults already, "
                      "fault event is not generated.");
        vtd_set_frcd_and_update_ppf(s, s->next_frcd_reg);
        s->next_frcd_reg++;
        if (s->next_frcd_reg == DMAR_FRCD_REG_NR) {
            s->next_frcd_reg = 0;
        }
    } else {
        vtd_set_clear_mask_long(s, DMAR_FSTS_REG, VTD_FSTS_FRI_MASK,
                                VTD_FSTS_FRI(s->next_frcd_reg));
        vtd_set_frcd_and_update_ppf(s, s->next_frcd_reg); /* Will set PPF */
        s->next_frcd_reg++;
        if (s->next_frcd_reg == DMAR_FRCD_REG_NR) {
            s->next_frcd_reg = 0;
        }
        /* This case actually cause the PPF to be Set.
         * So generate fault event (interrupt).
         */
         vtd_generate_fault_event(s, fsts_reg);
    }
}

/* Handle Invalidation Queue Errors of queued invalidation interface error
 * conditions.
 */
static void vtd_handle_inv_queue_error(IntelIOMMUState *s)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);

    vtd_set_clear_mask_long(s, DMAR_FSTS_REG, 0, VTD_FSTS_IQE);
    vtd_generate_fault_event(s, fsts_reg);
}

/* Set the IWC field and try to generate an invalidation completion interrupt */
static void vtd_generate_completion_event(IntelIOMMUState *s)
{
    if (vtd_get_long_raw(s, DMAR_ICS_REG) & VTD_ICS_IWC) {
        trace_vtd_inv_desc_wait_irq("One pending, skip current");
        return;
    }
    vtd_set_clear_mask_long(s, DMAR_ICS_REG, 0, VTD_ICS_IWC);
    vtd_set_clear_mask_long(s, DMAR_IECTL_REG, 0, VTD_IECTL_IP);
    if (vtd_get_long_raw(s, DMAR_IECTL_REG) & VTD_IECTL_IM) {
        trace_vtd_inv_desc_wait_irq("IM in IECTL_REG is set, "
                                    "new event not generated");
        return;
    } else {
        /* Generate the interrupt event */
        trace_vtd_inv_desc_wait_irq("Generating complete event");
        vtd_generate_interrupt(s, DMAR_IEADDR_REG, DMAR_IEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static inline bool vtd_root_entry_present(VTDRootEntry *root)
{
    return root->val & VTD_ROOT_ENTRY_P;
}

static int vtd_get_root_entry(IntelIOMMUState *s, uint8_t index,
                              VTDRootEntry *re)
{
    dma_addr_t addr;

    addr = s->root + index * sizeof(*re);
    if (dma_memory_read(&address_space_memory, addr, re, sizeof(*re))) {
        trace_vtd_re_invalid(re->rsvd, re->val);
        re->val = 0;
        return -VTD_FR_ROOT_TABLE_INV;
    }
    re->val = le64_to_cpu(re->val);
    return 0;
}

static inline bool vtd_ce_present(VTDContextEntry *context)
{
    return context->lo & VTD_CONTEXT_ENTRY_P;
}

static int vtd_get_context_entry_from_root(VTDRootEntry *root, uint8_t index,
                                           VTDContextEntry *ce)
{
    dma_addr_t addr;

    /* we have checked that root entry is present */
    addr = (root->val & VTD_ROOT_ENTRY_CTP) + index * sizeof(*ce);
    if (dma_memory_read(&address_space_memory, addr, ce, sizeof(*ce))) {
        trace_vtd_re_invalid(root->rsvd, root->val);
        return -VTD_FR_CONTEXT_TABLE_INV;
    }
    ce->lo = le64_to_cpu(ce->lo);
    ce->hi = le64_to_cpu(ce->hi);
    return 0;
}

static inline dma_addr_t vtd_ce_get_slpt_base(VTDContextEntry *ce)
{
    return ce->lo & VTD_CONTEXT_ENTRY_SLPTPTR;
}

static inline uint64_t vtd_get_slpte_addr(uint64_t slpte, uint8_t aw)
{
    return slpte & VTD_SL_PT_BASE_ADDR_MASK(aw);
}

/* Whether the pte indicates the address of the page frame */
static inline bool vtd_is_last_slpte(uint64_t slpte, uint32_t level)
{
    return level == VTD_SL_PT_LEVEL || (slpte & VTD_SL_PT_PAGE_SIZE_MASK);
}

/* Get the content of a spte located in @base_addr[@index] */
static uint64_t vtd_get_slpte(dma_addr_t base_addr, uint32_t index)
{
    uint64_t slpte;

    assert(index < VTD_SL_PT_ENTRY_NR);

    if (dma_memory_read(&address_space_memory,
                        base_addr + index * sizeof(slpte), &slpte,
                        sizeof(slpte))) {
        slpte = (uint64_t)-1;
        return slpte;
    }
    slpte = le64_to_cpu(slpte);
    return slpte;
}

/* Given an iova and the level of paging structure, return the offset
 * of current level.
 */
static inline uint32_t vtd_iova_level_offset(uint64_t iova, uint32_t level)
{
    return (iova >> vtd_slpt_level_shift(level)) &
            ((1ULL << VTD_SL_LEVEL_BITS) - 1);
}

/* Check Capability Register to see if the @level of page-table is supported */
static inline bool vtd_is_level_supported(IntelIOMMUState *s, uint32_t level)
{
    return VTD_CAP_SAGAW_MASK & s->cap &
           (1ULL << (level - 2 + VTD_CAP_SAGAW_SHIFT));
}

/* Get the page-table level that hardware should use for the second-level
 * page-table walk from the Address Width field of context-entry.
 */
static inline uint32_t vtd_ce_get_level(VTDContextEntry *ce)
{
    return 2 + (ce->hi & VTD_CONTEXT_ENTRY_AW);
}

static inline uint32_t vtd_ce_get_agaw(VTDContextEntry *ce)
{
    return 30 + (ce->hi & VTD_CONTEXT_ENTRY_AW) * 9;
}

static inline uint32_t vtd_ce_get_type(VTDContextEntry *ce)
{
    return ce->lo & VTD_CONTEXT_ENTRY_TT;
}

/* Return true if check passed, otherwise false */
static inline bool vtd_ce_type_check(X86IOMMUState *x86_iommu,
                                     VTDContextEntry *ce)
{
    switch (vtd_ce_get_type(ce)) {
    case VTD_CONTEXT_TT_MULTI_LEVEL:
        /* Always supported */
        break;
    case VTD_CONTEXT_TT_DEV_IOTLB:
        if (!x86_iommu->dt_supported) {
            return false;
        }
        break;
    case VTD_CONTEXT_TT_PASS_THROUGH:
        if (!x86_iommu->pt_supported) {
            return false;
        }
        break;
    default:
        /* Unknwon type */
        return false;
    }
    return true;
}

static inline uint64_t vtd_iova_limit(VTDContextEntry *ce, uint8_t aw)
{
    uint32_t ce_agaw = vtd_ce_get_agaw(ce);
    return 1ULL << MIN(ce_agaw, aw);
}

/* Return true if IOVA passes range check, otherwise false. */
static inline bool vtd_iova_range_check(uint64_t iova, VTDContextEntry *ce,
                                        uint8_t aw)
{
    /*
     * Check if @iova is above 2^X-1, where X is the minimum of MGAW
     * in CAP_REG and AW in context-entry.
     */
    return !(iova & ~(vtd_iova_limit(ce, aw) - 1));
}

/*
 * Rsvd field masks for spte:
 *     Index [1] to [4] 4k pages
 *     Index [5] to [8] large pages
 */
static uint64_t vtd_paging_entry_rsvd_field[9];

static bool vtd_slpte_nonzero_rsvd(uint64_t slpte, uint32_t level)
{
    if (slpte & VTD_SL_PT_PAGE_SIZE_MASK) {
        /* Maybe large page */
        return slpte & vtd_paging_entry_rsvd_field[level + 4];
    } else {
        return slpte & vtd_paging_entry_rsvd_field[level];
    }
}

/* Find the VTD address space associated with a given bus number */
static VTDBus *vtd_find_as_from_bus_num(IntelIOMMUState *s, uint8_t bus_num)
{
    VTDBus *vtd_bus = s->vtd_as_by_bus_num[bus_num];
    if (!vtd_bus) {
        /*
         * Iterate over the registered buses to find the one which
         * currently hold this bus number, and update the bus_num
         * lookup table:
         */
        GHashTableIter iter;

        g_hash_table_iter_init(&iter, s->vtd_as_by_busptr);
        while (g_hash_table_iter_next(&iter, NULL, (void **)&vtd_bus)) {
            if (pci_bus_num(vtd_bus->bus) == bus_num) {
                s->vtd_as_by_bus_num[bus_num] = vtd_bus;
                return vtd_bus;
            }
        }
    }
    return vtd_bus;
}

/* Given the @iova, get relevant @slptep. @slpte_level will be the last level
 * of the translation, can be used for deciding the size of large page.
 */
static int vtd_iova_to_slpte(VTDContextEntry *ce, uint64_t iova, bool is_write,
                             uint64_t *slptep, uint32_t *slpte_level,
                             bool *reads, bool *writes, uint8_t aw_bits)
{
    dma_addr_t addr = vtd_ce_get_slpt_base(ce);
    uint32_t level = vtd_ce_get_level(ce);
    uint32_t offset;
    uint64_t slpte;
    uint64_t access_right_check;

    if (!vtd_iova_range_check(iova, ce, aw_bits)) {
        trace_vtd_err_dmar_iova_overflow(iova);
        return -VTD_FR_ADDR_BEYOND_MGAW;
    }

    /* FIXME: what is the Atomics request here? */
    access_right_check = is_write ? VTD_SL_W : VTD_SL_R;

    while (true) {
        offset = vtd_iova_level_offset(iova, level);
        slpte = vtd_get_slpte(addr, offset);

        if (slpte == (uint64_t)-1) {
            trace_vtd_err_dmar_slpte_read_error(iova, level);
            if (level == vtd_ce_get_level(ce)) {
                /* Invalid programming of context-entry */
                return -VTD_FR_CONTEXT_ENTRY_INV;
            } else {
                return -VTD_FR_PAGING_ENTRY_INV;
            }
        }
        *reads = (*reads) && (slpte & VTD_SL_R);
        *writes = (*writes) && (slpte & VTD_SL_W);
        if (!(slpte & access_right_check)) {
            trace_vtd_err_dmar_slpte_perm_error(iova, level, slpte, is_write);
            return is_write ? -VTD_FR_WRITE : -VTD_FR_READ;
        }
        if (vtd_slpte_nonzero_rsvd(slpte, level)) {
            trace_vtd_err_dmar_slpte_resv_error(iova, level, slpte);
            return -VTD_FR_PAGING_ENTRY_RSVD;
        }

        if (vtd_is_last_slpte(slpte, level)) {
            *slptep = slpte;
            *slpte_level = level;
            return 0;
        }
        addr = vtd_get_slpte_addr(slpte, aw_bits);
        level--;
    }
}

typedef int (*vtd_page_walk_hook)(IOMMUTLBEntry *entry, void *private);

/**
 * vtd_page_walk_level - walk over specific level for IOVA range
 *
 * @addr: base GPA addr to start the walk
 * @start: IOVA range start address
 * @end: IOVA range end address (start <= addr < end)
 * @hook_fn: hook func to be called when detected page
 * @private: private data to be passed into hook func
 * @read: whether parent level has read permission
 * @write: whether parent level has write permission
 * @notify_unmap: whether we should notify invalid entries
 * @aw: maximum address width
 */
static int vtd_page_walk_level(dma_addr_t addr, uint64_t start,
                               uint64_t end, vtd_page_walk_hook hook_fn,
                               void *private, uint32_t level, bool read,
                               bool write, bool notify_unmap, uint8_t aw)
{
    bool read_cur, write_cur, entry_valid;
    uint32_t offset;
    uint64_t slpte;
    uint64_t subpage_size, subpage_mask;
    IOMMUTLBEntry entry;
    uint64_t iova = start;
    uint64_t iova_next;
    int ret = 0;

    trace_vtd_page_walk_level(addr, level, start, end);

    subpage_size = 1ULL << vtd_slpt_level_shift(level);
    subpage_mask = vtd_slpt_level_page_mask(level);

    while (iova < end) {
        iova_next = (iova & subpage_mask) + subpage_size;

        offset = vtd_iova_level_offset(iova, level);
        slpte = vtd_get_slpte(addr, offset);

        if (slpte == (uint64_t)-1) {
            trace_vtd_page_walk_skip_read(iova, iova_next);
            goto next;
        }

        if (vtd_slpte_nonzero_rsvd(slpte, level)) {
            trace_vtd_page_walk_skip_reserve(iova, iova_next);
            goto next;
        }

        /* Permissions are stacked with parents' */
        read_cur = read && (slpte & VTD_SL_R);
        write_cur = write && (slpte & VTD_SL_W);

        /*
         * As long as we have either read/write permission, this is a
         * valid entry. The rule works for both page entries and page
         * table entries.
         */
        entry_valid = read_cur | write_cur;

        if (vtd_is_last_slpte(slpte, level)) {
            entry.target_as = &address_space_memory;
            entry.iova = iova & subpage_mask;
            /* NOTE: this is only meaningful if entry_valid == true */
            entry.translated_addr = vtd_get_slpte_addr(slpte, aw);
            entry.addr_mask = ~subpage_mask;
            entry.perm = IOMMU_ACCESS_FLAG(read_cur, write_cur);
            if (!entry_valid && !notify_unmap) {
                trace_vtd_page_walk_skip_perm(iova, iova_next);
                goto next;
            }
            trace_vtd_page_walk_one(level, entry.iova, entry.translated_addr,
                                    entry.addr_mask, entry.perm);
            if (hook_fn) {
                ret = hook_fn(&entry, private);
                if (ret < 0) {
                    return ret;
                }
            }
        } else {
            if (!entry_valid) {
                trace_vtd_page_walk_skip_perm(iova, iova_next);
                goto next;
            }
            ret = vtd_page_walk_level(vtd_get_slpte_addr(slpte, aw), iova,
                                      MIN(iova_next, end), hook_fn, private,
                                      level - 1, read_cur, write_cur,
                                      notify_unmap, aw);
            if (ret < 0) {
                return ret;
            }
        }

next:
        iova = iova_next;
    }

    return 0;
}

/**
 * vtd_page_walk - walk specific IOVA range, and call the hook
 *
 * @ce: context entry to walk upon
 * @start: IOVA address to start the walk
 * @end: IOVA range end address (start <= addr < end)
 * @hook_fn: the hook that to be called for each detected area
 * @private: private data for the hook function
 * @aw: maximum address width
 */
static int vtd_page_walk(VTDContextEntry *ce, uint64_t start, uint64_t end,
                         vtd_page_walk_hook hook_fn, void *private,
                         bool notify_unmap, uint8_t aw)
{
    dma_addr_t addr = vtd_ce_get_slpt_base(ce);
    uint32_t level = vtd_ce_get_level(ce);

    if (!vtd_iova_range_check(start, ce, aw)) {
        return -VTD_FR_ADDR_BEYOND_MGAW;
    }

    if (!vtd_iova_range_check(end, ce, aw)) {
        /* Fix end so that it reaches the maximum */
        end = vtd_iova_limit(ce, aw);
    }

    return vtd_page_walk_level(addr, start, end, hook_fn, private,
                               level, true, true, notify_unmap, aw);
}

/* Map a device to its corresponding domain (context-entry) */
static int vtd_dev_to_context_entry(IntelIOMMUState *s, uint8_t bus_num,
                                    uint8_t devfn, VTDContextEntry *ce)
{
    VTDRootEntry re;
    int ret_fr;
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    ret_fr = vtd_get_root_entry(s, bus_num, &re);
    if (ret_fr) {
        return ret_fr;
    }

    if (!vtd_root_entry_present(&re)) {
        /* Not error - it's okay we don't have root entry. */
        trace_vtd_re_not_present(bus_num);
        return -VTD_FR_ROOT_ENTRY_P;
    }

    if (re.rsvd || (re.val & VTD_ROOT_ENTRY_RSVD(s->aw_bits))) {
        trace_vtd_re_invalid(re.rsvd, re.val);
        return -VTD_FR_ROOT_ENTRY_RSVD;
    }

    ret_fr = vtd_get_context_entry_from_root(&re, devfn, ce);
    if (ret_fr) {
        return ret_fr;
    }

    if (!vtd_ce_present(ce)) {
        /* Not error - it's okay we don't have context entry. */
        trace_vtd_ce_not_present(bus_num, devfn);
        return -VTD_FR_CONTEXT_ENTRY_P;
    }

    if ((ce->hi & VTD_CONTEXT_ENTRY_RSVD_HI) ||
               (ce->lo & VTD_CONTEXT_ENTRY_RSVD_LO(s->aw_bits))) {
        trace_vtd_ce_invalid(ce->hi, ce->lo);
        return -VTD_FR_CONTEXT_ENTRY_RSVD;
    }

    /* Check if the programming of context-entry is valid */
    if (!vtd_is_level_supported(s, vtd_ce_get_level(ce))) {
        trace_vtd_ce_invalid(ce->hi, ce->lo);
        return -VTD_FR_CONTEXT_ENTRY_INV;
    }

    /* Do translation type check */
    if (!vtd_ce_type_check(x86_iommu, ce)) {
        trace_vtd_ce_invalid(ce->hi, ce->lo);
        return -VTD_FR_CONTEXT_ENTRY_INV;
    }

    return 0;
}

/*
 * Fetch translation type for specific device. Returns <0 if error
 * happens, otherwise return the shifted type to check against
 * VTD_CONTEXT_TT_*.
 */
static int vtd_dev_get_trans_type(VTDAddressSpace *as)
{
    IntelIOMMUState *s;
    VTDContextEntry ce;
    int ret;

    s = as->iommu_state;

    ret = vtd_dev_to_context_entry(s, pci_bus_num(as->bus),
                                   as->devfn, &ce);
    if (ret) {
        return ret;
    }

    return vtd_ce_get_type(&ce);
}

static bool vtd_dev_pt_enabled(VTDAddressSpace *as)
{
    int ret;

    assert(as);

    ret = vtd_dev_get_trans_type(as);
    if (ret < 0) {
        /*
         * Possibly failed to parse the context entry for some reason
         * (e.g., during init, or any guest configuration errors on
         * context entries). We should assume PT not enabled for
         * safety.
         */
        return false;
    }

    return ret == VTD_CONTEXT_TT_PASS_THROUGH;
}

/* Return whether the device is using IOMMU translation. */
static bool vtd_switch_address_space(VTDAddressSpace *as)
{
    bool use_iommu;
    /* Whether we need to take the BQL on our own */
    bool take_bql = !qemu_mutex_iothread_locked();

    assert(as);

    use_iommu = as->iommu_state->dmar_enabled & !vtd_dev_pt_enabled(as);

    trace_vtd_switch_address_space(pci_bus_num(as->bus),
                                   VTD_PCI_SLOT(as->devfn),
                                   VTD_PCI_FUNC(as->devfn),
                                   use_iommu);

    /*
     * It's possible that we reach here without BQL, e.g., when called
     * from vtd_pt_enable_fast_path(). However the memory APIs need
     * it. We'd better make sure we have had it already, or, take it.
     */
    if (take_bql) {
        qemu_mutex_lock_iothread();
    }

    /* Turn off first then on the other */
    if (use_iommu) {
        memory_region_set_enabled(&as->sys_alias, false);
        memory_region_set_enabled(MEMORY_REGION(&as->iommu), true);
    } else {
        memory_region_set_enabled(MEMORY_REGION(&as->iommu), false);
        memory_region_set_enabled(&as->sys_alias, true);
    }

    if (take_bql) {
        qemu_mutex_unlock_iothread();
    }

    return use_iommu;
}

static void vtd_switch_address_space_all(IntelIOMMUState *s)
{
    GHashTableIter iter;
    VTDBus *vtd_bus;
    int i;

    g_hash_table_iter_init(&iter, s->vtd_as_by_busptr);
    while (g_hash_table_iter_next(&iter, NULL, (void **)&vtd_bus)) {
        for (i = 0; i < PCI_DEVFN_MAX; i++) {
            if (!vtd_bus->dev_as[i]) {
                continue;
            }
            vtd_switch_address_space(vtd_bus->dev_as[i]);
        }
    }
}

static inline uint16_t vtd_make_source_id(uint8_t bus_num, uint8_t devfn)
{
    return ((bus_num & 0xffUL) << 8) | (devfn & 0xffUL);
}

static const bool vtd_qualified_faults[] = {
    [VTD_FR_RESERVED] = false,
    [VTD_FR_ROOT_ENTRY_P] = false,
    [VTD_FR_CONTEXT_ENTRY_P] = true,
    [VTD_FR_CONTEXT_ENTRY_INV] = true,
    [VTD_FR_ADDR_BEYOND_MGAW] = true,
    [VTD_FR_WRITE] = true,
    [VTD_FR_READ] = true,
    [VTD_FR_PAGING_ENTRY_INV] = true,
    [VTD_FR_ROOT_TABLE_INV] = false,
    [VTD_FR_CONTEXT_TABLE_INV] = false,
    [VTD_FR_ROOT_ENTRY_RSVD] = false,
    [VTD_FR_PAGING_ENTRY_RSVD] = true,
    [VTD_FR_CONTEXT_ENTRY_TT] = true,
    [VTD_FR_RESERVED_ERR] = false,
    [VTD_FR_MAX] = false,
};

/* To see if a fault condition is "qualified", which is reported to software
 * only if the FPD field in the context-entry used to process the faulting
 * request is 0.
 */
static inline bool vtd_is_qualified_fault(VTDFaultReason fault)
{
    return vtd_qualified_faults[fault];
}

static inline bool vtd_is_interrupt_addr(hwaddr addr)
{
    return VTD_INTERRUPT_ADDR_FIRST <= addr && addr <= VTD_INTERRUPT_ADDR_LAST;
}

static void vtd_pt_enable_fast_path(IntelIOMMUState *s, uint16_t source_id)
{
    VTDBus *vtd_bus;
    VTDAddressSpace *vtd_as;
    bool success = false;

    vtd_bus = vtd_find_as_from_bus_num(s, VTD_SID_TO_BUS(source_id));
    if (!vtd_bus) {
        goto out;
    }

    vtd_as = vtd_bus->dev_as[VTD_SID_TO_DEVFN(source_id)];
    if (!vtd_as) {
        goto out;
    }

    if (vtd_switch_address_space(vtd_as) == false) {
        /* We switched off IOMMU region successfully. */
        success = true;
    }

out:
    trace_vtd_pt_enable_fast_path(source_id, success);
}

/* Map dev to context-entry then do a paging-structures walk to do a iommu
 * translation.
 *
 * Called from RCU critical section.
 *
 * @bus_num: The bus number
 * @devfn: The devfn, which is the  combined of device and function number
 * @is_write: The access is a write operation
 * @entry: IOMMUTLBEntry that contain the addr to be translated and result
 *
 * Returns true if translation is successful, otherwise false.
 */
static bool vtd_do_iommu_translate(VTDAddressSpace *vtd_as, PCIBus *bus,
                                   uint8_t devfn, hwaddr addr, bool is_write,
                                   IOMMUTLBEntry *entry)
{
    IntelIOMMUState *s = vtd_as->iommu_state;
    VTDContextEntry ce;
    uint8_t bus_num = pci_bus_num(bus);
    VTDContextCacheEntry *cc_entry = &vtd_as->context_cache_entry;
    uint64_t slpte, page_mask;
    uint32_t level;
    uint16_t source_id = vtd_make_source_id(bus_num, devfn);
    int ret_fr;
    bool is_fpd_set = false;
    bool reads = true;
    bool writes = true;
    uint8_t access_flags;
    VTDIOTLBEntry *iotlb_entry;

    /*
     * We have standalone memory region for interrupt addresses, we
     * should never receive translation requests in this region.
     */
    assert(!vtd_is_interrupt_addr(addr));

    /* Try to fetch slpte form IOTLB */
    iotlb_entry = vtd_lookup_iotlb(s, source_id, addr);
    if (iotlb_entry) {
        trace_vtd_iotlb_page_hit(source_id, addr, iotlb_entry->slpte,
                                 iotlb_entry->domain_id);
        slpte = iotlb_entry->slpte;
        access_flags = iotlb_entry->access_flags;
        page_mask = iotlb_entry->mask;
        goto out;
    }

    /* Try to fetch context-entry from cache first */
    if (cc_entry->context_cache_gen == s->context_cache_gen) {
        trace_vtd_iotlb_cc_hit(bus_num, devfn, cc_entry->context_entry.hi,
                               cc_entry->context_entry.lo,
                               cc_entry->context_cache_gen);
        ce = cc_entry->context_entry;
        is_fpd_set = ce.lo & VTD_CONTEXT_ENTRY_FPD;
    } else {
        ret_fr = vtd_dev_to_context_entry(s, bus_num, devfn, &ce);
        is_fpd_set = ce.lo & VTD_CONTEXT_ENTRY_FPD;
        if (ret_fr) {
            ret_fr = -ret_fr;
            if (is_fpd_set && vtd_is_qualified_fault(ret_fr)) {
                trace_vtd_fault_disabled();
            } else {
                vtd_report_dmar_fault(s, source_id, addr, ret_fr, is_write);
            }
            goto error;
        }
        /* Update context-cache */
        trace_vtd_iotlb_cc_update(bus_num, devfn, ce.hi, ce.lo,
                                  cc_entry->context_cache_gen,
                                  s->context_cache_gen);
        cc_entry->context_entry = ce;
        cc_entry->context_cache_gen = s->context_cache_gen;
    }

    /*
     * We don't need to translate for pass-through context entries.
     * Also, let's ignore IOTLB caching as well for PT devices.
     */
    if (vtd_ce_get_type(&ce) == VTD_CONTEXT_TT_PASS_THROUGH) {
        entry->iova = addr & VTD_PAGE_MASK_4K;
        entry->translated_addr = entry->iova;
        entry->addr_mask = ~VTD_PAGE_MASK_4K;
        entry->perm = IOMMU_RW;
        trace_vtd_translate_pt(source_id, entry->iova);

        /*
         * When this happens, it means firstly caching-mode is not
         * enabled, and this is the first passthrough translation for
         * the device. Let's enable the fast path for passthrough.
         *
         * When passthrough is disabled again for the device, we can
         * capture it via the context entry invalidation, then the
         * IOMMU region can be swapped back.
         */
        vtd_pt_enable_fast_path(s, source_id);

        return true;
    }

    ret_fr = vtd_iova_to_slpte(&ce, addr, is_write, &slpte, &level,
                               &reads, &writes, s->aw_bits);
    if (ret_fr) {
        ret_fr = -ret_fr;
        if (is_fpd_set && vtd_is_qualified_fault(ret_fr)) {
            trace_vtd_fault_disabled();
        } else {
            vtd_report_dmar_fault(s, source_id, addr, ret_fr, is_write);
        }
        goto error;
    }

    page_mask = vtd_slpt_level_page_mask(level);
    access_flags = IOMMU_ACCESS_FLAG(reads, writes);
    vtd_update_iotlb(s, source_id, VTD_CONTEXT_ENTRY_DID(ce.hi), addr, slpte,
                     access_flags, level);
out:
    entry->iova = addr & page_mask;
    entry->translated_addr = vtd_get_slpte_addr(slpte, s->aw_bits) & page_mask;
    entry->addr_mask = ~page_mask;
    entry->perm = access_flags;
    return true;

error:
    entry->iova = 0;
    entry->translated_addr = 0;
    entry->addr_mask = 0;
    entry->perm = IOMMU_NONE;
    return false;
}

static void vtd_root_table_setup(IntelIOMMUState *s)
{
    s->root = vtd_get_quad_raw(s, DMAR_RTADDR_REG);
    s->root_extended = s->root & VTD_RTADDR_RTT;
    s->root &= VTD_RTADDR_ADDR_MASK(s->aw_bits);

    trace_vtd_reg_dmar_root(s->root, s->root_extended);
}

static void vtd_iec_notify_all(IntelIOMMUState *s, bool global,
                               uint32_t index, uint32_t mask)
{
    x86_iommu_iec_notify_all(X86_IOMMU_DEVICE(s), global, index, mask);
}

static void vtd_interrupt_remap_table_setup(IntelIOMMUState *s)
{
    uint64_t value = 0;
    value = vtd_get_quad_raw(s, DMAR_IRTA_REG);
    s->intr_size = 1UL << ((value & VTD_IRTA_SIZE_MASK) + 1);
    s->intr_root = value & VTD_IRTA_ADDR_MASK(s->aw_bits);
    s->intr_eime = value & VTD_IRTA_EIME;

    /* Notify global invalidation */
    vtd_iec_notify_all(s, true, 0, 0);

    trace_vtd_reg_ir_root(s->intr_root, s->intr_size);
}

static void vtd_iommu_replay_all(IntelIOMMUState *s)
{
    IntelIOMMUNotifierNode *node;

    QLIST_FOREACH(node, &s->notifiers_list, next) {
        memory_region_iommu_replay_all(&node->vtd_as->iommu);
    }
}

static void vtd_context_global_invalidate(IntelIOMMUState *s)
{
    trace_vtd_inv_desc_cc_global();
    s->context_cache_gen++;
    if (s->context_cache_gen == VTD_CONTEXT_CACHE_GEN_MAX) {
        vtd_reset_context_cache(s);
    }
    vtd_switch_address_space_all(s);
    /*
     * From VT-d spec 6.5.2.1, a global context entry invalidation
     * should be followed by a IOTLB global invalidation, so we should
     * be safe even without this. Hoewever, let's replay the region as
     * well to be safer, and go back here when we need finer tunes for
     * VT-d emulation codes.
     */
    vtd_iommu_replay_all(s);
}

/* Do a context-cache device-selective invalidation.
 * @func_mask: FM field after shifting
 */
static void vtd_context_device_invalidate(IntelIOMMUState *s,
                                          uint16_t source_id,
                                          uint16_t func_mask)
{
    uint16_t mask;
    VTDBus *vtd_bus;
    VTDAddressSpace *vtd_as;
    uint8_t bus_n, devfn;
    uint16_t devfn_it;

    trace_vtd_inv_desc_cc_devices(source_id, func_mask);

    switch (func_mask & 3) {
    case 0:
        mask = 0;   /* No bits in the SID field masked */
        break;
    case 1:
        mask = 4;   /* Mask bit 2 in the SID field */
        break;
    case 2:
        mask = 6;   /* Mask bit 2:1 in the SID field */
        break;
    case 3:
        mask = 7;   /* Mask bit 2:0 in the SID field */
        break;
    }
    mask = ~mask;

    bus_n = VTD_SID_TO_BUS(source_id);
    vtd_bus = vtd_find_as_from_bus_num(s, bus_n);
    if (vtd_bus) {
        devfn = VTD_SID_TO_DEVFN(source_id);
        for (devfn_it = 0; devfn_it < PCI_DEVFN_MAX; ++devfn_it) {
            vtd_as = vtd_bus->dev_as[devfn_it];
            if (vtd_as && ((devfn_it & mask) == (devfn & mask))) {
                trace_vtd_inv_desc_cc_device(bus_n, VTD_PCI_SLOT(devfn_it),
                                             VTD_PCI_FUNC(devfn_it));
                vtd_as->context_cache_entry.context_cache_gen = 0;
                /*
                 * Do switch address space when needed, in case if the
                 * device passthrough bit is switched.
                 */
                vtd_switch_address_space(vtd_as);
                /*
                 * So a device is moving out of (or moving into) a
                 * domain, a replay() suites here to notify all the
                 * IOMMU_NOTIFIER_MAP registers about this change.
                 * This won't bring bad even if we have no such
                 * notifier registered - the IOMMU notification
                 * framework will skip MAP notifications if that
                 * happened.
                 */
                memory_region_iommu_replay_all(&vtd_as->iommu);
            }
        }
    }
}

/* Context-cache invalidation
 * Returns the Context Actual Invalidation Granularity.
 * @val: the content of the CCMD_REG
 */
static uint64_t vtd_context_cache_invalidate(IntelIOMMUState *s, uint64_t val)
{
    uint64_t caig;
    uint64_t type = val & VTD_CCMD_CIRG_MASK;

    switch (type) {
    case VTD_CCMD_DOMAIN_INVL:
        /* Fall through */
    case VTD_CCMD_GLOBAL_INVL:
        caig = VTD_CCMD_GLOBAL_INVL_A;
        vtd_context_global_invalidate(s);
        break;

    case VTD_CCMD_DEVICE_INVL:
        caig = VTD_CCMD_DEVICE_INVL_A;
        vtd_context_device_invalidate(s, VTD_CCMD_SID(val), VTD_CCMD_FM(val));
        break;

    default:
        trace_vtd_err("Context cache invalidate type error.");
        caig = 0;
    }
    return caig;
}

static void vtd_iotlb_global_invalidate(IntelIOMMUState *s)
{
    trace_vtd_inv_desc_iotlb_global();
    vtd_reset_iotlb(s);
    vtd_iommu_replay_all(s);
}

static void vtd_iotlb_domain_invalidate(IntelIOMMUState *s, uint16_t domain_id)
{
    IntelIOMMUNotifierNode *node;
    VTDContextEntry ce;
    VTDAddressSpace *vtd_as;

    trace_vtd_inv_desc_iotlb_domain(domain_id);

    g_hash_table_foreach_remove(s->iotlb, vtd_hash_remove_by_domain,
                                &domain_id);

    QLIST_FOREACH(node, &s->notifiers_list, next) {
        vtd_as = node->vtd_as;
        if (!vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                      vtd_as->devfn, &ce) &&
            domain_id == VTD_CONTEXT_ENTRY_DID(ce.hi)) {
            memory_region_iommu_replay_all(&vtd_as->iommu);
        }
    }
}

static int vtd_page_invalidate_notify_hook(IOMMUTLBEntry *entry,
                                           void *private)
{
    memory_region_notify_iommu((IOMMUMemoryRegion *)private, *entry);
    return 0;
}

static void vtd_iotlb_page_invalidate_notify(IntelIOMMUState *s,
                                           uint16_t domain_id, hwaddr addr,
                                           uint8_t am)
{
    IntelIOMMUNotifierNode *node;
    VTDContextEntry ce;
    int ret;

    QLIST_FOREACH(node, &(s->notifiers_list), next) {
        VTDAddressSpace *vtd_as = node->vtd_as;
        ret = vtd_dev_to_context_entry(s, pci_bus_num(vtd_as->bus),
                                       vtd_as->devfn, &ce);
        if (!ret && domain_id == VTD_CONTEXT_ENTRY_DID(ce.hi)) {
            vtd_page_walk(&ce, addr, addr + (1 << am) * VTD_PAGE_SIZE,
                          vtd_page_invalidate_notify_hook,
                          (void *)&vtd_as->iommu, true, s->aw_bits);
        }
    }
}

static void vtd_iotlb_page_invalidate(IntelIOMMUState *s, uint16_t domain_id,
                                      hwaddr addr, uint8_t am)
{
    VTDIOTLBPageInvInfo info;

    trace_vtd_inv_desc_iotlb_pages(domain_id, addr, am);

    assert(am <= VTD_MAMV);
    info.domain_id = domain_id;
    info.addr = addr;
    info.mask = ~((1 << am) - 1);
    g_hash_table_foreach_remove(s->iotlb, vtd_hash_remove_by_page, &info);
    vtd_iotlb_page_invalidate_notify(s, domain_id, addr, am);
}

/* Flush IOTLB
 * Returns the IOTLB Actual Invalidation Granularity.
 * @val: the content of the IOTLB_REG
 */
static uint64_t vtd_iotlb_flush(IntelIOMMUState *s, uint64_t val)
{
    uint64_t iaig;
    uint64_t type = val & VTD_TLB_FLUSH_GRANU_MASK;
    uint16_t domain_id;
    hwaddr addr;
    uint8_t am;

    switch (type) {
    case VTD_TLB_GLOBAL_FLUSH:
        iaig = VTD_TLB_GLOBAL_FLUSH_A;
        vtd_iotlb_global_invalidate(s);
        break;

    case VTD_TLB_DSI_FLUSH:
        domain_id = VTD_TLB_DID(val);
        iaig = VTD_TLB_DSI_FLUSH_A;
        vtd_iotlb_domain_invalidate(s, domain_id);
        break;

    case VTD_TLB_PSI_FLUSH:
        domain_id = VTD_TLB_DID(val);
        addr = vtd_get_quad_raw(s, DMAR_IVA_REG);
        am = VTD_IVA_AM(addr);
        addr = VTD_IVA_ADDR(addr);
        if (am > VTD_MAMV) {
            trace_vtd_err("IOTLB PSI flush: address mask overflow.");
            iaig = 0;
            break;
        }
        iaig = VTD_TLB_PSI_FLUSH_A;
        vtd_iotlb_page_invalidate(s, domain_id, addr, am);
        break;

    default:
        trace_vtd_err("IOTLB flush: invalid granularity.");
        iaig = 0;
    }
    return iaig;
}

static void vtd_fetch_inv_desc(IntelIOMMUState *s);

static inline bool vtd_queued_inv_disable_check(IntelIOMMUState *s)
{
    return s->qi_enabled && (s->iq_tail == s->iq_head) &&
           (s->iq_last_desc_type == VTD_INV_DESC_WAIT);
}

static void vtd_handle_gcmd_qie(IntelIOMMUState *s, bool en)
{
    uint64_t iqa_val = vtd_get_quad_raw(s, DMAR_IQA_REG);

    trace_vtd_inv_qi_enable(en);

    if (en) {
        s->iq = iqa_val & VTD_IQA_IQA_MASK(s->aw_bits);
        /* 2^(x+8) entries */
        s->iq_size = 1UL << ((iqa_val & VTD_IQA_QS) + 8);
        s->qi_enabled = true;
        trace_vtd_inv_qi_setup(s->iq, s->iq_size);
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_QIES);

        if (s->iq_tail != 0) {
            /*
             * This is a spec violation but Windows guests are known to set up
             * Queued Invalidation this way so we allow the write and process
             * Invalidation Descriptors right away.
             */
            trace_vtd_warn_invalid_qi_tail(s->iq_tail);
            if (!(vtd_get_long_raw(s, DMAR_FSTS_REG) & VTD_FSTS_IQE)) {
                vtd_fetch_inv_desc(s);
            }
        }
    } else {
        if (vtd_queued_inv_disable_check(s)) {
            /* disable Queued Invalidation */
            vtd_set_quad_raw(s, DMAR_IQH_REG, 0);
            s->iq_head = 0;
            s->qi_enabled = false;
            /* Ok - report back to driver */
            vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_QIES, 0);
        } else {
            trace_vtd_err_qi_disable(s->iq_head, s->iq_tail, s->iq_last_desc_type);
        }
    }
}

/* Set Root Table Pointer */
static void vtd_handle_gcmd_srtp(IntelIOMMUState *s)
{
    vtd_root_table_setup(s);
    /* Ok - report back to driver */
    vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_RTPS);
}

/* Set Interrupt Remap Table Pointer */
static void vtd_handle_gcmd_sirtp(IntelIOMMUState *s)
{
    vtd_interrupt_remap_table_setup(s);
    /* Ok - report back to driver */
    vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_IRTPS);
}

/* Handle Translation Enable/Disable */
static void vtd_handle_gcmd_te(IntelIOMMUState *s, bool en)
{
    if (s->dmar_enabled == en) {
        return;
    }

    trace_vtd_dmar_enable(en);

    if (en) {
        s->dmar_enabled = true;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_TES);
    } else {
        s->dmar_enabled = false;

        /* Clear the index of Fault Recording Register */
        s->next_frcd_reg = 0;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_TES, 0);
    }

    vtd_switch_address_space_all(s);
}

/* Handle Interrupt Remap Enable/Disable */
static void vtd_handle_gcmd_ire(IntelIOMMUState *s, bool en)
{
    trace_vtd_ir_enable(en);

    if (en) {
        s->intr_enabled = true;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, 0, VTD_GSTS_IRES);
    } else {
        s->intr_enabled = false;
        /* Ok - report back to driver */
        vtd_set_clear_mask_long(s, DMAR_GSTS_REG, VTD_GSTS_IRES, 0);
    }
}

/* Handle write to Global Command Register */
static void vtd_handle_gcmd_write(IntelIOMMUState *s)
{
    uint32_t status = vtd_get_long_raw(s, DMAR_GSTS_REG);
    uint32_t val = vtd_get_long_raw(s, DMAR_GCMD_REG);
    uint32_t changed = status ^ val;

    trace_vtd_reg_write_gcmd(status, val);
    if (changed & VTD_GCMD_TE) {
        /* Translation enable/disable */
        vtd_handle_gcmd_te(s, val & VTD_GCMD_TE);
    }
    if (val & VTD_GCMD_SRTP) {
        /* Set/update the root-table pointer */
        vtd_handle_gcmd_srtp(s);
    }
    if (changed & VTD_GCMD_QIE) {
        /* Queued Invalidation Enable */
        vtd_handle_gcmd_qie(s, val & VTD_GCMD_QIE);
    }
    if (val & VTD_GCMD_SIRTP) {
        /* Set/update the interrupt remapping root-table pointer */
        vtd_handle_gcmd_sirtp(s);
    }
    if (changed & VTD_GCMD_IRE) {
        /* Interrupt remap enable/disable */
        vtd_handle_gcmd_ire(s, val & VTD_GCMD_IRE);
    }
}

/* Handle write to Context Command Register */
static void vtd_handle_ccmd_write(IntelIOMMUState *s)
{
    uint64_t ret;
    uint64_t val = vtd_get_quad_raw(s, DMAR_CCMD_REG);

    /* Context-cache invalidation request */
    if (val & VTD_CCMD_ICC) {
        if (s->qi_enabled) {
            trace_vtd_err("Queued Invalidation enabled, "
                          "should not use register-based invalidation");
            return;
        }
        ret = vtd_context_cache_invalidate(s, val);
        /* Invalidation completed. Change something to show */
        vtd_set_clear_mask_quad(s, DMAR_CCMD_REG, VTD_CCMD_ICC, 0ULL);
        ret = vtd_set_clear_mask_quad(s, DMAR_CCMD_REG, VTD_CCMD_CAIG_MASK,
                                      ret);
    }
}

/* Handle write to IOTLB Invalidation Register */
static void vtd_handle_iotlb_write(IntelIOMMUState *s)
{
    uint64_t ret;
    uint64_t val = vtd_get_quad_raw(s, DMAR_IOTLB_REG);

    /* IOTLB invalidation request */
    if (val & VTD_TLB_IVT) {
        if (s->qi_enabled) {
            trace_vtd_err("Queued Invalidation enabled, "
                          "should not use register-based invalidation.");
            return;
        }
        ret = vtd_iotlb_flush(s, val);
        /* Invalidation completed. Change something to show */
        vtd_set_clear_mask_quad(s, DMAR_IOTLB_REG, VTD_TLB_IVT, 0ULL);
        ret = vtd_set_clear_mask_quad(s, DMAR_IOTLB_REG,
                                      VTD_TLB_FLUSH_GRANU_MASK_A, ret);
    }
}

/* Fetch an Invalidation Descriptor from the Invalidation Queue */
static bool vtd_get_inv_desc(dma_addr_t base_addr, uint32_t offset,
                             VTDInvDesc *inv_desc)
{
    dma_addr_t addr = base_addr + offset * sizeof(*inv_desc);
    if (dma_memory_read(&address_space_memory, addr, inv_desc,
        sizeof(*inv_desc))) {
        trace_vtd_err("Read INV DESC failed.");
        inv_desc->lo = 0;
        inv_desc->hi = 0;
        return false;
    }
    inv_desc->lo = le64_to_cpu(inv_desc->lo);
    inv_desc->hi = le64_to_cpu(inv_desc->hi);
    return true;
}

static bool vtd_process_wait_desc(IntelIOMMUState *s, VTDInvDesc *inv_desc)
{
    if ((inv_desc->hi & VTD_INV_DESC_WAIT_RSVD_HI) ||
        (inv_desc->lo & VTD_INV_DESC_WAIT_RSVD_LO)) {
        trace_vtd_inv_desc_wait_invalid(inv_desc->hi, inv_desc->lo);
        return false;
    }
    if (inv_desc->lo & VTD_INV_DESC_WAIT_SW) {
        /* Status Write */
        uint32_t status_data = (uint32_t)(inv_desc->lo >>
                               VTD_INV_DESC_WAIT_DATA_SHIFT);

        assert(!(inv_desc->lo & VTD_INV_DESC_WAIT_IF));

        /* FIXME: need to be masked with HAW? */
        dma_addr_t status_addr = inv_desc->hi;
        trace_vtd_inv_desc_wait_sw(status_addr, status_data);
        status_data = cpu_to_le32(status_data);
        if (dma_memory_write(&address_space_memory, status_addr, &status_data,
                             sizeof(status_data))) {
            trace_vtd_inv_desc_wait_write_fail(inv_desc->hi, inv_desc->lo);
            return false;
        }
    } else if (inv_desc->lo & VTD_INV_DESC_WAIT_IF) {
        /* Interrupt flag */
        vtd_generate_completion_event(s);
    } else {
        trace_vtd_inv_desc_wait_invalid(inv_desc->hi, inv_desc->lo);
        return false;
    }
    return true;
}

static bool vtd_process_context_cache_desc(IntelIOMMUState *s,
                                           VTDInvDesc *inv_desc)
{
    uint16_t sid, fmask;

    if ((inv_desc->lo & VTD_INV_DESC_CC_RSVD) || inv_desc->hi) {
        trace_vtd_inv_desc_cc_invalid(inv_desc->hi, inv_desc->lo);
        return false;
    }
    switch (inv_desc->lo & VTD_INV_DESC_CC_G) {
    case VTD_INV_DESC_CC_DOMAIN:
        trace_vtd_inv_desc_cc_domain(
            (uint16_t)VTD_INV_DESC_CC_DID(inv_desc->lo));
        /* Fall through */
    case VTD_INV_DESC_CC_GLOBAL:
        vtd_context_global_invalidate(s);
        break;

    case VTD_INV_DESC_CC_DEVICE:
        sid = VTD_INV_DESC_CC_SID(inv_desc->lo);
        fmask = VTD_INV_DESC_CC_FM(inv_desc->lo);
        vtd_context_device_invalidate(s, sid, fmask);
        break;

    default:
        trace_vtd_inv_desc_cc_invalid(inv_desc->hi, inv_desc->lo);
        return false;
    }
    return true;
}

static bool vtd_process_iotlb_desc(IntelIOMMUState *s, VTDInvDesc *inv_desc)
{
    uint16_t domain_id;
    uint8_t am;
    hwaddr addr;

    if ((inv_desc->lo & VTD_INV_DESC_IOTLB_RSVD_LO) ||
        (inv_desc->hi & VTD_INV_DESC_IOTLB_RSVD_HI)) {
        trace_vtd_inv_desc_iotlb_invalid(inv_desc->hi, inv_desc->lo);
        return false;
    }

    switch (inv_desc->lo & VTD_INV_DESC_IOTLB_G) {
    case VTD_INV_DESC_IOTLB_GLOBAL:
        vtd_iotlb_global_invalidate(s);
        break;

    case VTD_INV_DESC_IOTLB_DOMAIN:
        domain_id = VTD_INV_DESC_IOTLB_DID(inv_desc->lo);
        vtd_iotlb_domain_invalidate(s, domain_id);
        break;

    case VTD_INV_DESC_IOTLB_PAGE:
        domain_id = VTD_INV_DESC_IOTLB_DID(inv_desc->lo);
        addr = VTD_INV_DESC_IOTLB_ADDR(inv_desc->hi);
        am = VTD_INV_DESC_IOTLB_AM(inv_desc->hi);
        if (am > VTD_MAMV) {
            trace_vtd_inv_desc_iotlb_invalid(inv_desc->hi, inv_desc->lo);
            return false;
        }
        vtd_iotlb_page_invalidate(s, domain_id, addr, am);
        break;

    default:
        trace_vtd_inv_desc_iotlb_invalid(inv_desc->hi, inv_desc->lo);
        return false;
    }
    return true;
}

static bool vtd_process_inv_iec_desc(IntelIOMMUState *s,
                                     VTDInvDesc *inv_desc)
{
    trace_vtd_inv_desc_iec(inv_desc->iec.granularity,
                           inv_desc->iec.index,
                           inv_desc->iec.index_mask);

    vtd_iec_notify_all(s, !inv_desc->iec.granularity,
                       inv_desc->iec.index,
                       inv_desc->iec.index_mask);
    return true;
}

static bool vtd_process_device_iotlb_desc(IntelIOMMUState *s,
                                          VTDInvDesc *inv_desc)
{
    VTDAddressSpace *vtd_dev_as;
    IOMMUTLBEntry entry;
    struct VTDBus *vtd_bus;
    hwaddr addr;
    uint64_t sz;
    uint16_t sid;
    uint8_t devfn;
    bool size;
    uint8_t bus_num;

    addr = VTD_INV_DESC_DEVICE_IOTLB_ADDR(inv_desc->hi);
    sid = VTD_INV_DESC_DEVICE_IOTLB_SID(inv_desc->lo);
    devfn = sid & 0xff;
    bus_num = sid >> 8;
    size = VTD_INV_DESC_DEVICE_IOTLB_SIZE(inv_desc->hi);

    if ((inv_desc->lo & VTD_INV_DESC_DEVICE_IOTLB_RSVD_LO) ||
        (inv_desc->hi & VTD_INV_DESC_DEVICE_IOTLB_RSVD_HI)) {
        trace_vtd_inv_desc_iotlb_invalid(inv_desc->hi, inv_desc->lo);
        return false;
    }

    vtd_bus = vtd_find_as_from_bus_num(s, bus_num);
    if (!vtd_bus) {
        goto done;
    }

    vtd_dev_as = vtd_bus->dev_as[devfn];
    if (!vtd_dev_as) {
        goto done;
    }

    /* According to ATS spec table 2.4:
     * S = 0, bits 15:12 = xxxx     range size: 4K
     * S = 1, bits 15:12 = xxx0     range size: 8K
     * S = 1, bits 15:12 = xx01     range size: 16K
     * S = 1, bits 15:12 = x011     range size: 32K
     * S = 1, bits 15:12 = 0111     range size: 64K
     * ...
     */
    if (size) {
        sz = (VTD_PAGE_SIZE * 2) << cto64(addr >> VTD_PAGE_SHIFT);
        addr &= ~(sz - 1);
    } else {
        sz = VTD_PAGE_SIZE;
    }

    entry.target_as = &vtd_dev_as->as;
    entry.addr_mask = sz - 1;
    entry.iova = addr;
    entry.perm = IOMMU_NONE;
    entry.translated_addr = 0;
    memory_region_notify_iommu(&vtd_dev_as->iommu, entry);

done:
    return true;
}

static bool vtd_process_inv_desc(IntelIOMMUState *s)
{
    VTDInvDesc inv_desc;
    uint8_t desc_type;

    trace_vtd_inv_qi_head(s->iq_head);
    if (!vtd_get_inv_desc(s->iq, s->iq_head, &inv_desc)) {
        s->iq_last_desc_type = VTD_INV_DESC_NONE;
        return false;
    }
    desc_type = inv_desc.lo & VTD_INV_DESC_TYPE;
    /* FIXME: should update at first or at last? */
    s->iq_last_desc_type = desc_type;

    switch (desc_type) {
    case VTD_INV_DESC_CC:
        trace_vtd_inv_desc("context-cache", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_context_cache_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_IOTLB:
        trace_vtd_inv_desc("iotlb", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_iotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_WAIT:
        trace_vtd_inv_desc("wait", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_wait_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_IEC:
        trace_vtd_inv_desc("iec", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_inv_iec_desc(s, &inv_desc)) {
            return false;
        }
        break;

    case VTD_INV_DESC_DEVICE:
        trace_vtd_inv_desc("device", inv_desc.hi, inv_desc.lo);
        if (!vtd_process_device_iotlb_desc(s, &inv_desc)) {
            return false;
        }
        break;

    default:
        trace_vtd_inv_desc_invalid(inv_desc.hi, inv_desc.lo);
        return false;
    }
    s->iq_head++;
    if (s->iq_head == s->iq_size) {
        s->iq_head = 0;
    }
    return true;
}

/* Try to fetch and process more Invalidation Descriptors */
static void vtd_fetch_inv_desc(IntelIOMMUState *s)
{
    trace_vtd_inv_qi_fetch();

    if (s->iq_tail >= s->iq_size) {
        /* Detects an invalid Tail pointer */
        trace_vtd_err_qi_tail(s->iq_tail, s->iq_size);
        vtd_handle_inv_queue_error(s);
        return;
    }
    while (s->iq_head != s->iq_tail) {
        if (!vtd_process_inv_desc(s)) {
            /* Invalidation Queue Errors */
            vtd_handle_inv_queue_error(s);
            break;
        }
        /* Must update the IQH_REG in time */
        vtd_set_quad_raw(s, DMAR_IQH_REG,
                         (((uint64_t)(s->iq_head)) << VTD_IQH_QH_SHIFT) &
                         VTD_IQH_QH_MASK);
    }
}

/* Handle write to Invalidation Queue Tail Register */
static void vtd_handle_iqt_write(IntelIOMMUState *s)
{
    uint64_t val = vtd_get_quad_raw(s, DMAR_IQT_REG);

    s->iq_tail = VTD_IQT_QT(val);
    trace_vtd_inv_qi_tail(s->iq_tail);

    if (s->qi_enabled && !(vtd_get_long_raw(s, DMAR_FSTS_REG) & VTD_FSTS_IQE)) {
        /* Process Invalidation Queue here */
        vtd_fetch_inv_desc(s);
    }
}

static void vtd_handle_fsts_write(IntelIOMMUState *s)
{
    uint32_t fsts_reg = vtd_get_long_raw(s, DMAR_FSTS_REG);
    uint32_t fectl_reg = vtd_get_long_raw(s, DMAR_FECTL_REG);
    uint32_t status_fields = VTD_FSTS_PFO | VTD_FSTS_PPF | VTD_FSTS_IQE;

    if ((fectl_reg & VTD_FECTL_IP) && !(fsts_reg & status_fields)) {
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
        trace_vtd_fsts_clear_ip();
    }
    /* FIXME: when IQE is Clear, should we try to fetch some Invalidation
     * Descriptors if there are any when Queued Invalidation is enabled?
     */
}

static void vtd_handle_fectl_write(IntelIOMMUState *s)
{
    uint32_t fectl_reg;
    /* FIXME: when software clears the IM field, check the IP field. But do we
     * need to compare the old value and the new value to conclude that
     * software clears the IM field? Or just check if the IM field is zero?
     */
    fectl_reg = vtd_get_long_raw(s, DMAR_FECTL_REG);

    trace_vtd_reg_write_fectl(fectl_reg);

    if ((fectl_reg & VTD_FECTL_IP) && !(fectl_reg & VTD_FECTL_IM)) {
        vtd_generate_interrupt(s, DMAR_FEADDR_REG, DMAR_FEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_FECTL_REG, VTD_FECTL_IP, 0);
    }
}

static void vtd_handle_ics_write(IntelIOMMUState *s)
{
    uint32_t ics_reg = vtd_get_long_raw(s, DMAR_ICS_REG);
    uint32_t iectl_reg = vtd_get_long_raw(s, DMAR_IECTL_REG);

    if ((iectl_reg & VTD_IECTL_IP) && !(ics_reg & VTD_ICS_IWC)) {
        trace_vtd_reg_ics_clear_ip();
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static void vtd_handle_iectl_write(IntelIOMMUState *s)
{
    uint32_t iectl_reg;
    /* FIXME: when software clears the IM field, check the IP field. But do we
     * need to compare the old value and the new value to conclude that
     * software clears the IM field? Or just check if the IM field is zero?
     */
    iectl_reg = vtd_get_long_raw(s, DMAR_IECTL_REG);

    trace_vtd_reg_write_iectl(iectl_reg);

    if ((iectl_reg & VTD_IECTL_IP) && !(iectl_reg & VTD_IECTL_IM)) {
        vtd_generate_interrupt(s, DMAR_IEADDR_REG, DMAR_IEDATA_REG);
        vtd_set_clear_mask_long(s, DMAR_IECTL_REG, VTD_IECTL_IP, 0);
    }
}

static uint64_t vtd_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    IntelIOMMUState *s = opaque;
    uint64_t val;

    trace_vtd_reg_read(addr, size);

    if (addr + size > DMAR_REG_SIZE) {
        trace_vtd_err("Read MMIO over range.");
        return (uint64_t)-1;
    }

    switch (addr) {
    /* Root Table Address Register, 64-bit */
    case DMAR_RTADDR_REG:
        if (size == 4) {
            val = s->root & ((1ULL << 32) - 1);
        } else {
            val = s->root;
        }
        break;

    case DMAR_RTADDR_REG_HI:
        assert(size == 4);
        val = s->root >> 32;
        break;

    /* Invalidation Queue Address Register, 64-bit */
    case DMAR_IQA_REG:
        val = s->iq | (vtd_get_quad(s, DMAR_IQA_REG) & VTD_IQA_QS);
        if (size == 4) {
            val = val & ((1ULL << 32) - 1);
        }
        break;

    case DMAR_IQA_REG_HI:
        assert(size == 4);
        val = s->iq >> 32;
        break;

    default:
        if (size == 4) {
            val = vtd_get_long(s, addr);
        } else {
            val = vtd_get_quad(s, addr);
        }
    }

    return val;
}

static void vtd_mem_write(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
{
    IntelIOMMUState *s = opaque;

    trace_vtd_reg_write(addr, size, val);

    if (addr + size > DMAR_REG_SIZE) {
        trace_vtd_err("Write MMIO over range.");
        return;
    }

    switch (addr) {
    /* Global Command Register, 32-bit */
    case DMAR_GCMD_REG:
        vtd_set_long(s, addr, val);
        vtd_handle_gcmd_write(s);
        break;

    /* Context Command Register, 64-bit */
    case DMAR_CCMD_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            vtd_handle_ccmd_write(s);
        }
        break;

    case DMAR_CCMD_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_ccmd_write(s);
        break;

    /* IOTLB Invalidation Register, 64-bit */
    case DMAR_IOTLB_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            vtd_handle_iotlb_write(s);
        }
        break;

    case DMAR_IOTLB_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_iotlb_write(s);
        break;

    /* Invalidate Address Register, 64-bit */
    case DMAR_IVA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_IVA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Status Register, 32-bit */
    case DMAR_FSTS_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_fsts_write(s);
        break;

    /* Fault Event Control Register, 32-bit */
    case DMAR_FECTL_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_fectl_write(s);
        break;

    /* Fault Event Data Register, 32-bit */
    case DMAR_FEDATA_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Event Address Register, 32-bit */
    case DMAR_FEADDR_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            /*
             * While the register is 32-bit only, some guests (Xen...) write to
             * it with 64-bit.
             */
            vtd_set_quad(s, addr, val);
        }
        break;

    /* Fault Event Upper Address Register, 32-bit */
    case DMAR_FEUADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Protected Memory Enable Register, 32-bit */
    case DMAR_PMEN_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Root Table Address Register, 64-bit */
    case DMAR_RTADDR_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_RTADDR_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Queue Tail Register, 64-bit */
    case DMAR_IQT_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        vtd_handle_iqt_write(s);
        break;

    case DMAR_IQT_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* 19:63 of IQT_REG is RsvdZ, do nothing here */
        break;

    /* Invalidation Queue Address Register, 64-bit */
    case DMAR_IQA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_IQA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Completion Status Register, 32-bit */
    case DMAR_ICS_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_ics_write(s);
        break;

    /* Invalidation Event Control Register, 32-bit */
    case DMAR_IECTL_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        vtd_handle_iectl_write(s);
        break;

    /* Invalidation Event Data Register, 32-bit */
    case DMAR_IEDATA_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Event Address Register, 32-bit */
    case DMAR_IEADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Invalidation Event Upper Address Register, 32-bit */
    case DMAR_IEUADDR_REG:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    /* Fault Recording Registers, 128-bit */
    case DMAR_FRCD_REG_0_0:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_FRCD_REG_0_1:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    case DMAR_FRCD_REG_0_2:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
            /* May clear bit 127 (Fault), update PPF */
            vtd_update_fsts_ppf(s);
        }
        break;

    case DMAR_FRCD_REG_0_3:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        /* May clear bit 127 (Fault), update PPF */
        vtd_update_fsts_ppf(s);
        break;

    case DMAR_IRTA_REG:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
        break;

    case DMAR_IRTA_REG_HI:
        assert(size == 4);
        vtd_set_long(s, addr, val);
        break;

    default:
        if (size == 4) {
            vtd_set_long(s, addr, val);
        } else {
            vtd_set_quad(s, addr, val);
        }
    }
}

static IOMMUTLBEntry vtd_iommu_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                         IOMMUAccessFlags flag)
{
    VTDAddressSpace *vtd_as = container_of(iommu, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    IOMMUTLBEntry iotlb = {
        /* We'll fill in the rest later. */
        .target_as = &address_space_memory,
    };
    bool success;

    if (likely(s->dmar_enabled)) {
        success = vtd_do_iommu_translate(vtd_as, vtd_as->bus, vtd_as->devfn,
                                         addr, flag & IOMMU_WO, &iotlb);
    } else {
        /* DMAR disabled, passthrough, use 4k-page*/
        iotlb.iova = addr & VTD_PAGE_MASK_4K;
        iotlb.translated_addr = addr & VTD_PAGE_MASK_4K;
        iotlb.addr_mask = ~VTD_PAGE_MASK_4K;
        iotlb.perm = IOMMU_RW;
        success = true;
    }

    if (likely(success)) {
        trace_vtd_dmar_translate(pci_bus_num(vtd_as->bus),
                                 VTD_PCI_SLOT(vtd_as->devfn),
                                 VTD_PCI_FUNC(vtd_as->devfn),
                                 iotlb.iova, iotlb.translated_addr,
                                 iotlb.addr_mask);
    } else {
        trace_vtd_err_dmar_translate(pci_bus_num(vtd_as->bus),
                                     VTD_PCI_SLOT(vtd_as->devfn),
                                     VTD_PCI_FUNC(vtd_as->devfn),
                                     iotlb.iova);
    }

    return iotlb;
}

static void vtd_iommu_notify_flag_changed(IOMMUMemoryRegion *iommu,
                                          IOMMUNotifierFlag old,
                                          IOMMUNotifierFlag new)
{
    VTDAddressSpace *vtd_as = container_of(iommu, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    IntelIOMMUNotifierNode *node = NULL;
    IntelIOMMUNotifierNode *next_node = NULL;

    if (!s->caching_mode && new & IOMMU_NOTIFIER_MAP) {
        error_report("We need to set caching-mode=1 for intel-iommu to enable "
                     "device assignment with IOMMU protection.");
        exit(1);
    }

    if (old == IOMMU_NOTIFIER_NONE) {
        node = g_malloc0(sizeof(*node));
        node->vtd_as = vtd_as;
        QLIST_INSERT_HEAD(&s->notifiers_list, node, next);
        return;
    }

    /* update notifier node with new flags */
    QLIST_FOREACH_SAFE(node, &s->notifiers_list, next, next_node) {
        if (node->vtd_as == vtd_as) {
            if (new == IOMMU_NOTIFIER_NONE) {
                QLIST_REMOVE(node, next);
                g_free(node);
            }
            return;
        }
    }
}

static int vtd_post_load(void *opaque, int version_id)
{
    IntelIOMMUState *iommu = opaque;

    /*
     * Memory regions are dynamically turned on/off depending on
     * context entry configurations from the guest. After migration,
     * we need to make sure the memory regions are still correct.
     */
    vtd_switch_address_space_all(iommu);

    return 0;
}

static const VMStateDescription vtd_vmstate = {
    .name = "iommu-intel",
    .version_id = 1,
    .minimum_version_id = 1,
    .priority = MIG_PRI_IOMMU,
    .post_load = vtd_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(root, IntelIOMMUState),
        VMSTATE_UINT64(intr_root, IntelIOMMUState),
        VMSTATE_UINT64(iq, IntelIOMMUState),
        VMSTATE_UINT32(intr_size, IntelIOMMUState),
        VMSTATE_UINT16(iq_head, IntelIOMMUState),
        VMSTATE_UINT16(iq_tail, IntelIOMMUState),
        VMSTATE_UINT16(iq_size, IntelIOMMUState),
        VMSTATE_UINT16(next_frcd_reg, IntelIOMMUState),
        VMSTATE_UINT8_ARRAY(csr, IntelIOMMUState, DMAR_REG_SIZE),
        VMSTATE_UINT8(iq_last_desc_type, IntelIOMMUState),
        VMSTATE_BOOL(root_extended, IntelIOMMUState),
        VMSTATE_BOOL(dmar_enabled, IntelIOMMUState),
        VMSTATE_BOOL(qi_enabled, IntelIOMMUState),
        VMSTATE_BOOL(intr_enabled, IntelIOMMUState),
        VMSTATE_BOOL(intr_eime, IntelIOMMUState),
        VMSTATE_END_OF_LIST()
    }
};

static const MemoryRegionOps vtd_mem_ops = {
    .read = vtd_mem_read,
    .write = vtd_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static Property vtd_properties[] = {
    DEFINE_PROP_UINT32("version", IntelIOMMUState, version, 0),
    DEFINE_PROP_ON_OFF_AUTO("eim", IntelIOMMUState, intr_eim,
                            ON_OFF_AUTO_AUTO),
    DEFINE_PROP_BOOL("x-buggy-eim", IntelIOMMUState, buggy_eim, false),
    DEFINE_PROP_UINT8("x-aw-bits", IntelIOMMUState, aw_bits,
                      VTD_HOST_ADDRESS_WIDTH),
    DEFINE_PROP_BOOL("caching-mode", IntelIOMMUState, caching_mode, FALSE),
    DEFINE_PROP_END_OF_LIST(),
};

/* Read IRTE entry with specific index */
static int vtd_irte_get(IntelIOMMUState *iommu, uint16_t index,
                        VTD_IR_TableEntry *entry, uint16_t sid)
{
    static const uint16_t vtd_svt_mask[VTD_SQ_MAX] = \
        {0xffff, 0xfffb, 0xfff9, 0xfff8};
    dma_addr_t addr = 0x00;
    uint16_t mask, source_id;
    uint8_t bus, bus_max, bus_min;

    addr = iommu->intr_root + index * sizeof(*entry);
    if (dma_memory_read(&address_space_memory, addr, entry,
                        sizeof(*entry))) {
        trace_vtd_err("Memory read failed for IRTE.");
        return -VTD_FR_IR_ROOT_INVAL;
    }

    trace_vtd_ir_irte_get(index, le64_to_cpu(entry->data[1]),
                          le64_to_cpu(entry->data[0]));

    if (!entry->irte.present) {
        trace_vtd_err_irte(index, le64_to_cpu(entry->data[1]),
                           le64_to_cpu(entry->data[0]));
        return -VTD_FR_IR_ENTRY_P;
    }

    if (entry->irte.__reserved_0 || entry->irte.__reserved_1 ||
        entry->irte.__reserved_2) {
        trace_vtd_err_irte(index, le64_to_cpu(entry->data[1]),
                           le64_to_cpu(entry->data[0]));
        return -VTD_FR_IR_IRTE_RSVD;
    }

    if (sid != X86_IOMMU_SID_INVALID) {
        /* Validate IRTE SID */
        source_id = le32_to_cpu(entry->irte.source_id);
        switch (entry->irte.sid_vtype) {
        case VTD_SVT_NONE:
            break;

        case VTD_SVT_ALL:
            mask = vtd_svt_mask[entry->irte.sid_q];
            if ((source_id & mask) != (sid & mask)) {
                trace_vtd_err_irte_sid(index, sid, source_id);
                return -VTD_FR_IR_SID_ERR;
            }
            break;

        case VTD_SVT_BUS:
            bus_max = source_id >> 8;
            bus_min = source_id & 0xff;
            bus = sid >> 8;
            if (bus > bus_max || bus < bus_min) {
                trace_vtd_err_irte_sid_bus(index, bus, bus_min, bus_max);
                return -VTD_FR_IR_SID_ERR;
            }
            break;

        default:
            trace_vtd_err_irte_svt(index, entry->irte.sid_vtype);
            /* Take this as verification failure. */
            return -VTD_FR_IR_SID_ERR;
            break;
        }
    }

    return 0;
}

/* Fetch IRQ information of specific IR index */
static int vtd_remap_irq_get(IntelIOMMUState *iommu, uint16_t index,
                             VTDIrq *irq, uint16_t sid)
{
    VTD_IR_TableEntry irte = {};
    int ret = 0;

    ret = vtd_irte_get(iommu, index, &irte, sid);
    if (ret) {
        return ret;
    }

    irq->trigger_mode = irte.irte.trigger_mode;
    irq->vector = irte.irte.vector;
    irq->delivery_mode = irte.irte.delivery_mode;
    irq->dest = le32_to_cpu(irte.irte.dest_id);
    if (!iommu->intr_eime) {
#define  VTD_IR_APIC_DEST_MASK         (0xff00ULL)
#define  VTD_IR_APIC_DEST_SHIFT        (8)
        irq->dest = (irq->dest & VTD_IR_APIC_DEST_MASK) >>
            VTD_IR_APIC_DEST_SHIFT;
    }
    irq->dest_mode = irte.irte.dest_mode;
    irq->redir_hint = irte.irte.redir_hint;

    trace_vtd_ir_remap(index, irq->trigger_mode, irq->vector,
                       irq->delivery_mode, irq->dest, irq->dest_mode);

    return 0;
}

/* Generate one MSI message from VTDIrq info */
static void vtd_generate_msi_message(VTDIrq *irq, MSIMessage *msg_out)
{
    VTD_MSIMessage msg = {};

    /* Generate address bits */
    msg.dest_mode = irq->dest_mode;
    msg.redir_hint = irq->redir_hint;
    msg.dest = irq->dest;
    msg.__addr_hi = irq->dest & 0xffffff00;
    msg.__addr_head = cpu_to_le32(0xfee);
    /* Keep this from original MSI address bits */
    msg.__not_used = irq->msi_addr_last_bits;

    /* Generate data bits */
    msg.vector = irq->vector;
    msg.delivery_mode = irq->delivery_mode;
    msg.level = 1;
    msg.trigger_mode = irq->trigger_mode;

    msg_out->address = msg.msi_addr;
    msg_out->data = msg.msi_data;
}

/* Interrupt remapping for MSI/MSI-X entry */
static int vtd_interrupt_remap_msi(IntelIOMMUState *iommu,
                                   MSIMessage *origin,
                                   MSIMessage *translated,
                                   uint16_t sid)
{
    int ret = 0;
    VTD_IR_MSIAddress addr;
    uint16_t index;
    VTDIrq irq = {};

    assert(origin && translated);

    trace_vtd_ir_remap_msi_req(origin->address, origin->data);

    if (!iommu || !iommu->intr_enabled) {
        memcpy(translated, origin, sizeof(*origin));
        goto out;
    }

    if (origin->address & VTD_MSI_ADDR_HI_MASK) {
        trace_vtd_err("MSI address high 32 bits non-zero when "
                      "Interrupt Remapping enabled.");
        return -VTD_FR_IR_REQ_RSVD;
    }

    addr.data = origin->address & VTD_MSI_ADDR_LO_MASK;
    if (addr.addr.__head != 0xfee) {
        trace_vtd_err("MSI addr low 32 bit invalid.");
        return -VTD_FR_IR_REQ_RSVD;
    }

    /* This is compatible mode. */
    if (addr.addr.int_mode != VTD_IR_INT_FORMAT_REMAP) {
        memcpy(translated, origin, sizeof(*origin));
        goto out;
    }

    index = addr.addr.index_h << 15 | le16_to_cpu(addr.addr.index_l);

#define  VTD_IR_MSI_DATA_SUBHANDLE       (0x0000ffff)
#define  VTD_IR_MSI_DATA_RESERVED        (0xffff0000)

    if (addr.addr.sub_valid) {
        /* See VT-d spec 5.1.2.2 and 5.1.3 on subhandle */
        index += origin->data & VTD_IR_MSI_DATA_SUBHANDLE;
    }

    ret = vtd_remap_irq_get(iommu, index, &irq, sid);
    if (ret) {
        return ret;
    }

    if (addr.addr.sub_valid) {
        trace_vtd_ir_remap_type("MSI");
        if (origin->data & VTD_IR_MSI_DATA_RESERVED) {
            trace_vtd_err_ir_msi_invalid(sid, origin->address, origin->data);
            return -VTD_FR_IR_REQ_RSVD;
        }
    } else {
        uint8_t vector = origin->data & 0xff;
        uint8_t trigger_mode = (origin->data >> MSI_DATA_TRIGGER_SHIFT) & 0x1;

        trace_vtd_ir_remap_type("IOAPIC");
        /* IOAPIC entry vector should be aligned with IRTE vector
         * (see vt-d spec 5.1.5.1). */
        if (vector != irq.vector) {
            trace_vtd_warn_ir_vector(sid, index, vector, irq.vector);
        }

        /* The Trigger Mode field must match the Trigger Mode in the IRTE.
         * (see vt-d spec 5.1.5.1). */
        if (trigger_mode != irq.trigger_mode) {
            trace_vtd_warn_ir_trigger(sid, index, trigger_mode,
                                      irq.trigger_mode);
        }
    }

    /*
     * We'd better keep the last two bits, assuming that guest OS
     * might modify it. Keep it does not hurt after all.
     */
    irq.msi_addr_last_bits = addr.addr.__not_care;

    /* Translate VTDIrq to MSI message */
    vtd_generate_msi_message(&irq, translated);

out:
    trace_vtd_ir_remap_msi(origin->address, origin->data,
                           translated->address, translated->data);
    return 0;
}

static int vtd_int_remap(X86IOMMUState *iommu, MSIMessage *src,
                         MSIMessage *dst, uint16_t sid)
{
    return vtd_interrupt_remap_msi(INTEL_IOMMU_DEVICE(iommu),
                                   src, dst, sid);
}

static MemTxResult vtd_mem_ir_read(void *opaque, hwaddr addr,
                                   uint64_t *data, unsigned size,
                                   MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static MemTxResult vtd_mem_ir_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size,
                                    MemTxAttrs attrs)
{
    int ret = 0;
    MSIMessage from = {}, to = {};
    uint16_t sid = X86_IOMMU_SID_INVALID;

    from.address = (uint64_t) addr + VTD_INTERRUPT_ADDR_FIRST;
    from.data = (uint32_t) value;

    if (!attrs.unspecified) {
        /* We have explicit Source ID */
        sid = attrs.requester_id;
    }

    ret = vtd_interrupt_remap_msi(opaque, &from, &to, sid);
    if (ret) {
        /* TODO: report error */
        /* Drop this interrupt */
        return MEMTX_ERROR;
    }

    apic_get_class()->send_msi(&to);

    return MEMTX_OK;
}

static const MemoryRegionOps vtd_mem_ir_ops = {
    .read_with_attrs = vtd_mem_ir_read,
    .write_with_attrs = vtd_mem_ir_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

VTDAddressSpace *vtd_find_add_as(IntelIOMMUState *s, PCIBus *bus, int devfn)
{
    uintptr_t key = (uintptr_t)bus;
    VTDBus *vtd_bus = g_hash_table_lookup(s->vtd_as_by_busptr, &key);
    VTDAddressSpace *vtd_dev_as;
    char name[128];

    if (!vtd_bus) {
        uintptr_t *new_key = g_malloc(sizeof(*new_key));
        *new_key = (uintptr_t)bus;
        /* No corresponding free() */
        vtd_bus = g_malloc0(sizeof(VTDBus) + sizeof(VTDAddressSpace *) * \
                            PCI_DEVFN_MAX);
        vtd_bus->bus = bus;
        g_hash_table_insert(s->vtd_as_by_busptr, new_key, vtd_bus);
    }

    vtd_dev_as = vtd_bus->dev_as[devfn];

    if (!vtd_dev_as) {
        snprintf(name, sizeof(name), "intel_iommu_devfn_%d", devfn);
        vtd_bus->dev_as[devfn] = vtd_dev_as = g_malloc0(sizeof(VTDAddressSpace));

        vtd_dev_as->bus = bus;
        vtd_dev_as->devfn = (uint8_t)devfn;
        vtd_dev_as->iommu_state = s;
        vtd_dev_as->context_cache_entry.context_cache_gen = 0;

        /*
         * Memory region relationships looks like (Address range shows
         * only lower 32 bits to make it short in length...):
         *
         * |-----------------+-------------------+----------|
         * | Name            | Address range     | Priority |
         * |-----------------+-------------------+----------+
         * | vtd_root        | 00000000-ffffffff |        0 |
         * |  intel_iommu    | 00000000-ffffffff |        1 |
         * |  vtd_sys_alias  | 00000000-ffffffff |        1 |
         * |  intel_iommu_ir | fee00000-feefffff |       64 |
         * |-----------------+-------------------+----------|
         *
         * We enable/disable DMAR by switching enablement for
         * vtd_sys_alias and intel_iommu regions. IR region is always
         * enabled.
         */
        memory_region_init_iommu(&vtd_dev_as->iommu, sizeof(vtd_dev_as->iommu),
                                 TYPE_INTEL_IOMMU_MEMORY_REGION, OBJECT(s),
                                 "intel_iommu_dmar",
                                 UINT64_MAX);
        memory_region_init_alias(&vtd_dev_as->sys_alias, OBJECT(s),
                                 "vtd_sys_alias", get_system_memory(),
                                 0, memory_region_size(get_system_memory()));
        memory_region_init_io(&vtd_dev_as->iommu_ir, OBJECT(s),
                              &vtd_mem_ir_ops, s, "intel_iommu_ir",
                              VTD_INTERRUPT_ADDR_SIZE);
        memory_region_init(&vtd_dev_as->root, OBJECT(s),
                           "vtd_root", UINT64_MAX);
        memory_region_add_subregion_overlap(&vtd_dev_as->root,
                                            VTD_INTERRUPT_ADDR_FIRST,
                                            &vtd_dev_as->iommu_ir, 64);
        address_space_init(&vtd_dev_as->as, &vtd_dev_as->root, name);
        memory_region_add_subregion_overlap(&vtd_dev_as->root, 0,
                                            &vtd_dev_as->sys_alias, 1);
        memory_region_add_subregion_overlap(&vtd_dev_as->root, 0,
                                            MEMORY_REGION(&vtd_dev_as->iommu),
                                            1);
        vtd_switch_address_space(vtd_dev_as);
    }
    return vtd_dev_as;
}

/* Unmap the whole range in the notifier's scope. */
static void vtd_address_space_unmap(VTDAddressSpace *as, IOMMUNotifier *n)
{
    IOMMUTLBEntry entry;
    hwaddr size;
    hwaddr start = n->start;
    hwaddr end = n->end;
    IntelIOMMUState *s = as->iommu_state;

    /*
     * Note: all the codes in this function has a assumption that IOVA
     * bits are no more than VTD_MGAW bits (which is restricted by
     * VT-d spec), otherwise we need to consider overflow of 64 bits.
     */

    if (end > VTD_ADDRESS_SIZE(s->aw_bits)) {
        /*
         * Don't need to unmap regions that is bigger than the whole
         * VT-d supported address space size
         */
        end = VTD_ADDRESS_SIZE(s->aw_bits);
    }

    assert(start <= end);
    size = end - start;

    if (ctpop64(size) != 1) {
        /*
         * This size cannot format a correct mask. Let's enlarge it to
         * suite the minimum available mask.
         */
        int n = 64 - clz64(size);
        if (n > s->aw_bits) {
            /* should not happen, but in case it happens, limit it */
            n = s->aw_bits;
        }
        size = 1ULL << n;
    }

    entry.target_as = &address_space_memory;
    /* Adjust iova for the size */
    entry.iova = n->start & ~(size - 1);
    /* This field is meaningless for unmap */
    entry.translated_addr = 0;
    entry.perm = IOMMU_NONE;
    entry.addr_mask = size - 1;

    trace_vtd_as_unmap_whole(pci_bus_num(as->bus),
                             VTD_PCI_SLOT(as->devfn),
                             VTD_PCI_FUNC(as->devfn),
                             entry.iova, size);

    memory_region_notify_one(n, &entry);
}

static void vtd_address_space_unmap_all(IntelIOMMUState *s)
{
    IntelIOMMUNotifierNode *node;
    VTDAddressSpace *vtd_as;
    IOMMUNotifier *n;

    QLIST_FOREACH(node, &s->notifiers_list, next) {
        vtd_as = node->vtd_as;
        IOMMU_NOTIFIER_FOREACH(n, &vtd_as->iommu) {
            vtd_address_space_unmap(vtd_as, n);
        }
    }
}

static int vtd_replay_hook(IOMMUTLBEntry *entry, void *private)
{
    memory_region_notify_one((IOMMUNotifier *)private, entry);
    return 0;
}

static void vtd_iommu_replay(IOMMUMemoryRegion *iommu_mr, IOMMUNotifier *n)
{
    VTDAddressSpace *vtd_as = container_of(iommu_mr, VTDAddressSpace, iommu);
    IntelIOMMUState *s = vtd_as->iommu_state;
    uint8_t bus_n = pci_bus_num(vtd_as->bus);
    VTDContextEntry ce;

    /*
     * The replay can be triggered by either a invalidation or a newly
     * created entry. No matter what, we release existing mappings
     * (it means flushing caches for UNMAP-only registers).
     */
    vtd_address_space_unmap(vtd_as, n);

    if (vtd_dev_to_context_entry(s, bus_n, vtd_as->devfn, &ce) == 0) {
        trace_vtd_replay_ce_valid(bus_n, PCI_SLOT(vtd_as->devfn),
                                  PCI_FUNC(vtd_as->devfn),
                                  VTD_CONTEXT_ENTRY_DID(ce.hi),
                                  ce.hi, ce.lo);
        vtd_page_walk(&ce, 0, ~0ULL, vtd_replay_hook, (void *)n, false,
                      s->aw_bits);
    } else {
        trace_vtd_replay_ce_invalid(bus_n, PCI_SLOT(vtd_as->devfn),
                                    PCI_FUNC(vtd_as->devfn));
    }

    return;
}

/* Do the initialization. It will also be called when reset, so pay
 * attention when adding new initialization stuff.
 */
static void vtd_init(IntelIOMMUState *s)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    memset(s->csr, 0, DMAR_REG_SIZE);
    memset(s->wmask, 0, DMAR_REG_SIZE);
    memset(s->w1cmask, 0, DMAR_REG_SIZE);
    memset(s->womask, 0, DMAR_REG_SIZE);

    s->root = 0;
    s->root_extended = false;
    s->dmar_enabled = false;
    s->iq_head = 0;
    s->iq_tail = 0;
    s->iq = 0;
    s->iq_size = 0;
    s->qi_enabled = false;
    s->iq_last_desc_type = VTD_INV_DESC_NONE;
    s->next_frcd_reg = 0;
    s->cap = VTD_CAP_FRO | VTD_CAP_NFR | VTD_CAP_ND |
             VTD_CAP_MAMV | VTD_CAP_PSI | VTD_CAP_SLLPS |
             VTD_CAP_SAGAW_39bit | VTD_CAP_MGAW(s->aw_bits);
    if (s->aw_bits == VTD_HOST_AW_48BIT) {
        s->cap |= VTD_CAP_SAGAW_48bit;
    }
    s->ecap = VTD_ECAP_QI | VTD_ECAP_IRO;

    /*
     * Rsvd field masks for spte
     */
    vtd_paging_entry_rsvd_field[0] = ~0ULL;
    vtd_paging_entry_rsvd_field[1] = VTD_SPTE_PAGE_L1_RSVD_MASK(s->aw_bits);
    vtd_paging_entry_rsvd_field[2] = VTD_SPTE_PAGE_L2_RSVD_MASK(s->aw_bits);
    vtd_paging_entry_rsvd_field[3] = VTD_SPTE_PAGE_L3_RSVD_MASK(s->aw_bits);
    vtd_paging_entry_rsvd_field[4] = VTD_SPTE_PAGE_L4_RSVD_MASK(s->aw_bits);
    vtd_paging_entry_rsvd_field[5] = VTD_SPTE_LPAGE_L1_RSVD_MASK(s->aw_bits);
    vtd_paging_entry_rsvd_field[6] = VTD_SPTE_LPAGE_L2_RSVD_MASK(s->aw_bits);
    vtd_paging_entry_rsvd_field[7] = VTD_SPTE_LPAGE_L3_RSVD_MASK(s->aw_bits);
    vtd_paging_entry_rsvd_field[8] = VTD_SPTE_LPAGE_L4_RSVD_MASK(s->aw_bits);

    if (x86_iommu->intr_supported) {
        s->ecap |= VTD_ECAP_IR | VTD_ECAP_MHMV;
        if (s->intr_eim == ON_OFF_AUTO_ON) {
            s->ecap |= VTD_ECAP_EIM;
        }
        assert(s->intr_eim != ON_OFF_AUTO_AUTO);
    }

    if (x86_iommu->dt_supported) {
        s->ecap |= VTD_ECAP_DT;
    }

    if (x86_iommu->pt_supported) {
        s->ecap |= VTD_ECAP_PT;
    }

    if (s->caching_mode) {
        s->cap |= VTD_CAP_CM;
    }

    vtd_reset_context_cache(s);
    vtd_reset_iotlb(s);

    /* Define registers with default values and bit semantics */
    vtd_define_long(s, DMAR_VER_REG, 0x10UL, 0, 0);
    vtd_define_quad(s, DMAR_CAP_REG, s->cap, 0, 0);
    vtd_define_quad(s, DMAR_ECAP_REG, s->ecap, 0, 0);
    vtd_define_long(s, DMAR_GCMD_REG, 0, 0xff800000UL, 0);
    vtd_define_long_wo(s, DMAR_GCMD_REG, 0xff800000UL);
    vtd_define_long(s, DMAR_GSTS_REG, 0, 0, 0);
    vtd_define_quad(s, DMAR_RTADDR_REG, 0, 0xfffffffffffff000ULL, 0);
    vtd_define_quad(s, DMAR_CCMD_REG, 0, 0xe0000003ffffffffULL, 0);
    vtd_define_quad_wo(s, DMAR_CCMD_REG, 0x3ffff0000ULL);

    /* Advanced Fault Logging not supported */
    vtd_define_long(s, DMAR_FSTS_REG, 0, 0, 0x11UL);
    vtd_define_long(s, DMAR_FECTL_REG, 0x80000000UL, 0x80000000UL, 0);
    vtd_define_long(s, DMAR_FEDATA_REG, 0, 0x0000ffffUL, 0);
    vtd_define_long(s, DMAR_FEADDR_REG, 0, 0xfffffffcUL, 0);

    /* Treated as RsvdZ when EIM in ECAP_REG is not supported
     * vtd_define_long(s, DMAR_FEUADDR_REG, 0, 0xffffffffUL, 0);
     */
    vtd_define_long(s, DMAR_FEUADDR_REG, 0, 0, 0);

    /* Treated as RO for implementations that PLMR and PHMR fields reported
     * as Clear in the CAP_REG.
     * vtd_define_long(s, DMAR_PMEN_REG, 0, 0x80000000UL, 0);
     */
    vtd_define_long(s, DMAR_PMEN_REG, 0, 0, 0);

    vtd_define_quad(s, DMAR_IQH_REG, 0, 0, 0);
    vtd_define_quad(s, DMAR_IQT_REG, 0, 0x7fff0ULL, 0);
    vtd_define_quad(s, DMAR_IQA_REG, 0, 0xfffffffffffff007ULL, 0);
    vtd_define_long(s, DMAR_ICS_REG, 0, 0, 0x1UL);
    vtd_define_long(s, DMAR_IECTL_REG, 0x80000000UL, 0x80000000UL, 0);
    vtd_define_long(s, DMAR_IEDATA_REG, 0, 0xffffffffUL, 0);
    vtd_define_long(s, DMAR_IEADDR_REG, 0, 0xfffffffcUL, 0);
    /* Treadted as RsvdZ when EIM in ECAP_REG is not supported */
    vtd_define_long(s, DMAR_IEUADDR_REG, 0, 0, 0);

    /* IOTLB registers */
    vtd_define_quad(s, DMAR_IOTLB_REG, 0, 0Xb003ffff00000000ULL, 0);
    vtd_define_quad(s, DMAR_IVA_REG, 0, 0xfffffffffffff07fULL, 0);
    vtd_define_quad_wo(s, DMAR_IVA_REG, 0xfffffffffffff07fULL);

    /* Fault Recording Registers, 128-bit */
    vtd_define_quad(s, DMAR_FRCD_REG_0_0, 0, 0, 0);
    vtd_define_quad(s, DMAR_FRCD_REG_0_2, 0, 0, 0x8000000000000000ULL);

    /*
     * Interrupt remapping registers.
     */
    vtd_define_quad(s, DMAR_IRTA_REG, 0, 0xfffffffffffff80fULL, 0);
}

/* Should not reset address_spaces when reset because devices will still use
 * the address space they got at first (won't ask the bus again).
 */
static void vtd_reset(DeviceState *dev)
{
    IntelIOMMUState *s = INTEL_IOMMU_DEVICE(dev);

    vtd_init(s);

    /*
     * When device reset, throw away all mappings and external caches
     */
    vtd_address_space_unmap_all(s);
}

static AddressSpace *vtd_host_dma_iommu(PCIBus *bus, void *opaque, int devfn)
{
    IntelIOMMUState *s = opaque;
    VTDAddressSpace *vtd_as;

    assert(0 <= devfn && devfn < PCI_DEVFN_MAX);

    vtd_as = vtd_find_add_as(s, bus, devfn);
    return &vtd_as->as;
}

static bool vtd_decide_config(IntelIOMMUState *s, Error **errp)
{
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(s);

    if (s->intr_eim == ON_OFF_AUTO_ON && !x86_iommu->intr_supported) {
        error_setg(errp, "eim=on cannot be selected without intremap=on");
        return false;
    }

    if (s->intr_eim == ON_OFF_AUTO_AUTO) {
        s->intr_eim = s->buggy_eim && x86_iommu->intr_supported ?
                                      ON_OFF_AUTO_ON : ON_OFF_AUTO_OFF;
    }
    if (s->intr_eim == ON_OFF_AUTO_ON && !s->buggy_eim) {
        if (!kvm_enable_x2apic()) {
            error_setg(errp, "eim=on requires support on the KVM side"
                             "(X2APIC_API, first shipped in v4.7)");
            return false;
        }
    }

    /* Currently only address widths supported are 39 and 48 bits */
    if ((s->aw_bits != VTD_HOST_AW_39BIT) &&
        (s->aw_bits != VTD_HOST_AW_48BIT)) {
        error_setg(errp, "Supported values for x-aw-bits are: %d, %d",
                   VTD_HOST_AW_39BIT, VTD_HOST_AW_48BIT);
        return false;
    }

    return true;
}

static void vtd_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    PCMachineState *pcms = PC_MACHINE(ms);
    PCIBus *bus = pcms->bus;
    IntelIOMMUState *s = INTEL_IOMMU_DEVICE(dev);
    X86IOMMUState *x86_iommu = X86_IOMMU_DEVICE(dev);

    x86_iommu->type = TYPE_INTEL;

    if (!vtd_decide_config(s, errp)) {
        return;
    }

    QLIST_INIT(&s->notifiers_list);
    memset(s->vtd_as_by_bus_num, 0, sizeof(s->vtd_as_by_bus_num));
    memory_region_init_io(&s->csrmem, OBJECT(s), &vtd_mem_ops, s,
                          "intel_iommu", DMAR_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->csrmem);
    /* No corresponding destroy */
    s->iotlb = g_hash_table_new_full(vtd_uint64_hash, vtd_uint64_equal,
                                     g_free, g_free);
    s->vtd_as_by_busptr = g_hash_table_new_full(vtd_uint64_hash, vtd_uint64_equal,
                                              g_free, g_free);
    vtd_init(s);
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, Q35_HOST_BRIDGE_IOMMU_ADDR);
    pci_setup_iommu(bus, vtd_host_dma_iommu, dev);
    /* Pseudo address space under root PCI bus. */
    pcms->ioapic_as = vtd_host_dma_iommu(bus, s, Q35_PSEUDO_DEVFN_IOAPIC);
}

static void vtd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    X86IOMMUClass *x86_class = X86_IOMMU_CLASS(klass);

    dc->reset = vtd_reset;
    dc->vmsd = &vtd_vmstate;
    dc->props = vtd_properties;
    dc->hotpluggable = false;
    x86_class->realize = vtd_realize;
    x86_class->int_remap = vtd_int_remap;
    /* Supported by the pc-q35-* machine types */
    dc->user_creatable = true;
}

static const TypeInfo vtd_info = {
    .name          = TYPE_INTEL_IOMMU_DEVICE,
    .parent        = TYPE_X86_IOMMU_DEVICE,
    .instance_size = sizeof(IntelIOMMUState),
    .class_init    = vtd_class_init,
};

static void vtd_iommu_memory_region_class_init(ObjectClass *klass,
                                                     void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = vtd_iommu_translate;
    imrc->notify_flag_changed = vtd_iommu_notify_flag_changed;
    imrc->replay = vtd_iommu_replay;
}

static const TypeInfo vtd_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_INTEL_IOMMU_MEMORY_REGION,
    .class_init = vtd_iommu_memory_region_class_init,
};

static void vtd_register_types(void)
{
    type_register_static(&vtd_info);
    type_register_static(&vtd_iommu_memory_region_info);
}

type_init(vtd_register_types)

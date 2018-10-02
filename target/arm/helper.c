#include "qemu/osdep.h"
#include "target/arm/idau.h"
#include "trace.h"
#include "cpu.h"
#include "internals.h"
#include "exec/gdbstub.h"
#include "exec/helper-proto.h"
#include "qemu/host-utils.h"
#include "sysemu/arch_init.h"
#include "sysemu/sysemu.h"
#include "qemu/bitops.h"
#include "qemu/crc32c.h"
#include "exec/exec-all.h"
#include <zlib.h> /* For crc32 */
#include "exec/semihost.h"
#include "sysemu/kvm.h"
#include "fpu/softfloat.h"
#include "qemu/range.h"

#ifndef CONFIG_USER_ONLY
static bool get_phys_addr_lpae(CPUARMState *env, target_ulong address,
                               MMUAccessType access_type, ARMMMUIdx mmu_idx,
                               hwaddr *phys_ptr, MemTxAttrs *txattrs, int *prot,
                               target_ulong *page_size_ptr,
                               ARMMMUFaultInfo *fi, ARMCacheAttrs *cacheattrs);
#endif

uint64_t raw_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    assert(ri->fieldoffset);
    if (cpreg_field_is_64bit(ri)) {
        return CPREG_FIELD64(env, ri);
    } else {
        return CPREG_FIELD32(env, ri);
    }
}

void raw_write(CPUARMState *env, const ARMCPRegInfo *ri,
               uint64_t value)
{
    assert(ri->fieldoffset);
    if (cpreg_field_is_64bit(ri)) {
        CPREG_FIELD64(env, ri) = value;
    } else {
        CPREG_FIELD32(env, ri) = value;
    }
}

uint64_t read_raw_cp_reg(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /* Raw read of a coprocessor register (as needed for migration, etc). */
    if (ri->type & ARM_CP_CONST) {
        return ri->resetvalue;
    } else if (ri->raw_readfn) {
        return ri->raw_readfn(env, ri);
    } else if (ri->readfn) {
        return ri->readfn(env, ri);
    } else {
        return raw_read(env, ri);
    }
}

static void write_raw_cp_reg(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t v)
{
    /* Raw write of a coprocessor register (as needed for migration, etc).
     * Note that constant registers are treated as write-ignored; the
     * caller should check for success by whether a readback gives the
     * value written.
     */
    if (ri->type & ARM_CP_CONST) {
        return;
    } else if (ri->raw_writefn) {
        ri->raw_writefn(env, ri, v);
    } else if (ri->writefn) {
        ri->writefn(env, ri, v);
    } else {
        raw_write(env, ri, v);
    }
}

static bool raw_accessors_invalid(const ARMCPRegInfo *ri)
{
   /* Return true if the regdef would cause an assertion if you called
    * read_raw_cp_reg() or write_raw_cp_reg() on it (ie if it is a
    * program bug for it not to have the NO_RAW flag).
    * NB that returning false here doesn't necessarily mean that calling
    * read/write_raw_cp_reg() is safe, because we can't distinguish "has
    * read/write access functions which are safe for raw use" from "has
    * read/write access functions which have side effects but has forgotten
    * to provide raw access functions".
    * The tests here line up with the conditions in read/write_raw_cp_reg()
    * and assertions in raw_read()/raw_write().
    */
    if ((ri->type & ARM_CP_CONST) ||
        ri->fieldoffset ||
        ((ri->raw_writefn || ri->writefn) && (ri->raw_readfn || ri->readfn))) {
        return false;
    }
    return true;
}

bool write_cpustate_to_list(ARMCPU *cpu)
{
    /* Write the coprocessor state from cpu->env to the (index,value) list. */
    int i;
    bool ok = true;

    for (i = 0; i < cpu->cpreg_array_len; i++) {
        uint32_t regidx = kvm_to_cpreg_id(cpu->cpreg_indexes[i]);
        const ARMCPRegInfo *ri;

        ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);
        if (!ri) {
            ok = false;
            continue;
        }
        if (ri->type & ARM_CP_NO_RAW) {
            continue;
        }
        cpu->cpreg_values[i] = read_raw_cp_reg(&cpu->env, ri);
    }
    return ok;
}

bool write_list_to_cpustate(ARMCPU *cpu)
{
    int i;
    bool ok = true;

    for (i = 0; i < cpu->cpreg_array_len; i++) {
        uint32_t regidx = kvm_to_cpreg_id(cpu->cpreg_indexes[i]);
        uint64_t v = cpu->cpreg_values[i];
        const ARMCPRegInfo *ri;

        ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);
        if (!ri) {
            ok = false;
            continue;
        }
        if (ri->type & ARM_CP_NO_RAW) {
            continue;
        }
        /* Write value and confirm it reads back as written
         * (to catch read-only registers and partially read-only
         * registers where the incoming migration value doesn't match)
         */
        write_raw_cp_reg(&cpu->env, ri, v);
        if (read_raw_cp_reg(&cpu->env, ri) != v) {
            ok = false;
        }
    }
    return ok;
}

static void add_cpreg_to_list(gpointer key, gpointer opaque)
{
    ARMCPU *cpu = opaque;
    uint64_t regidx;
    const ARMCPRegInfo *ri;

    regidx = *(uint32_t *)key;
    ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);

    if (!(ri->type & (ARM_CP_NO_RAW|ARM_CP_ALIAS))) {
        cpu->cpreg_indexes[cpu->cpreg_array_len] = cpreg_to_kvm_id(regidx);
        /* The value array need not be initialized at this point */
        cpu->cpreg_array_len++;
    }
}

static void count_cpreg(gpointer key, gpointer opaque)
{
    ARMCPU *cpu = opaque;
    uint64_t regidx;
    const ARMCPRegInfo *ri;

    regidx = *(uint32_t *)key;
    ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);

    if (!(ri->type & (ARM_CP_NO_RAW|ARM_CP_ALIAS))) {
        cpu->cpreg_array_len++;
    }
}

static gint cpreg_key_compare(gconstpointer a, gconstpointer b)
{
    uint64_t aidx = cpreg_to_kvm_id(*(uint32_t *)a);
    uint64_t bidx = cpreg_to_kvm_id(*(uint32_t *)b);

    if (aidx > bidx) {
        return 1;
    }
    if (aidx < bidx) {
        return -1;
    }
    return 0;
}

void init_cpreg_list(ARMCPU *cpu)
{
    /* Initialise the cpreg_tuples[] array based on the cp_regs hash.
     * Note that we require cpreg_tuples[] to be sorted by key ID.
     */
    GList *keys;
    int arraylen;

    keys = g_hash_table_get_keys(cpu->cp_regs);
    keys = g_list_sort(keys, cpreg_key_compare);

    cpu->cpreg_array_len = 0;

    g_list_foreach(keys, count_cpreg, cpu);

    arraylen = cpu->cpreg_array_len;
    cpu->cpreg_indexes = g_new(uint64_t, arraylen);
    cpu->cpreg_values = g_new(uint64_t, arraylen);
    cpu->cpreg_vmstate_indexes = g_new(uint64_t, arraylen);
    cpu->cpreg_vmstate_values = g_new(uint64_t, arraylen);
    cpu->cpreg_vmstate_array_len = cpu->cpreg_array_len;
    cpu->cpreg_array_len = 0;

    g_list_foreach(keys, add_cpreg_to_list, cpu);

    assert(cpu->cpreg_array_len == arraylen);

    g_list_free(keys);
}

#ifndef CONFIG_USER_ONLY

uint64_t gt_get_countervalue(CPUARMState *env)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / GTIMER_SCALE;
}

void gt_recalc_timer(ARMCPU *cpu, int timeridx)
{
    ARMGenericTimer *gt = &cpu->env.cp15.c14_timer[timeridx];

    if (gt->ctl & 1) {
        /* Timer enabled: calculate and set current ISTATUS, irq, and
         * reset timer to when ISTATUS next has to change
         */
        uint64_t offset = timeridx == GTIMER_VIRT ?
                                      cpu->env.cp15.cntvoff_el2 : 0;
        uint64_t count = gt_get_countervalue(&cpu->env);
        /* Note that this must be unsigned 64 bit arithmetic: */
        int istatus = count - offset >= gt->cval;
        uint64_t nexttick;
        int irqstate;

        gt->ctl = deposit32(gt->ctl, 2, 1, istatus);

        irqstate = (istatus && !(gt->ctl & 2));
        qemu_set_irq(cpu->gt_timer_outputs[timeridx], irqstate);

        if (istatus) {
            /* Next transition is when count rolls back over to zero */
            nexttick = UINT64_MAX;
        } else {
            /* Next transition is when we hit cval */
            nexttick = gt->cval + offset;
        }
        /* Note that the desired next expiry time might be beyond the
         * signed-64-bit range of a QEMUTimer -- in this case we just
         * set the timer for as far in the future as possible. When the
         * timer expires we will reset the timer for any remaining period.
         */
        if (nexttick > INT64_MAX / GTIMER_SCALE) {
            nexttick = INT64_MAX / GTIMER_SCALE;
        }
        timer_mod(cpu->gt_timer[timeridx], nexttick);
        trace_arm_gt_recalc(timeridx, irqstate, nexttick);
    } else {
        /* Timer disabled: ISTATUS and timer output always clear */
        gt->ctl &= ~4;
        qemu_set_irq(cpu->gt_timer_outputs[timeridx], 0);
        timer_del(cpu->gt_timer[timeridx]);
        trace_arm_gt_recalc_disabled(timeridx);
    }
}

void arm_gt_ptimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_PHYS);
}

void arm_gt_vtimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_VIRT);
}

void arm_gt_htimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_HYP);
}

void arm_gt_stimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_SEC);
}

#endif

void hw_watchpoint_update(ARMCPU *cpu, int n)
{
    CPUARMState *env = &cpu->env;
    vaddr len = 0;
    vaddr wvr = env->cp15.dbgwvr[n];
    uint64_t wcr = env->cp15.dbgwcr[n];
    int mask;
    int flags = BP_CPU | BP_STOP_BEFORE_ACCESS;

    if (env->cpu_watchpoint[n]) {
        cpu_watchpoint_remove_by_ref(CPU(cpu), env->cpu_watchpoint[n]);
        env->cpu_watchpoint[n] = NULL;
    }

    if (!extract64(wcr, 0, 1)) {
        /* E bit clear : watchpoint disabled */
        return;
    }

    switch (extract64(wcr, 3, 2)) {
    case 0:
        /* LSC 00 is reserved and must behave as if the wp is disabled */
        return;
    case 1:
        flags |= BP_MEM_READ;
        break;
    case 2:
        flags |= BP_MEM_WRITE;
        break;
    case 3:
        flags |= BP_MEM_ACCESS;
        break;
    }

    /* Attempts to use both MASK and BAS fields simultaneously are
     * CONSTRAINED UNPREDICTABLE; we opt to ignore BAS in this case,
     * thus generating a watchpoint for every byte in the masked region.
     */
    mask = extract64(wcr, 24, 4);
    if (mask == 1 || mask == 2) {
        /* Reserved values of MASK; we must act as if the mask value was
         * some non-reserved value, or as if the watchpoint were disabled.
         * We choose the latter.
         */
        return;
    } else if (mask) {
        /* Watchpoint covers an aligned area up to 2GB in size */
        len = 1ULL << mask;
        /* If masked bits in WVR are not zero it's CONSTRAINED UNPREDICTABLE
         * whether the watchpoint fires when the unmasked bits match; we opt
         * to generate the exceptions.
         */
        wvr &= ~(len - 1);
    } else {
        /* Watchpoint covers bytes defined by the byte address select bits */
        int bas = extract64(wcr, 5, 8);
        int basstart;

        if (bas == 0) {
            /* This must act as if the watchpoint is disabled */
            return;
        }

        if (extract64(wvr, 2, 1)) {
            /* Deprecated case of an only 4-aligned address. BAS[7:4] are
             * ignored, and BAS[3:0] define which bytes to watch.
             */
            bas &= 0xf;
        }
        /* The BAS bits are supposed to be programmed to indicate a contiguous
         * range of bytes. Otherwise it is CONSTRAINED UNPREDICTABLE whether
         * we fire for each byte in the word/doubleword addressed by the WVR.
         * We choose to ignore any non-zero bits after the first range of 1s.
         */
        basstart = ctz32(bas);
        len = cto32(bas >> basstart);
        wvr += basstart;
    }

    cpu_watchpoint_insert(CPU(cpu), wvr, len, flags,
                          &env->cpu_watchpoint[n]);
}

void hw_watchpoint_update_all(ARMCPU *cpu)
{
    int i;
    CPUARMState *env = &cpu->env;

    /* Completely clear out existing QEMU watchpoints and our array, to
     * avoid possible stale entries following migration load.
     */
    cpu_watchpoint_remove_all(CPU(cpu), BP_CPU);
    memset(env->cpu_watchpoint, 0, sizeof(env->cpu_watchpoint));

    for (i = 0; i < ARRAY_SIZE(cpu->env.cpu_watchpoint); i++) {
        hw_watchpoint_update(cpu, i);
    }
}

void hw_breakpoint_update(ARMCPU *cpu, int n)
{
    CPUARMState *env = &cpu->env;
    uint64_t bvr = env->cp15.dbgbvr[n];
    uint64_t bcr = env->cp15.dbgbcr[n];
    vaddr addr;
    int bt;
    int flags = BP_CPU;

    if (env->cpu_breakpoint[n]) {
        cpu_breakpoint_remove_by_ref(CPU(cpu), env->cpu_breakpoint[n]);
        env->cpu_breakpoint[n] = NULL;
    }

    if (!extract64(bcr, 0, 1)) {
        /* E bit clear : watchpoint disabled */
        return;
    }

    bt = extract64(bcr, 20, 4);

    switch (bt) {
    case 4: /* unlinked address mismatch (reserved if AArch64) */
    case 5: /* linked address mismatch (reserved if AArch64) */
        qemu_log_mask(LOG_UNIMP,
                      "arm: address mismatch breakpoint types not implemented\n");
        return;
    case 0: /* unlinked address match */
    case 1: /* linked address match */
    {
        /* Bits [63:49] are hardwired to the value of bit [48]; that is,
         * we behave as if the register was sign extended. Bits [1:0] are
         * RES0. The BAS field is used to allow setting breakpoints on 16
         * bit wide instructions; it is CONSTRAINED UNPREDICTABLE whether
         * a bp will fire if the addresses covered by the bp and the addresses
         * covered by the insn overlap but the insn doesn't start at the
         * start of the bp address range. We choose to require the insn and
         * the bp to have the same address. The constraints on writing to
         * BAS enforced in dbgbcr_write mean we have only four cases:
         *  0b0000  => no breakpoint
         *  0b0011  => breakpoint on addr
         *  0b1100  => breakpoint on addr + 2
         *  0b1111  => breakpoint on addr
         * See also figure D2-3 in the v8 ARM ARM (DDI0487A.c).
         */
        int bas = extract64(bcr, 5, 4);
        addr = sextract64(bvr, 0, 49) & ~3ULL;
        if (bas == 0) {
            return;
        }
        if (bas == 0xc) {
            addr += 2;
        }
        break;
    }
    case 2: /* unlinked context ID match */
    case 8: /* unlinked VMID match (reserved if no EL2) */
    case 10: /* unlinked context ID and VMID match (reserved if no EL2) */
        qemu_log_mask(LOG_UNIMP,
                      "arm: unlinked context breakpoint types not implemented\n");
        return;
    case 9: /* linked VMID match (reserved if no EL2) */
    case 11: /* linked context ID and VMID match (reserved if no EL2) */
    case 3: /* linked context ID match */
    default:
        /* We must generate no events for Linked context matches (unless
         * they are linked to by some other bp/wp, which is handled in
         * updates for the linking bp/wp). We choose to also generate no events
         * for reserved values.
         */
        return;
    }

    cpu_breakpoint_insert(CPU(cpu), addr, flags, &env->cpu_breakpoint[n]);
}

void hw_breakpoint_update_all(ARMCPU *cpu)
{
    int i;
    CPUARMState *env = &cpu->env;

    /* Completely clear out existing QEMU breakpoints and our array, to
     * avoid possible stale entries following migration load.
     */
    cpu_breakpoint_remove_all(CPU(cpu), BP_CPU);
    memset(env->cpu_breakpoint, 0, sizeof(env->cpu_breakpoint));

    for (i = 0; i < ARRAY_SIZE(cpu->env.cpu_breakpoint); i++) {
        hw_breakpoint_update(cpu, i);
    }
}

/* Sort alphabetically by type name, except for "any". */
static gint arm_cpu_list_compare(gconstpointer a, gconstpointer b)
{
    ObjectClass *class_a = (ObjectClass *)a;
    ObjectClass *class_b = (ObjectClass *)b;
    const char *name_a, *name_b;

    name_a = object_class_get_name(class_a);
    name_b = object_class_get_name(class_b);
    if (strcmp(name_a, "any-" TYPE_ARM_CPU) == 0) {
        return 1;
    } else if (strcmp(name_b, "any-" TYPE_ARM_CPU) == 0) {
        return -1;
    } else {
        return strcmp(name_a, name_b);
    }
}

static void arm_cpu_list_entry(gpointer data, gpointer user_data)
{
    ObjectClass *oc = data;
    CPUListState *s = user_data;
    const char *typename;
    char *name;

    typename = object_class_get_name(oc);
    name = g_strndup(typename, strlen(typename) - strlen("-" TYPE_ARM_CPU));
    (*s->cpu_fprintf)(s->file, "  %s\n",
                      name);
    g_free(name);
}

void arm_cpu_list(FILE *f, fprintf_function cpu_fprintf)
{
    CPUListState s = {
        .file = f,
        .cpu_fprintf = cpu_fprintf,
    };
    GSList *list;

    list = object_class_get_list(TYPE_ARM_CPU, false);
    list = g_slist_sort(list, arm_cpu_list_compare);
    (*cpu_fprintf)(f, "Available CPUs:\n");
    g_slist_foreach(list, arm_cpu_list_entry, &s);
    g_slist_free(list);
}


static void add_cpreg_to_hashtable(ARMCPU *cpu, const ARMCPRegInfo *r,
                                   void *opaque, int state, int secstate,
                                   int crm, int opc1, int opc2,
                                   const char *name)
{
    /* Private utility function for define_one_arm_cp_reg_with_opaque():
     * add a single reginfo struct to the hash table.
     */
    uint32_t *key = g_new(uint32_t, 1);
    ARMCPRegInfo *r2 = g_memdup(r, sizeof(ARMCPRegInfo));
    int is64 = (r->type & ARM_CP_64BIT) ? 1 : 0;
    int ns = (secstate & ARM_CP_SECSTATE_NS) ? 1 : 0;

    r2->name = g_strdup(name);
    /* Reset the secure state to the specific incoming state.  This is
     * necessary as the register may have been defined with both states.
     */
    r2->secure = secstate;

    if (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1]) {
        /* Register is banked (using both entries in array).
         * Overwriting fieldoffset as the array is only used to define
         * banked registers but later only fieldoffset is used.
         */
        r2->fieldoffset = r->bank_fieldoffsets[ns];
    }

    if (state == ARM_CP_STATE_AA32) {
        if (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1]) {
            /* If the register is banked then we don't need to migrate or
             * reset the 32-bit instance in certain cases:
             *
             * 1) If the register has both 32-bit and 64-bit instances then we
             *    can count on the 64-bit instance taking care of the
             *    non-secure bank.
             * 2) If ARMv8 is enabled then we can count on a 64-bit version
             *    taking care of the secure bank.  This requires that separate
             *    32 and 64-bit definitions are provided.
             */
            if ((r->state == ARM_CP_STATE_BOTH && ns) ||
                (arm_feature(&cpu->env, ARM_FEATURE_V8) && !ns)) {
                r2->type |= ARM_CP_ALIAS;
            }
        } else if ((secstate != r->secure) && !ns) {
            /* The register is not banked so we only want to allow migration of
             * the non-secure instance.
             */
            r2->type |= ARM_CP_ALIAS;
        }

        if (r->state == ARM_CP_STATE_BOTH) {
            /* We assume it is a cp15 register if the .cp field is left unset.
             */
            if (r2->cp == 0) {
                r2->cp = 15;
            }

#ifdef HOST_WORDS_BIGENDIAN
            if (r2->fieldoffset) {
                r2->fieldoffset += sizeof(uint32_t);
            }
#endif
        }
    }
    if (state == ARM_CP_STATE_AA64) {
        /* To allow abbreviation of ARMCPRegInfo
         * definitions, we treat cp == 0 as equivalent to
         * the value for "standard guest-visible sysreg".
         * STATE_BOTH definitions are also always "standard
         * sysreg" in their AArch64 view (the .cp value may
         * be non-zero for the benefit of the AArch32 view).
         */
        if (r->cp == 0 || r->state == ARM_CP_STATE_BOTH) {
            r2->cp = CP_REG_ARM64_SYSREG_CP;
        }
        *key = ENCODE_AA64_CP_REG(r2->cp, r2->crn, crm,
                                  r2->opc0, opc1, opc2);
    } else {
        *key = ENCODE_CP_REG(r2->cp, is64, ns, r2->crn, crm, opc1, opc2);
    }
    if (opaque) {
        r2->opaque = opaque;
    }
    /* reginfo passed to helpers is correct for the actual access,
     * and is never ARM_CP_STATE_BOTH:
     */
    r2->state = state;
    /* Make sure reginfo passed to helpers for wildcarded regs
     * has the correct crm/opc1/opc2 for this reg, not CP_ANY:
     */
    r2->crm = crm;
    r2->opc1 = opc1;
    r2->opc2 = opc2;
    /* By convention, for wildcarded registers only the first
     * entry is used for migration; the others are marked as
     * ALIAS so we don't try to transfer the register
     * multiple times. Special registers (ie NOP/WFI) are
     * never migratable and not even raw-accessible.
     */
    if ((r->type & ARM_CP_SPECIAL)) {
        r2->type |= ARM_CP_NO_RAW;
    }
    if (((r->crm == CP_ANY) && crm != 0) ||
        ((r->opc1 == CP_ANY) && opc1 != 0) ||
        ((r->opc2 == CP_ANY) && opc2 != 0)) {
        r2->type |= ARM_CP_ALIAS | ARM_CP_NO_GDB;
    }

    /* Check that raw accesses are either forbidden or handled. Note that
     * we can't assert this earlier because the setup of fieldoffset for
     * banked registers has to be done first.
     */
    if (!(r2->type & ARM_CP_NO_RAW)) {
        assert(!raw_accessors_invalid(r2));
    }

    /* Overriding of an existing definition must be explicitly
     * requested.
     */
    if (!(r->type & ARM_CP_OVERRIDE)) {
        ARMCPRegInfo *oldreg;
        oldreg = g_hash_table_lookup(cpu->cp_regs, key);
        if (oldreg && !(oldreg->type & ARM_CP_OVERRIDE)) {
            fprintf(stderr, "Register redefined: cp=%d %d bit "
                    "crn=%d crm=%d opc1=%d opc2=%d, "
                    "was %s, now %s\n", r2->cp, 32 + 32 * is64,
                    r2->crn, r2->crm, r2->opc1, r2->opc2,
                    oldreg->name, r2->name);
            g_assert_not_reached();
        }
    }
    g_hash_table_insert(cpu->cp_regs, key, r2);
}


void define_one_arm_cp_reg_with_opaque(ARMCPU *cpu,
                                       const ARMCPRegInfo *r, void *opaque)
{
    /* Define implementations of coprocessor registers.
     * We store these in a hashtable because typically
     * there are less than 150 registers in a space which
     * is 16*16*16*8*8 = 262144 in size.
     * Wildcarding is supported for the crm, opc1 and opc2 fields.
     * If a register is defined twice then the second definition is
     * used, so this can be used to define some generic registers and
     * then override them with implementation specific variations.
     * At least one of the original and the second definition should
     * include ARM_CP_OVERRIDE in its type bits -- this is just a guard
     * against accidental use.
     *
     * The state field defines whether the register is to be
     * visible in the AArch32 or AArch64 execution state. If the
     * state is set to ARM_CP_STATE_BOTH then we synthesise a
     * reginfo structure for the AArch32 view, which sees the lower
     * 32 bits of the 64 bit register.
     *
     * Only registers visible in AArch64 may set r->opc0; opc0 cannot
     * be wildcarded. AArch64 registers are always considered to be 64
     * bits; the ARM_CP_64BIT* flag applies only to the AArch32 view of
     * the register, if any.
     */
    int crm, opc1, opc2, state;
    int crmmin = (r->crm == CP_ANY) ? 0 : r->crm;
    int crmmax = (r->crm == CP_ANY) ? 15 : r->crm;
    int opc1min = (r->opc1 == CP_ANY) ? 0 : r->opc1;
    int opc1max = (r->opc1 == CP_ANY) ? 7 : r->opc1;
    int opc2min = (r->opc2 == CP_ANY) ? 0 : r->opc2;
    int opc2max = (r->opc2 == CP_ANY) ? 7 : r->opc2;
    /* 64 bit registers have only CRm and Opc1 fields */
    assert(!((r->type & ARM_CP_64BIT) && (r->opc2 || r->crn)));
    /* op0 only exists in the AArch64 encodings */
    assert((r->state != ARM_CP_STATE_AA32) || (r->opc0 == 0));
    /* AArch64 regs are all 64 bit so ARM_CP_64BIT is meaningless */
    assert((r->state != ARM_CP_STATE_AA64) || !(r->type & ARM_CP_64BIT));
    /* The AArch64 pseudocode CheckSystemAccess() specifies that op1
     * encodes a minimum access level for the register. We roll this
     * runtime check into our general permission check code, so check
     * here that the reginfo's specified permissions are strict enough
     * to encompass the generic architectural permission check.
     */
    if (r->state != ARM_CP_STATE_AA32) {
        int mask = 0;
        switch (r->opc1) {
        case 0: case 1: case 2:
            /* min_EL EL1 */
            mask = PL1_RW;
            break;
        case 3:
            /* min_EL EL0 */
            mask = PL0_RW;
            break;
        case 4:
            /* min_EL EL2 */
            mask = PL2_RW;
            break;
        case 5:
            /* unallocated encoding, so not possible */
            assert(false);
            break;
        case 6:
            /* min_EL EL3 */
            mask = PL3_RW;
            break;
        case 7:
            /* min_EL EL1, secure mode only (we don't check the latter) */
            mask = PL1_RW;
            break;
        default:
            /* broken reginfo with out-of-range opc1 */
            assert(false);
            break;
        }
        /* assert our permissions are not too lax (stricter is fine) */
        assert((r->access & ~mask) == 0);
    }

    /* Check that the register definition has enough info to handle
     * reads and writes if they are permitted.
     */
    if (!(r->type & (ARM_CP_SPECIAL|ARM_CP_CONST))) {
        if (r->access & PL3_R) {
            assert((r->fieldoffset ||
                   (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1])) ||
                   r->readfn);
        }
        if (r->access & PL3_W) {
            assert((r->fieldoffset ||
                   (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1])) ||
                   r->writefn);
        }
    }
    /* Bad type field probably means missing sentinel at end of reg list */
    assert(cptype_valid(r->type));
    for (crm = crmmin; crm <= crmmax; crm++) {
        for (opc1 = opc1min; opc1 <= opc1max; opc1++) {
            for (opc2 = opc2min; opc2 <= opc2max; opc2++) {
                for (state = ARM_CP_STATE_AA32;
                     state <= ARM_CP_STATE_AA64; state++) {
                    if (r->state != state && r->state != ARM_CP_STATE_BOTH) {
                        continue;
                    }
                    if (state == ARM_CP_STATE_AA32) {
                        /* Under AArch32 CP registers can be common
                         * (same for secure and non-secure world) or banked.
                         */
                        char *name;

                        switch (r->secure) {
                        case ARM_CP_SECSTATE_S:
                        case ARM_CP_SECSTATE_NS:
                            add_cpreg_to_hashtable(cpu, r, opaque, state,
                                                   r->secure, crm, opc1, opc2,
                                                   r->name);
                            break;
                        default:
                            name = g_strdup_printf("%s_S", r->name);
                            add_cpreg_to_hashtable(cpu, r, opaque, state,
                                                   ARM_CP_SECSTATE_S,
                                                   crm, opc1, opc2, name);
                            g_free(name);
                            add_cpreg_to_hashtable(cpu, r, opaque, state,
                                                   ARM_CP_SECSTATE_NS,
                                                   crm, opc1, opc2, r->name);
                            break;
                        }
                    } else {
                        /* AArch64 registers get mapped to non-secure instance
                         * of AArch32 */
                        add_cpreg_to_hashtable(cpu, r, opaque, state,
                                               ARM_CP_SECSTATE_NS,
                                               crm, opc1, opc2, r->name);
                    }
                }
            }
        }
    }
}

void define_arm_cp_regs_with_opaque(ARMCPU *cpu,
                                    const ARMCPRegInfo *regs, void *opaque)
{
    /* Define a whole list of registers */
    const ARMCPRegInfo *r;
    for (r = regs; r->type != ARM_CP_SENTINEL; r++) {
        define_one_arm_cp_reg_with_opaque(cpu, r, opaque);
    }
}

const ARMCPRegInfo *get_arm_cp_reginfo(GHashTable *cpregs, uint32_t encoded_cp)
{
    return g_hash_table_lookup(cpregs, &encoded_cp);
}

void arm_cp_write_ignore(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    /* Helper coprocessor write function for write-ignore registers */
}

uint64_t arm_cp_read_zero(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /* Helper coprocessor write function for read-as-zero registers */
    return 0;
}

void arm_cp_reset_ignore(CPUARMState *env, const ARMCPRegInfo *opaque)
{
    /* Helper coprocessor reset function for do-nothing-on-reset registers */
}

static int bad_mode_switch(CPUARMState *env, int mode, CPSRWriteType write_type)
{
    /* Return true if it is not valid for us to switch to
     * this CPU mode (ie all the UNPREDICTABLE cases in
     * the ARM ARM CPSRWriteByInstr pseudocode).
     */

    /* Changes to or from Hyp via MSR and CPS are illegal. */
    if (write_type == CPSRWriteByInstr &&
        ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_HYP ||
         mode == ARM_CPU_MODE_HYP)) {
        return 1;
    }

    switch (mode) {
    case ARM_CPU_MODE_USR:
        return 0;
    case ARM_CPU_MODE_SYS:
    case ARM_CPU_MODE_SVC:
    case ARM_CPU_MODE_ABT:
    case ARM_CPU_MODE_UND:
    case ARM_CPU_MODE_IRQ:
    case ARM_CPU_MODE_FIQ:
        /* Note that we don't implement the IMPDEF NSACR.RFR which in v7
         * allows FIQ mode to be Secure-only. (In v8 this doesn't exist.)
         */
        /* If HCR.TGE is set then changes from Monitor to NS PL1 via MSR
         * and CPS are treated as illegal mode changes.
         */
        if (write_type == CPSRWriteByInstr &&
            (env->cp15.hcr_el2 & HCR_TGE) &&
            (env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_MON &&
            !arm_is_secure_below_el3(env)) {
            return 1;
        }
        return 0;
    case ARM_CPU_MODE_HYP:
        return !arm_feature(env, ARM_FEATURE_EL2)
            || arm_current_el(env) < 2 || arm_is_secure(env);
    case ARM_CPU_MODE_MON:
        return arm_current_el(env) < 3;
    default:
        return 1;
    }
}

uint32_t cpsr_read(CPUARMState *env)
{
    int ZF;
    ZF = (env->ZF == 0);
    return env->uncached_cpsr | (env->NF & 0x80000000) | (ZF << 30) |
        (env->CF << 29) | ((env->VF & 0x80000000) >> 3) | (env->QF << 27)
        | (env->thumb << 5) | ((env->condexec_bits & 3) << 25)
        | ((env->condexec_bits & 0xfc) << 8)
        | (env->GE << 16) | (env->daif & CPSR_AIF);
}

void cpsr_write(CPUARMState *env, uint32_t val, uint32_t mask,
                CPSRWriteType write_type)
{
    uint32_t changed_daif;

    if (mask & CPSR_NZCV) {
        env->ZF = (~val) & CPSR_Z;
        env->NF = val;
        env->CF = (val >> 29) & 1;
        env->VF = (val << 3) & 0x80000000;
    }
    if (mask & CPSR_Q)
        env->QF = ((val & CPSR_Q) != 0);
    if (mask & CPSR_T)
        env->thumb = ((val & CPSR_T) != 0);
    if (mask & CPSR_IT_0_1) {
        env->condexec_bits &= ~3;
        env->condexec_bits |= (val >> 25) & 3;
    }
    if (mask & CPSR_IT_2_7) {
        env->condexec_bits &= 3;
        env->condexec_bits |= (val >> 8) & 0xfc;
    }
    if (mask & CPSR_GE) {
        env->GE = (val >> 16) & 0xf;
    }

    /* In a V7 implementation that includes the security extensions but does
     * not include Virtualization Extensions the SCR.FW and SCR.AW bits control
     * whether non-secure software is allowed to change the CPSR_F and CPSR_A
     * bits respectively.
     *
     * In a V8 implementation, it is permitted for privileged software to
     * change the CPSR A/F bits regardless of the SCR.AW/FW bits.
     */
    if (write_type != CPSRWriteRaw && !arm_feature(env, ARM_FEATURE_V8) &&
        arm_feature(env, ARM_FEATURE_EL3) &&
        !arm_feature(env, ARM_FEATURE_EL2) &&
        !arm_is_secure(env)) {

        changed_daif = (env->daif ^ val) & mask;

        if (changed_daif & CPSR_A) {
            /* Check to see if we are allowed to change the masking of async
             * abort exceptions from a non-secure state.
             */
            if (!(env->cp15.scr_el3 & SCR_AW)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Ignoring attempt to switch CPSR_A flag from "
                              "non-secure world with SCR.AW bit clear\n");
                mask &= ~CPSR_A;
            }
        }

        if (changed_daif & CPSR_F) {
            /* Check to see if we are allowed to change the masking of FIQ
             * exceptions from a non-secure state.
             */
            if (!(env->cp15.scr_el3 & SCR_FW)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Ignoring attempt to switch CPSR_F flag from "
                              "non-secure world with SCR.FW bit clear\n");
                mask &= ~CPSR_F;
            }

            /* Check whether non-maskable FIQ (NMFI) support is enabled.
             * If this bit is set software is not allowed to mask
             * FIQs, but is allowed to set CPSR_F to 0.
             */
            if ((A32_BANKED_CURRENT_REG_GET(env, sctlr) & SCTLR_NMFI) &&
                (val & CPSR_F)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Ignoring attempt to enable CPSR_F flag "
                              "(non-maskable FIQ [NMFI] support enabled)\n");
                mask &= ~CPSR_F;
            }
        }
    }

    env->daif &= ~(CPSR_AIF & mask);
    env->daif |= val & CPSR_AIF & mask;

    if (write_type != CPSRWriteRaw &&
        ((env->uncached_cpsr ^ val) & mask & CPSR_M)) {
        if ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_USR) {
            /* Note that we can only get here in USR mode if this is a
             * gdb stub write; for this case we follow the architectural
             * behaviour for guest writes in USR mode of ignoring an attempt
             * to switch mode. (Those are caught by translate.c for writes
             * triggered by guest instructions.)
             */
            mask &= ~CPSR_M;
        } else if (bad_mode_switch(env, val & CPSR_M, write_type)) {
            /* Attempt to switch to an invalid mode: this is UNPREDICTABLE in
             * v7, and has defined behaviour in v8:
             *  + leave CPSR.M untouched
             *  + allow changes to the other CPSR fields
             *  + set PSTATE.IL
             * For user changes via the GDB stub, we don't set PSTATE.IL,
             * as this would be unnecessarily harsh for a user error.
             */
            mask &= ~CPSR_M;
            if (write_type != CPSRWriteByGDBStub &&
                arm_feature(env, ARM_FEATURE_V8)) {
                mask |= CPSR_IL;
                val |= CPSR_IL;
            }
        } else {
            switch_mode(env, val & CPSR_M);
        }
    }
    mask &= ~CACHED_CPSR_BITS;
    env->uncached_cpsr = (env->uncached_cpsr & ~mask) | (val & mask);
}

#if defined(CONFIG_USER_ONLY)

void switch_mode(CPUARMState *env, int mode)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    if (mode != ARM_CPU_MODE_USR) {
        cpu_abort(CPU(cpu), "Tried to switch out of user mode\n");
    }
}

uint32_t arm_phys_excp_target_el(CPUState *cs, uint32_t excp_idx,
                                 uint32_t cur_el, bool secure)
{
    return 1;
}

void aarch64_sync_64_to_32(CPUARMState *env)
{
    g_assert_not_reached();
}

#else

void switch_mode(CPUARMState *env, int mode)
{
    int old_mode;
    int i;

    old_mode = env->uncached_cpsr & CPSR_M;
    if (mode == old_mode)
        return;

    if (old_mode == ARM_CPU_MODE_FIQ) {
        memcpy (env->fiq_regs, env->regs + 8, 5 * sizeof(uint32_t));
        memcpy (env->regs + 8, env->usr_regs, 5 * sizeof(uint32_t));
    } else if (mode == ARM_CPU_MODE_FIQ) {
        memcpy (env->usr_regs, env->regs + 8, 5 * sizeof(uint32_t));
        memcpy (env->regs + 8, env->fiq_regs, 5 * sizeof(uint32_t));
    }

    i = bank_number(old_mode);
    env->banked_r13[i] = env->regs[13];
    env->banked_r14[i] = env->regs[14];
    env->banked_spsr[i] = env->spsr;

    i = bank_number(mode);
    env->regs[13] = env->banked_r13[i];
    env->regs[14] = env->banked_r14[i];
    env->spsr = env->banked_spsr[i];
}

/* Physical Interrupt Target EL Lookup Table
 *
 * [ From ARM ARM section G1.13.4 (Table G1-15) ]
 *
 * The below multi-dimensional table is used for looking up the target
 * exception level given numerous condition criteria.  Specifically, the
 * target EL is based on SCR and HCR routing controls as well as the
 * currently executing EL and secure state.
 *
 *    Dimensions:
 *    target_el_table[2][2][2][2][2][4]
 *                    |  |  |  |  |  +--- Current EL
 *                    |  |  |  |  +------ Non-secure(0)/Secure(1)
 *                    |  |  |  +--------- HCR mask override
 *                    |  |  +------------ SCR exec state control
 *                    |  +--------------- SCR mask override
 *                    +------------------ 32-bit(0)/64-bit(1) EL3
 *
 *    The table values are as such:
 *    0-3 = EL0-EL3
 *     -1 = Cannot occur
 *
 * The ARM ARM target EL table includes entries indicating that an "exception
 * is not taken".  The two cases where this is applicable are:
 *    1) An exception is taken from EL3 but the SCR does not have the exception
 *    routed to EL3.
 *    2) An exception is taken from EL2 but the HCR does not have the exception
 *    routed to EL2.
 * In these two cases, the below table contain a target of EL1.  This value is
 * returned as it is expected that the consumer of the table data will check
 * for "target EL >= current EL" to ensure the exception is not taken.
 *
 *            SCR     HCR
 *         64  EA     AMO                 From
 *        BIT IRQ     IMO      Non-secure         Secure
 *        EL3 FIQ  RW FMO   EL0 EL1 EL2 EL3   EL0 EL1 EL2 EL3
 */
static const int8_t target_el_table[2][2][2][2][2][4] = {
    {{{{/* 0   0   0   0 */{ 1,  1,  2, -1 },{ 3, -1, -1,  3 },},
       {/* 0   0   0   1 */{ 2,  2,  2, -1 },{ 3, -1, -1,  3 },},},
      {{/* 0   0   1   0 */{ 1,  1,  2, -1 },{ 3, -1, -1,  3 },},
       {/* 0   0   1   1 */{ 2,  2,  2, -1 },{ 3, -1, -1,  3 },},},},
     {{{/* 0   1   0   0 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},
       {/* 0   1   0   1 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},},
      {{/* 0   1   1   0 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},
       {/* 0   1   1   1 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},},},},
    {{{{/* 1   0   0   0 */{ 1,  1,  2, -1 },{ 1,  1, -1,  1 },},
       {/* 1   0   0   1 */{ 2,  2,  2, -1 },{ 1,  1, -1,  1 },},},
      {{/* 1   0   1   0 */{ 1,  1,  1, -1 },{ 1,  1, -1,  1 },},
       {/* 1   0   1   1 */{ 2,  2,  2, -1 },{ 1,  1, -1,  1 },},},},
     {{{/* 1   1   0   0 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},
       {/* 1   1   0   1 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},},
      {{/* 1   1   1   0 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},
       {/* 1   1   1   1 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},},},},
};

/*
 * Determine the target EL for physical exceptions
 */
uint32_t arm_phys_excp_target_el(CPUState *cs, uint32_t excp_idx,
                                 uint32_t cur_el, bool secure)
{
    CPUARMState *env = cs->env_ptr;
    int rw;
    int scr;
    int hcr;
    int target_el;
    /* Is the highest EL AArch64? */
    int is64 = arm_feature(env, ARM_FEATURE_AARCH64);

    if (arm_feature(env, ARM_FEATURE_EL3)) {
        rw = ((env->cp15.scr_el3 & SCR_RW) == SCR_RW);
    } else {
        /* Either EL2 is the highest EL (and so the EL2 register width
         * is given by is64); or there is no EL2 or EL3, in which case
         * the value of 'rw' does not affect the table lookup anyway.
         */
        rw = is64;
    }

    switch (excp_idx) {
    case EXCP_IRQ:
        scr = ((env->cp15.scr_el3 & SCR_IRQ) == SCR_IRQ);
        hcr = ((env->cp15.hcr_el2 & HCR_IMO) == HCR_IMO);
        break;
    case EXCP_FIQ:
        scr = ((env->cp15.scr_el3 & SCR_FIQ) == SCR_FIQ);
        hcr = ((env->cp15.hcr_el2 & HCR_FMO) == HCR_FMO);
        break;
    default:
        scr = ((env->cp15.scr_el3 & SCR_EA) == SCR_EA);
        hcr = ((env->cp15.hcr_el2 & HCR_AMO) == HCR_AMO);
        break;
    };

    /* If HCR.TGE is set then HCR is treated as being 1 */
    hcr |= ((env->cp15.hcr_el2 & HCR_TGE) == HCR_TGE);

    /* Perform a table-lookup for the target EL given the current state */
    target_el = target_el_table[is64][scr][rw][hcr][secure][cur_el];

    assert(target_el > 0);

    return target_el;
}

/* Return true if we're using the process stack pointer (not the MSP) */
bool v7m_using_psp(CPUARMState *env)
{
    /* Handler mode always uses the main stack; for thread mode
     * the CONTROL.SPSEL bit determines the answer.
     * Note that in v7M it is not possible to be in Handler mode with
     * CONTROL.SPSEL non-zero, but in v8M it is, so we must check both.
     */
    return !arm_v7m_is_handler_mode(env) &&
        env->v7m.control[env->v7m.secure] & R_V7M_CONTROL_SPSEL_MASK;
}

void write_v7m_exception(CPUARMState *env, uint32_t new_exc)
{
    /* Write a new value to v7m.exception, thus transitioning into or out
     * of Handler mode; this may result in a change of active stack pointer.
     */
    bool new_is_psp, old_is_psp = v7m_using_psp(env);
    uint32_t tmp;

    env->v7m.exception = new_exc;

    new_is_psp = v7m_using_psp(env);

    if (old_is_psp != new_is_psp) {
        tmp = env->v7m.other_sp;
        env->v7m.other_sp = env->regs[13];
        env->regs[13] = tmp;
    }
}

/* Function used to synchronize QEMU's AArch64 register set with AArch32
 * register set.  This is necessary when switching between AArch32 and AArch64
 * execution state.
 */
void aarch64_sync_32_to_64(CPUARMState *env)
{
    int i;
    uint32_t mode = env->uncached_cpsr & CPSR_M;

    /* We can blanket copy R[0:7] to X[0:7] */
    for (i = 0; i < 8; i++) {
        env->xregs[i] = env->regs[i];
    }

    /* Unless we are in FIQ mode, x8-x12 come from the user registers r8-r12.
     * Otherwise, they come from the banked user regs.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 8; i < 13; i++) {
            env->xregs[i] = env->usr_regs[i - 8];
        }
    } else {
        for (i = 8; i < 13; i++) {
            env->xregs[i] = env->regs[i];
        }
    }

    /* Registers x13-x23 are the various mode SP and FP registers. Registers
     * r13 and r14 are only copied if we are in that mode, otherwise we copy
     * from the mode banked register.
     */
    if (mode == ARM_CPU_MODE_USR || mode == ARM_CPU_MODE_SYS) {
        env->xregs[13] = env->regs[13];
        env->xregs[14] = env->regs[14];
    } else {
        env->xregs[13] = env->banked_r13[bank_number(ARM_CPU_MODE_USR)];
        /* HYP is an exception in that it is copied from r14 */
        if (mode == ARM_CPU_MODE_HYP) {
            env->xregs[14] = env->regs[14];
        } else {
            env->xregs[14] = env->banked_r14[bank_number(ARM_CPU_MODE_USR)];
        }
    }

    if (mode == ARM_CPU_MODE_HYP) {
        env->xregs[15] = env->regs[13];
    } else {
        env->xregs[15] = env->banked_r13[bank_number(ARM_CPU_MODE_HYP)];
    }

    if (mode == ARM_CPU_MODE_IRQ) {
        env->xregs[16] = env->regs[14];
        env->xregs[17] = env->regs[13];
    } else {
        env->xregs[16] = env->banked_r14[bank_number(ARM_CPU_MODE_IRQ)];
        env->xregs[17] = env->banked_r13[bank_number(ARM_CPU_MODE_IRQ)];
    }

    if (mode == ARM_CPU_MODE_SVC) {
        env->xregs[18] = env->regs[14];
        env->xregs[19] = env->regs[13];
    } else {
        env->xregs[18] = env->banked_r14[bank_number(ARM_CPU_MODE_SVC)];
        env->xregs[19] = env->banked_r13[bank_number(ARM_CPU_MODE_SVC)];
    }

    if (mode == ARM_CPU_MODE_ABT) {
        env->xregs[20] = env->regs[14];
        env->xregs[21] = env->regs[13];
    } else {
        env->xregs[20] = env->banked_r14[bank_number(ARM_CPU_MODE_ABT)];
        env->xregs[21] = env->banked_r13[bank_number(ARM_CPU_MODE_ABT)];
    }

    if (mode == ARM_CPU_MODE_UND) {
        env->xregs[22] = env->regs[14];
        env->xregs[23] = env->regs[13];
    } else {
        env->xregs[22] = env->banked_r14[bank_number(ARM_CPU_MODE_UND)];
        env->xregs[23] = env->banked_r13[bank_number(ARM_CPU_MODE_UND)];
    }

    /* Registers x24-x30 are mapped to r8-r14 in FIQ mode.  If we are in FIQ
     * mode, then we can copy from r8-r14.  Otherwise, we copy from the
     * FIQ bank for r8-r14.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 24; i < 31; i++) {
            env->xregs[i] = env->regs[i - 16];   /* X[24:30] <- R[8:14] */
        }
    } else {
        for (i = 24; i < 29; i++) {
            env->xregs[i] = env->fiq_regs[i - 24];
        }
        env->xregs[29] = env->banked_r13[bank_number(ARM_CPU_MODE_FIQ)];
        env->xregs[30] = env->banked_r14[bank_number(ARM_CPU_MODE_FIQ)];
    }

    env->pc = env->regs[15];
}

/* Function used to synchronize QEMU's AArch32 register set with AArch64
 * register set.  This is necessary when switching between AArch32 and AArch64
 * execution state.
 */
void aarch64_sync_64_to_32(CPUARMState *env)
{
    int i;
    uint32_t mode = env->uncached_cpsr & CPSR_M;

    /* We can blanket copy X[0:7] to R[0:7] */
    for (i = 0; i < 8; i++) {
        env->regs[i] = env->xregs[i];
    }

    /* Unless we are in FIQ mode, r8-r12 come from the user registers x8-x12.
     * Otherwise, we copy x8-x12 into the banked user regs.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 8; i < 13; i++) {
            env->usr_regs[i - 8] = env->xregs[i];
        }
    } else {
        for (i = 8; i < 13; i++) {
            env->regs[i] = env->xregs[i];
        }
    }

    /* Registers r13 & r14 depend on the current mode.
     * If we are in a given mode, we copy the corresponding x registers to r13
     * and r14.  Otherwise, we copy the x register to the banked r13 and r14
     * for the mode.
     */
    if (mode == ARM_CPU_MODE_USR || mode == ARM_CPU_MODE_SYS) {
        env->regs[13] = env->xregs[13];
        env->regs[14] = env->xregs[14];
    } else {
        env->banked_r13[bank_number(ARM_CPU_MODE_USR)] = env->xregs[13];

        /* HYP is an exception in that it does not have its own banked r14 but
         * shares the USR r14
         */
        if (mode == ARM_CPU_MODE_HYP) {
            env->regs[14] = env->xregs[14];
        } else {
            env->banked_r14[bank_number(ARM_CPU_MODE_USR)] = env->xregs[14];
        }
    }

    if (mode == ARM_CPU_MODE_HYP) {
        env->regs[13] = env->xregs[15];
    } else {
        env->banked_r13[bank_number(ARM_CPU_MODE_HYP)] = env->xregs[15];
    }

    if (mode == ARM_CPU_MODE_IRQ) {
        env->regs[14] = env->xregs[16];
        env->regs[13] = env->xregs[17];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_IRQ)] = env->xregs[16];
        env->banked_r13[bank_number(ARM_CPU_MODE_IRQ)] = env->xregs[17];
    }

    if (mode == ARM_CPU_MODE_SVC) {
        env->regs[14] = env->xregs[18];
        env->regs[13] = env->xregs[19];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_SVC)] = env->xregs[18];
        env->banked_r13[bank_number(ARM_CPU_MODE_SVC)] = env->xregs[19];
    }

    if (mode == ARM_CPU_MODE_ABT) {
        env->regs[14] = env->xregs[20];
        env->regs[13] = env->xregs[21];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_ABT)] = env->xregs[20];
        env->banked_r13[bank_number(ARM_CPU_MODE_ABT)] = env->xregs[21];
    }

    if (mode == ARM_CPU_MODE_UND) {
        env->regs[14] = env->xregs[22];
        env->regs[13] = env->xregs[23];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_UND)] = env->xregs[22];
        env->banked_r13[bank_number(ARM_CPU_MODE_UND)] = env->xregs[23];
    }

    /* Registers x24-x30 are mapped to r8-r14 in FIQ mode.  If we are in FIQ
     * mode, then we can copy to r8-r14.  Otherwise, we copy to the
     * FIQ bank for r8-r14.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 24; i < 31; i++) {
            env->regs[i - 16] = env->xregs[i];   /* X[24:30] -> R[8:14] */
        }
    } else {
        for (i = 24; i < 29; i++) {
            env->fiq_regs[i - 24] = env->xregs[i];
        }
        env->banked_r13[bank_number(ARM_CPU_MODE_FIQ)] = env->xregs[29];
        env->banked_r14[bank_number(ARM_CPU_MODE_FIQ)] = env->xregs[30];
    }

    env->regs[15] = env->pc;
}

/* Convert a possible stage1+2 MMU index into the appropriate
 * stage 1 MMU index
 */
static inline ARMMMUIdx stage_1_mmu_idx(ARMMMUIdx mmu_idx)
{
    if (mmu_idx == ARMMMUIdx_S12NSE0 || mmu_idx == ARMMMUIdx_S12NSE1) {
        mmu_idx += (ARMMMUIdx_S1NSE0 - ARMMMUIdx_S12NSE0);
    }
    return mmu_idx;
}

/* Returns TBI0 value for current regime el */
uint32_t arm_regime_tbi0(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    TCR *tcr;
    uint32_t el;

    /* For EL0 and EL1, TBI is controlled by stage 1's TCR, so convert
     * a stage 1+2 mmu index into the appropriate stage 1 mmu index.
     */
    mmu_idx = stage_1_mmu_idx(mmu_idx);

    tcr = regime_tcr(env, mmu_idx);
    el = regime_el(env, mmu_idx);

    if (el > 1) {
        return extract64(tcr->raw_tcr, 20, 1);
    } else {
        return extract64(tcr->raw_tcr, 37, 1);
    }
}

/* Returns TBI1 value for current regime el */
uint32_t arm_regime_tbi1(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    TCR *tcr;
    uint32_t el;

    /* For EL0 and EL1, TBI is controlled by stage 1's TCR, so convert
     * a stage 1+2 mmu index into the appropriate stage 1 mmu index.
     */
    mmu_idx = stage_1_mmu_idx(mmu_idx);

    tcr = regime_tcr(env, mmu_idx);
    el = regime_el(env, mmu_idx);

    if (el > 1) {
        return 0;
    } else {
        return extract64(tcr->raw_tcr, 38, 1);
    }
}

/* Returns true if the stage 1 translation regime is using LPAE format page
 * tables. Used when raising alignment exceptions, whose FSR changes depending
 * on whether the long or short descriptor format is in use. */
bool arm_s1_regime_using_lpae_format(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    mmu_idx = stage_1_mmu_idx(mmu_idx);

    return regime_using_lpae_format(env, mmu_idx);
}

/* Translate S2 section/page access permissions to protection flags
 *
 * @env:     CPUARMState
 * @s2ap:    The 2-bit stage2 access permissions (S2AP)
 * @xn:      XN (execute-never) bit
 */
static int get_S2prot(CPUARMState *env, int s2ap, int xn)
{
    int prot = 0;

    if (s2ap & 1) {
        prot |= PAGE_READ;
    }
    if (s2ap & 2) {
        prot |= PAGE_WRITE;
    }
    if (!xn) {
        if (arm_el_is_aa64(env, 2) || prot & PAGE_READ) {
            prot |= PAGE_EXEC;
        }
    }
    return prot;
}

/* Translate section/page access permissions to protection flags
 *
 * @env:     CPUARMState
 * @mmu_idx: MMU index indicating required translation regime
 * @is_aa64: TRUE if AArch64
 * @ap:      The 2-bit simple AP (AP[2:1])
 * @ns:      NS (non-secure) bit
 * @xn:      XN (execute-never) bit
 * @pxn:     PXN (privileged execute-never) bit
 */
static int get_S1prot(CPUARMState *env, ARMMMUIdx mmu_idx, bool is_aa64,
                      int ap, int ns, int xn, int pxn)
{
    bool is_user = regime_is_user(env, mmu_idx);
    int prot_rw, user_rw;
    bool have_wxn;
    int wxn = 0;

    assert(mmu_idx != ARMMMUIdx_S2NS);

    user_rw = simple_ap_to_rw_prot_is_user(ap, true);
    if (is_user) {
        prot_rw = user_rw;
    } else {
        prot_rw = simple_ap_to_rw_prot_is_user(ap, false);
    }

    if (ns && arm_is_secure(env) && (env->cp15.scr_el3 & SCR_SIF)) {
        return prot_rw;
    }

    /* TODO have_wxn should be replaced with
     *   ARM_FEATURE_V8 || (ARM_FEATURE_V7 && ARM_FEATURE_EL2)
     * when ARM_FEATURE_EL2 starts getting set. For now we assume all LPAE
     * compatible processors have EL2, which is required for [U]WXN.
     */
    have_wxn = arm_feature(env, ARM_FEATURE_LPAE);

    if (have_wxn) {
        wxn = regime_sctlr(env, mmu_idx) & SCTLR_WXN;
    }

    if (is_aa64) {
        switch (regime_el(env, mmu_idx)) {
        case 1:
            if (!is_user) {
                xn = pxn || (user_rw & PAGE_WRITE);
            }
            break;
        case 2:
        case 3:
            break;
        }
    } else if (arm_feature(env, ARM_FEATURE_V7)) {
        switch (regime_el(env, mmu_idx)) {
        case 1:
        case 3:
            if (is_user) {
                xn = xn || !(user_rw & PAGE_READ);
            } else {
                int uwxn = 0;
                if (have_wxn) {
                    uwxn = regime_sctlr(env, mmu_idx) & SCTLR_UWXN;
                }
                xn = xn || !(prot_rw & PAGE_READ) || pxn ||
                     (uwxn && (user_rw & PAGE_WRITE));
            }
            break;
        case 2:
            break;
        }
    } else {
        xn = wxn = 0;
    }

    if (xn || (wxn && (prot_rw & PAGE_WRITE))) {
        return prot_rw;
    }
    return prot_rw | PAGE_EXEC;
}

static bool get_level1_table_address(CPUARMState *env, ARMMMUIdx mmu_idx,
                                     uint32_t *table, uint32_t address)
{
    /* Note that we can only get here for an AArch32 PL0/PL1 lookup */
    TCR *tcr = regime_tcr(env, mmu_idx);

    if (address & tcr->mask) {
        if (tcr->raw_tcr & TTBCR_PD1) {
            /* Translation table walk disabled for TTBR1 */
            return false;
        }
        *table = regime_ttbr(env, mmu_idx, 1) & 0xffffc000;
    } else {
        if (tcr->raw_tcr & TTBCR_PD0) {
            /* Translation table walk disabled for TTBR0 */
            return false;
        }
        *table = regime_ttbr(env, mmu_idx, 0) & tcr->base_mask;
    }
    *table |= (address >> 18) & 0x3ffc;
    return true;
}

/* Translate a S1 pagetable walk through S2 if needed.  */
static hwaddr S1_ptw_translate(CPUARMState *env, ARMMMUIdx mmu_idx,
                               hwaddr addr, MemTxAttrs txattrs,
                               ARMMMUFaultInfo *fi)
{
    if ((mmu_idx == ARMMMUIdx_S1NSE0 || mmu_idx == ARMMMUIdx_S1NSE1) &&
        !regime_translation_disabled(env, ARMMMUIdx_S2NS)) {
        target_ulong s2size;
        hwaddr s2pa;
        int s2prot;
        int ret;

        ret = get_phys_addr_lpae(env, addr, 0, ARMMMUIdx_S2NS, &s2pa,
                                 &txattrs, &s2prot, &s2size, fi, NULL);
        if (ret) {
            assert(fi->type != ARMFault_None);
            fi->s2addr = addr;
            fi->stage2 = true;
            fi->s1ptw = true;
            return ~0;
        }
        addr = s2pa;
    }
    return addr;
}

/* All loads done in the course of a page table walk go through here. */
static uint32_t arm_ldl_ptw(CPUState *cs, hwaddr addr, bool is_secure,
                            ARMMMUIdx mmu_idx, ARMMMUFaultInfo *fi)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    MemTxAttrs attrs = {};
    MemTxResult result = MEMTX_OK;
    AddressSpace *as;
    uint32_t data;

    attrs.secure = is_secure;
    as = arm_addressspace(cs, attrs);
    addr = S1_ptw_translate(env, mmu_idx, addr, attrs, fi);
    if (fi->s1ptw) {
        return 0;
    }
    if (regime_translation_big_endian(env, mmu_idx)) {
        data = address_space_ldl_be(as, addr, attrs, &result);
    } else {
        data = address_space_ldl_le(as, addr, attrs, &result);
    }
    if (result == MEMTX_OK) {
        return data;
    }
    fi->type = ARMFault_SyncExternalOnWalk;
    fi->ea = arm_extabort_type(result);
    return 0;
}

static uint64_t arm_ldq_ptw(CPUState *cs, hwaddr addr, bool is_secure,
                            ARMMMUIdx mmu_idx, ARMMMUFaultInfo *fi)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    MemTxAttrs attrs = {};
    MemTxResult result = MEMTX_OK;
    AddressSpace *as;
    uint64_t data;

    attrs.secure = is_secure;
    as = arm_addressspace(cs, attrs);
    addr = S1_ptw_translate(env, mmu_idx, addr, attrs, fi);
    if (fi->s1ptw) {
        return 0;
    }
    if (regime_translation_big_endian(env, mmu_idx)) {
        data = address_space_ldq_be(as, addr, attrs, &result);
    } else {
        data = address_space_ldq_le(as, addr, attrs, &result);
    }
    if (result == MEMTX_OK) {
        return data;
    }
    fi->type = ARMFault_SyncExternalOnWalk;
    fi->ea = arm_extabort_type(result);
    return 0;
}

static bool get_phys_addr_v5(CPUARMState *env, uint32_t address,
                             MMUAccessType access_type, ARMMMUIdx mmu_idx,
                             hwaddr *phys_ptr, int *prot,
                             target_ulong *page_size,
                             ARMMMUFaultInfo *fi)
{
    CPUState *cs = CPU(arm_env_get_cpu(env));
    int level = 1;
    uint32_t table;
    uint32_t desc;
    int type;
    int ap;
    int domain = 0;
    int domain_prot;
    hwaddr phys_addr;
    uint32_t dacr;

    /* Pagetable walk.  */
    /* Lookup l1 descriptor.  */
    if (!get_level1_table_address(env, mmu_idx, &table, address)) {
        /* Section translation fault if page walk is disabled by PD0 or PD1 */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                       mmu_idx, fi);
    if (fi->type != ARMFault_None) {
        goto do_fault;
    }
    type = (desc & 3);
    domain = (desc >> 5) & 0x0f;
    if (regime_el(env, mmu_idx) == 1) {
        dacr = env->cp15.dacr_ns;
    } else {
        dacr = env->cp15.dacr_s;
    }
    domain_prot = (dacr >> (domain * 2)) & 3;
    if (type == 0) {
        /* Section translation fault.  */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    if (type != 2) {
        level = 2;
    }
    if (domain_prot == 0 || domain_prot == 2) {
        fi->type = ARMFault_Domain;
        goto do_fault;
    }
    if (type == 2) {
        /* 1Mb section.  */
        phys_addr = (desc & 0xfff00000) | (address & 0x000fffff);
        ap = (desc >> 10) & 3;
        *page_size = 1024 * 1024;
    } else {
        /* Lookup l2 entry.  */
        if (type == 1) {
            /* Coarse pagetable.  */
            table = (desc & 0xfffffc00) | ((address >> 10) & 0x3fc);
        } else {
            /* Fine pagetable.  */
            table = (desc & 0xfffff000) | ((address >> 8) & 0xffc);
        }
        desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                           mmu_idx, fi);
        if (fi->type != ARMFault_None) {
            goto do_fault;
        }
        switch (desc & 3) {
        case 0: /* Page translation fault.  */
            fi->type = ARMFault_Translation;
            goto do_fault;
        case 1: /* 64k page.  */
            phys_addr = (desc & 0xffff0000) | (address & 0xffff);
            ap = (desc >> (4 + ((address >> 13) & 6))) & 3;
            *page_size = 0x10000;
            break;
        case 2: /* 4k page.  */
            phys_addr = (desc & 0xfffff000) | (address & 0xfff);
            ap = (desc >> (4 + ((address >> 9) & 6))) & 3;
            *page_size = 0x1000;
            break;
        case 3: /* 1k page, or ARMv6/XScale "extended small (4k) page" */
            if (type == 1) {
                /* ARMv6/XScale extended small page format */
                if (arm_feature(env, ARM_FEATURE_XSCALE)
                    || arm_feature(env, ARM_FEATURE_V6)) {
                    phys_addr = (desc & 0xfffff000) | (address & 0xfff);
                    *page_size = 0x1000;
                } else {
                    /* UNPREDICTABLE in ARMv5; we choose to take a
                     * page translation fault.
                     */
                    fi->type = ARMFault_Translation;
                    goto do_fault;
                }
            } else {
                phys_addr = (desc & 0xfffffc00) | (address & 0x3ff);
                *page_size = 0x400;
            }
            ap = (desc >> 4) & 3;
            break;
        default:
            /* Never happens, but compiler isn't smart enough to tell.  */
            abort();
        }
    }
    *prot = ap_to_rw_prot(env, mmu_idx, ap, domain_prot);
    *prot |= *prot ? PAGE_EXEC : 0;
    if (!(*prot & (1 << access_type))) {
        /* Access permission fault.  */
        fi->type = ARMFault_Permission;
        goto do_fault;
    }
    *phys_ptr = phys_addr;
    return false;
do_fault:
    fi->domain = domain;
    fi->level = level;
    return true;
}

static bool get_phys_addr_v6(CPUARMState *env, uint32_t address,
                             MMUAccessType access_type, ARMMMUIdx mmu_idx,
                             hwaddr *phys_ptr, MemTxAttrs *attrs, int *prot,
                             target_ulong *page_size, ARMMMUFaultInfo *fi)
{
    CPUState *cs = CPU(arm_env_get_cpu(env));
    int level = 1;
    uint32_t table;
    uint32_t desc;
    uint32_t xn;
    uint32_t pxn = 0;
    int type;
    int ap;
    int domain = 0;
    int domain_prot;
    hwaddr phys_addr;
    uint32_t dacr;
    bool ns;

    /* Pagetable walk.  */
    /* Lookup l1 descriptor.  */
    if (!get_level1_table_address(env, mmu_idx, &table, address)) {
        /* Section translation fault if page walk is disabled by PD0 or PD1 */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                       mmu_idx, fi);
    if (fi->type != ARMFault_None) {
        goto do_fault;
    }
    type = (desc & 3);
    if (type == 0 || (type == 3 && !arm_feature(env, ARM_FEATURE_PXN))) {
        /* Section translation fault, or attempt to use the encoding
         * which is Reserved on implementations without PXN.
         */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    if ((type == 1) || !(desc & (1 << 18))) {
        /* Page or Section.  */
        domain = (desc >> 5) & 0x0f;
    }
    if (regime_el(env, mmu_idx) == 1) {
        dacr = env->cp15.dacr_ns;
    } else {
        dacr = env->cp15.dacr_s;
    }
    if (type == 1) {
        level = 2;
    }
    domain_prot = (dacr >> (domain * 2)) & 3;
    if (domain_prot == 0 || domain_prot == 2) {
        /* Section or Page domain fault */
        fi->type = ARMFault_Domain;
        goto do_fault;
    }
    if (type != 1) {
        if (desc & (1 << 18)) {
            /* Supersection.  */
            phys_addr = (desc & 0xff000000) | (address & 0x00ffffff);
            phys_addr |= (uint64_t)extract32(desc, 20, 4) << 32;
            phys_addr |= (uint64_t)extract32(desc, 5, 4) << 36;
            *page_size = 0x1000000;
        } else {
            /* Section.  */
            phys_addr = (desc & 0xfff00000) | (address & 0x000fffff);
            *page_size = 0x100000;
        }
        ap = ((desc >> 10) & 3) | ((desc >> 13) & 4);
        xn = desc & (1 << 4);
        pxn = desc & 1;
        ns = extract32(desc, 19, 1);
    } else {
        if (arm_feature(env, ARM_FEATURE_PXN)) {
            pxn = (desc >> 2) & 1;
        }
        ns = extract32(desc, 3, 1);
        /* Lookup l2 entry.  */
        table = (desc & 0xfffffc00) | ((address >> 10) & 0x3fc);
        desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                           mmu_idx, fi);
        if (fi->type != ARMFault_None) {
            goto do_fault;
        }
        ap = ((desc >> 4) & 3) | ((desc >> 7) & 4);
        switch (desc & 3) {
        case 0: /* Page translation fault.  */
            fi->type = ARMFault_Translation;
            goto do_fault;
        case 1: /* 64k page.  */
            phys_addr = (desc & 0xffff0000) | (address & 0xffff);
            xn = desc & (1 << 15);
            *page_size = 0x10000;
            break;
        case 2: case 3: /* 4k page.  */
            phys_addr = (desc & 0xfffff000) | (address & 0xfff);
            xn = desc & 1;
            *page_size = 0x1000;
            break;
        default:
            /* Never happens, but compiler isn't smart enough to tell.  */
            abort();
        }
    }
    if (domain_prot == 3) {
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    } else {
        if (pxn && !regime_is_user(env, mmu_idx)) {
            xn = 1;
        }
        if (xn && access_type == MMU_INST_FETCH) {
            fi->type = ARMFault_Permission;
            goto do_fault;
        }

        if (arm_feature(env, ARM_FEATURE_V6K) &&
                (regime_sctlr(env, mmu_idx) & SCTLR_AFE)) {
            /* The simplified model uses AP[0] as an access control bit.  */
            if ((ap & 1) == 0) {
                /* Access flag fault.  */
                fi->type = ARMFault_AccessFlag;
                goto do_fault;
            }
            *prot = simple_ap_to_rw_prot(env, mmu_idx, ap >> 1);
        } else {
            *prot = ap_to_rw_prot(env, mmu_idx, ap, domain_prot);
        }
        if (*prot && !xn) {
            *prot |= PAGE_EXEC;
        }
        if (!(*prot & (1 << access_type))) {
            /* Access permission fault.  */
            fi->type = ARMFault_Permission;
            goto do_fault;
        }
    }
    if (ns) {
        /* The NS bit will (as required by the architecture) have no effect if
         * the CPU doesn't support TZ or this is a non-secure translation
         * regime, because the attribute will already be non-secure.
         */
        attrs->secure = false;
    }
    *phys_ptr = phys_addr;
    return false;
do_fault:
    fi->domain = domain;
    fi->level = level;
    return true;
}

/*
 * check_s2_mmu_setup
 * @cpu:        ARMCPU
 * @is_aa64:    True if the translation regime is in AArch64 state
 * @startlevel: Suggested starting level
 * @inputsize:  Bitsize of IPAs
 * @stride:     Page-table stride (See the ARM ARM)
 *
 * Returns true if the suggested S2 translation parameters are OK and
 * false otherwise.
 */
static bool check_s2_mmu_setup(ARMCPU *cpu, bool is_aa64, int level,
                               int inputsize, int stride)
{
    const int grainsize = stride + 3;
    int startsizecheck;

    /* Negative levels are never allowed.  */
    if (level < 0) {
        return false;
    }

    startsizecheck = inputsize - ((3 - level) * stride + grainsize);
    if (startsizecheck < 1 || startsizecheck > stride + 4) {
        return false;
    }

    if (is_aa64) {
        CPUARMState *env = &cpu->env;
        unsigned int pamax = arm_pamax(cpu);

        switch (stride) {
        case 13: /* 64KB Pages.  */
            if (level == 0 || (level == 1 && pamax <= 42)) {
                return false;
            }
            break;
        case 11: /* 16KB Pages.  */
            if (level == 0 || (level == 1 && pamax <= 40)) {
                return false;
            }
            break;
        case 9: /* 4KB Pages.  */
            if (level == 0 && pamax <= 42) {
                return false;
            }
            break;
        default:
            g_assert_not_reached();
        }

        /* Inputsize checks.  */
        if (inputsize > pamax &&
            (arm_el_is_aa64(env, 1) || inputsize > 40)) {
            /* This is CONSTRAINED UNPREDICTABLE and we choose to fault.  */
            return false;
        }
    } else {
        /* AArch32 only supports 4KB pages. Assert on that.  */
        assert(stride == 9);

        if (level == 0) {
            return false;
        }
    }
    return true;
}

/* Translate from the 4-bit stage 2 representation of
 * memory attributes (without cache-allocation hints) to
 * the 8-bit representation of the stage 1 MAIR registers
 * (which includes allocation hints).
 *
 * ref: shared/translation/attrs/S2AttrDecode()
 *      .../S2ConvertAttrsHints()
 */
static uint8_t convert_stage2_attrs(CPUARMState *env, uint8_t s2attrs)
{
    uint8_t hiattr = extract32(s2attrs, 2, 2);
    uint8_t loattr = extract32(s2attrs, 0, 2);
    uint8_t hihint = 0, lohint = 0;

    if (hiattr != 0) { /* normal memory */
        if ((env->cp15.hcr_el2 & HCR_CD) != 0) { /* cache disabled */
            hiattr = loattr = 1; /* non-cacheable */
        } else {
            if (hiattr != 1) { /* Write-through or write-back */
                hihint = 3; /* RW allocate */
            }
            if (loattr != 1) { /* Write-through or write-back */
                lohint = 3; /* RW allocate */
            }
        }
    }

    return (hiattr << 6) | (hihint << 4) | (loattr << 2) | lohint;
}

static bool get_phys_addr_lpae(CPUARMState *env, target_ulong address,
                               MMUAccessType access_type, ARMMMUIdx mmu_idx,
                               hwaddr *phys_ptr, MemTxAttrs *txattrs, int *prot,
                               target_ulong *page_size_ptr,
                               ARMMMUFaultInfo *fi, ARMCacheAttrs *cacheattrs)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);
    /* Read an LPAE long-descriptor translation table. */
    ARMFaultType fault_type = ARMFault_Translation;
    uint32_t level;
    uint32_t epd = 0;
    int32_t t0sz, t1sz;
    uint32_t tg;
    uint64_t ttbr;
    int ttbr_select;
    hwaddr descaddr, indexmask, indexmask_grainsize;
    uint32_t tableattrs;
    target_ulong page_size;
    uint32_t attrs;
    int32_t stride = 9;
    int32_t addrsize;
    int inputsize;
    int32_t tbi = 0;
    TCR *tcr = regime_tcr(env, mmu_idx);
    int ap, ns, xn, pxn;
    uint32_t el = regime_el(env, mmu_idx);
    bool ttbr1_valid = true;
    uint64_t descaddrmask;
    bool aarch64 = arm_el_is_aa64(env, el);

    /* TODO:
     * This code does not handle the different format TCR for VTCR_EL2.
     * This code also does not support shareability levels.
     * Attribute and permission bit handling should also be checked when adding
     * support for those page table walks.
     */
    if (aarch64) {
        level = 0;
        addrsize = 64;
        if (el > 1) {
            if (mmu_idx != ARMMMUIdx_S2NS) {
                tbi = extract64(tcr->raw_tcr, 20, 1);
            }
        } else {
            if (extract64(address, 55, 1)) {
                tbi = extract64(tcr->raw_tcr, 38, 1);
            } else {
                tbi = extract64(tcr->raw_tcr, 37, 1);
            }
        }
        tbi *= 8;

        /* If we are in 64-bit EL2 or EL3 then there is no TTBR1, so mark it
         * invalid.
         */
        if (el > 1) {
            ttbr1_valid = false;
        }
    } else {
        level = 1;
        addrsize = 32;
        /* There is no TTBR1 for EL2 */
        if (el == 2) {
            ttbr1_valid = false;
        }
    }

    /* Determine whether this address is in the region controlled by
     * TTBR0 or TTBR1 (or if it is in neither region and should fault).
     * This is a Non-secure PL0/1 stage 1 translation, so controlled by
     * TTBCR/TTBR0/TTBR1 in accordance with ARM ARM DDI0406C table B-32:
     */
    if (aarch64) {
        /* AArch64 translation.  */
        t0sz = extract32(tcr->raw_tcr, 0, 6);
        t0sz = MIN(t0sz, 39);
        t0sz = MAX(t0sz, 16);
    } else if (mmu_idx != ARMMMUIdx_S2NS) {
        /* AArch32 stage 1 translation.  */
        t0sz = extract32(tcr->raw_tcr, 0, 3);
    } else {
        /* AArch32 stage 2 translation.  */
        bool sext = extract32(tcr->raw_tcr, 4, 1);
        bool sign = extract32(tcr->raw_tcr, 3, 1);
        /* Address size is 40-bit for a stage 2 translation,
         * and t0sz can be negative (from -8 to 7),
         * so we need to adjust it to use the TTBR selecting logic below.
         */
        addrsize = 40;
        t0sz = sextract32(tcr->raw_tcr, 0, 4) + 8;

        /* If the sign-extend bit is not the same as t0sz[3], the result
         * is unpredictable. Flag this as a guest error.  */
        if (sign != sext) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "AArch32: VTCR.S / VTCR.T0SZ[3] mismatch\n");
        }
    }
    t1sz = extract32(tcr->raw_tcr, 16, 6);
    if (aarch64) {
        t1sz = MIN(t1sz, 39);
        t1sz = MAX(t1sz, 16);
    }
    if (t0sz && !extract64(address, addrsize - t0sz, t0sz - tbi)) {
        /* there is a ttbr0 region and we are in it (high bits all zero) */
        ttbr_select = 0;
    } else if (ttbr1_valid && t1sz &&
               !extract64(~address, addrsize - t1sz, t1sz - tbi)) {
        /* there is a ttbr1 region and we are in it (high bits all one) */
        ttbr_select = 1;
    } else if (!t0sz) {
        /* ttbr0 region is "everything not in the ttbr1 region" */
        ttbr_select = 0;
    } else if (!t1sz && ttbr1_valid) {
        /* ttbr1 region is "everything not in the ttbr0 region" */
        ttbr_select = 1;
    } else {
        /* in the gap between the two regions, this is a Translation fault */
        fault_type = ARMFault_Translation;
        goto do_fault;
    }

    /* Note that QEMU ignores shareability and cacheability attributes,
     * so we don't need to do anything with the SH, ORGN, IRGN fields
     * in the TTBCR.  Similarly, TTBCR:A1 selects whether we get the
     * ASID from TTBR0 or TTBR1, but QEMU's TLB doesn't currently
     * implement any ASID-like capability so we can ignore it (instead
     * we will always flush the TLB any time the ASID is changed).
     */
    if (ttbr_select == 0) {
        ttbr = regime_ttbr(env, mmu_idx, 0);
        if (el < 2) {
            epd = extract32(tcr->raw_tcr, 7, 1);
        }
        inputsize = addrsize - t0sz;

        tg = extract32(tcr->raw_tcr, 14, 2);
        if (tg == 1) { /* 64KB pages */
            stride = 13;
        }
        if (tg == 2) { /* 16KB pages */
            stride = 11;
        }
    } else {
        /* We should only be here if TTBR1 is valid */
        assert(ttbr1_valid);

        ttbr = regime_ttbr(env, mmu_idx, 1);
        epd = extract32(tcr->raw_tcr, 23, 1);
        inputsize = addrsize - t1sz;

        tg = extract32(tcr->raw_tcr, 30, 2);
        if (tg == 3)  { /* 64KB pages */
            stride = 13;
        }
        if (tg == 1) { /* 16KB pages */
            stride = 11;
        }
    }

    /* Here we should have set up all the parameters for the translation:
     * inputsize, ttbr, epd, stride, tbi
     */

    if (epd) {
        /* Translation table walk disabled => Translation fault on TLB miss
         * Note: This is always 0 on 64-bit EL2 and EL3.
         */
        goto do_fault;
    }

    if (mmu_idx != ARMMMUIdx_S2NS) {
        /* The starting level depends on the virtual address size (which can
         * be up to 48 bits) and the translation granule size. It indicates
         * the number of strides (stride bits at a time) needed to
         * consume the bits of the input address. In the pseudocode this is:
         *  level = 4 - RoundUp((inputsize - grainsize) / stride)
         * where their 'inputsize' is our 'inputsize', 'grainsize' is
         * our 'stride + 3' and 'stride' is our 'stride'.
         * Applying the usual "rounded up m/n is (m+n-1)/n" and simplifying:
         * = 4 - (inputsize - stride - 3 + stride - 1) / stride
         * = 4 - (inputsize - 4) / stride;
         */
        level = 4 - (inputsize - 4) / stride;
    } else {
        /* For stage 2 translations the starting level is specified by the
         * VTCR_EL2.SL0 field (whose interpretation depends on the page size)
         */
        uint32_t sl0 = extract32(tcr->raw_tcr, 6, 2);
        uint32_t startlevel;
        bool ok;

        if (!aarch64 || stride == 9) {
            /* AArch32 or 4KB pages */
            startlevel = 2 - sl0;
        } else {
            /* 16KB or 64KB pages */
            startlevel = 3 - sl0;
        }

        /* Check that the starting level is valid. */
        ok = check_s2_mmu_setup(cpu, aarch64, startlevel,
                                inputsize, stride);
        if (!ok) {
            fault_type = ARMFault_Translation;
            goto do_fault;
        }
        level = startlevel;
    }

    indexmask_grainsize = (1ULL << (stride + 3)) - 1;
    indexmask = (1ULL << (inputsize - (stride * (4 - level)))) - 1;

    /* Now we can extract the actual base address from the TTBR */
    descaddr = extract64(ttbr, 0, 48);
    descaddr &= ~indexmask;

    /* The address field in the descriptor goes up to bit 39 for ARMv7
     * but up to bit 47 for ARMv8, but we use the descaddrmask
     * up to bit 39 for AArch32, because we don't need other bits in that case
     * to construct next descriptor address (anyway they should be all zeroes).
     */
    descaddrmask = ((1ull << (aarch64 ? 48 : 40)) - 1) &
                   ~indexmask_grainsize;

    /* Secure accesses start with the page table in secure memory and
     * can be downgraded to non-secure at any step. Non-secure accesses
     * remain non-secure. We implement this by just ORing in the NSTable/NS
     * bits at each step.
     */
    tableattrs = regime_is_secure(env, mmu_idx) ? 0 : (1 << 4);
    for (;;) {
        uint64_t descriptor;
        bool nstable;

        descaddr |= (address >> (stride * (4 - level))) & indexmask;
        descaddr &= ~7ULL;
        nstable = extract32(tableattrs, 4, 1);
        descriptor = arm_ldq_ptw(cs, descaddr, !nstable, mmu_idx, fi);
        if (fi->type != ARMFault_None) {
            goto do_fault;
        }

        if (!(descriptor & 1) ||
            (!(descriptor & 2) && (level == 3))) {
            /* Invalid, or the Reserved level 3 encoding */
            goto do_fault;
        }
        descaddr = descriptor & descaddrmask;

        if ((descriptor & 2) && (level < 3)) {
            /* Table entry. The top five bits are attributes which  may
             * propagate down through lower levels of the table (and
             * which are all arranged so that 0 means "no effect", so
             * we can gather them up by ORing in the bits at each level).
             */
            tableattrs |= extract64(descriptor, 59, 5);
            level++;
            indexmask = indexmask_grainsize;
            continue;
        }
        /* Block entry at level 1 or 2, or page entry at level 3.
         * These are basically the same thing, although the number
         * of bits we pull in from the vaddr varies.
         */
        page_size = (1ULL << ((stride * (4 - level)) + 3));
        descaddr |= (address & (page_size - 1));
        /* Extract attributes from the descriptor */
        attrs = extract64(descriptor, 2, 10)
            | (extract64(descriptor, 52, 12) << 10);

        if (mmu_idx == ARMMMUIdx_S2NS) {
            /* Stage 2 table descriptors do not include any attribute fields */
            break;
        }
        /* Merge in attributes from table descriptors */
        attrs |= extract32(tableattrs, 0, 2) << 11; /* XN, PXN */
        attrs |= extract32(tableattrs, 3, 1) << 5; /* APTable[1] => AP[2] */
        /* The sense of AP[1] vs APTable[0] is reversed, as APTable[0] == 1
         * means "force PL1 access only", which means forcing AP[1] to 0.
         */
        if (extract32(tableattrs, 2, 1)) {
            attrs &= ~(1 << 4);
        }
        attrs |= nstable << 3; /* NS */
        break;
    }
    /* Here descaddr is the final physical address, and attributes
     * are all in attrs.
     */
    fault_type = ARMFault_AccessFlag;
    if ((attrs & (1 << 8)) == 0) {
        /* Access flag */
        goto do_fault;
    }

    ap = extract32(attrs, 4, 2);
    xn = extract32(attrs, 12, 1);

    if (mmu_idx == ARMMMUIdx_S2NS) {
        ns = true;
        *prot = get_S2prot(env, ap, xn);
    } else {
        ns = extract32(attrs, 3, 1);
        pxn = extract32(attrs, 11, 1);
        *prot = get_S1prot(env, mmu_idx, aarch64, ap, ns, xn, pxn);
    }

    fault_type = ARMFault_Permission;
    if (!(*prot & (1 << access_type))) {
        goto do_fault;
    }

    if (ns) {
        /* The NS bit will (as required by the architecture) have no effect if
         * the CPU doesn't support TZ or this is a non-secure translation
         * regime, because the attribute will already be non-secure.
         */
        txattrs->secure = false;
    }

    if (cacheattrs != NULL) {
        if (mmu_idx == ARMMMUIdx_S2NS) {
            cacheattrs->attrs = convert_stage2_attrs(env,
                                                     extract32(attrs, 0, 4));
        } else {
            /* Index into MAIR registers for cache attributes */
            uint8_t attrindx = extract32(attrs, 0, 3);
            uint64_t mair = env->cp15.mair_el[regime_el(env, mmu_idx)];
            assert(attrindx <= 7);
            cacheattrs->attrs = extract64(mair, attrindx * 8, 8);
        }
        cacheattrs->shareability = extract32(attrs, 6, 2);
    }

    *phys_ptr = descaddr;
    *page_size_ptr = page_size;
    return false;

do_fault:
    fi->type = fault_type;
    fi->level = level;
    /* Tag the error as S2 for failed S1 PTW at S2 or ordinary S2.  */
    fi->stage2 = fi->s1ptw || (mmu_idx == ARMMMUIdx_S2NS);
    return true;
}

static inline void get_phys_addr_pmsav7_default(CPUARMState *env,
                                                ARMMMUIdx mmu_idx,
                                                int32_t address, int *prot)
{
    if (!arm_feature(env, ARM_FEATURE_M)) {
        *prot = PAGE_READ | PAGE_WRITE;
        switch (address) {
        case 0xF0000000 ... 0xFFFFFFFF:
            if (regime_sctlr(env, mmu_idx) & SCTLR_V) {
                /* hivecs execing is ok */
                *prot |= PAGE_EXEC;
            }
            break;
        case 0x00000000 ... 0x7FFFFFFF:
            *prot |= PAGE_EXEC;
            break;
        }
    } else {
        /* Default system address map for M profile cores.
         * The architecture specifies which regions are execute-never;
         * at the MPU level no other checks are defined.
         */
        switch (address) {
        case 0x00000000 ... 0x1fffffff: /* ROM */
        case 0x20000000 ... 0x3fffffff: /* SRAM */
        case 0x60000000 ... 0x7fffffff: /* RAM */
        case 0x80000000 ... 0x9fffffff: /* RAM */
            *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
            break;
        case 0x40000000 ... 0x5fffffff: /* Peripheral */
        case 0xa0000000 ... 0xbfffffff: /* Device */
        case 0xc0000000 ... 0xdfffffff: /* Device */
        case 0xe0000000 ... 0xffffffff: /* System */
            *prot = PAGE_READ | PAGE_WRITE;
            break;
        default:
            g_assert_not_reached();
        }
    }
}

static bool pmsav7_use_background_region(ARMCPU *cpu,
                                         ARMMMUIdx mmu_idx, bool is_user)
{
    /* Return true if we should use the default memory map as a
     * "background" region if there are no hits against any MPU regions.
     */
    CPUARMState *env = &cpu->env;

    if (is_user) {
        return false;
    }

    if (arm_feature(env, ARM_FEATURE_M)) {
        return env->v7m.mpu_ctrl[regime_is_secure(env, mmu_idx)]
            & R_V7M_MPU_CTRL_PRIVDEFENA_MASK;
    } else {
        return regime_sctlr(env, mmu_idx) & SCTLR_BR;
    }
}

static inline bool m_is_ppb_region(CPUARMState *env, uint32_t address)
{
    /* True if address is in the M profile PPB region 0xe0000000 - 0xe00fffff */
    return arm_feature(env, ARM_FEATURE_M) &&
        extract32(address, 20, 12) == 0xe00;
}

static inline bool m_is_system_region(CPUARMState *env, uint32_t address)
{
    /* True if address is in the M profile system region
     * 0xe0000000 - 0xffffffff
     */
    return arm_feature(env, ARM_FEATURE_M) && extract32(address, 29, 3) == 0x7;
}

static bool get_phys_addr_pmsav7(CPUARMState *env, uint32_t address,
                                 MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                 hwaddr *phys_ptr, int *prot,
                                 target_ulong *page_size,
                                 ARMMMUFaultInfo *fi)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    int n;
    bool is_user = regime_is_user(env, mmu_idx);

    *phys_ptr = address;
    *page_size = TARGET_PAGE_SIZE;
    *prot = 0;

    if (regime_translation_disabled(env, mmu_idx) ||
        m_is_ppb_region(env, address)) {
        /* MPU disabled or M profile PPB access: use default memory map.
         * The other case which uses the default memory map in the
         * v7M ARM ARM pseudocode is exception vector reads from the vector
         * table. In QEMU those accesses are done in arm_v7m_load_vector(),
         * which always does a direct read using address_space_ldl(), rather
         * than going via this function, so we don't need to check that here.
         */
        get_phys_addr_pmsav7_default(env, mmu_idx, address, prot);
    } else { /* MPU enabled */
        for (n = (int)cpu->pmsav7_dregion - 1; n >= 0; n--) {
            /* region search */
            uint32_t base = env->pmsav7.drbar[n];
            uint32_t rsize = extract32(env->pmsav7.drsr[n], 1, 5);
            uint32_t rmask;
            bool srdis = false;

            if (!(env->pmsav7.drsr[n] & 0x1)) {
                continue;
            }

            if (!rsize) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "DRSR[%d]: Rsize field cannot be 0\n", n);
                continue;
            }
            rsize++;
            rmask = (1ull << rsize) - 1;

            if (base & rmask) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "DRBAR[%d]: 0x%" PRIx32 " misaligned "
                              "to DRSR region size, mask = 0x%" PRIx32 "\n",
                              n, base, rmask);
                continue;
            }

            if (address < base || address > base + rmask) {
                /*
                 * Address not in this region. We must check whether the
                 * region covers addresses in the same page as our address.
                 * In that case we must not report a size that covers the
                 * whole page for a subsequent hit against a different MPU
                 * region or the background region, because it would result in
                 * incorrect TLB hits for subsequent accesses to addresses that
                 * are in this MPU region.
                 */
                if (ranges_overlap(base, rmask,
                                   address & TARGET_PAGE_MASK,
                                   TARGET_PAGE_SIZE)) {
                    *page_size = 1;
                }
                continue;
            }

            /* Region matched */

            if (rsize >= 8) { /* no subregions for regions < 256 bytes */
                int i, snd;
                uint32_t srdis_mask;

                rsize -= 3; /* sub region size (power of 2) */
                snd = ((address - base) >> rsize) & 0x7;
                srdis = extract32(env->pmsav7.drsr[n], snd + 8, 1);

                srdis_mask = srdis ? 0x3 : 0x0;
                for (i = 2; i <= 8 && rsize < TARGET_PAGE_BITS; i *= 2) {
                    /* This will check in groups of 2, 4 and then 8, whether
                     * the subregion bits are consistent. rsize is incremented
                     * back up to give the region size, considering consistent
                     * adjacent subregions as one region. Stop testing if rsize
                     * is already big enough for an entire QEMU page.
                     */
                    int snd_rounded = snd & ~(i - 1);
                    uint32_t srdis_multi = extract32(env->pmsav7.drsr[n],
                                                     snd_rounded + 8, i);
                    if (srdis_mask ^ srdis_multi) {
                        break;
                    }
                    srdis_mask = (srdis_mask << i) | srdis_mask;
                    rsize++;
                }
            }
            if (srdis) {
                continue;
            }
            if (rsize < TARGET_PAGE_BITS) {
                *page_size = 1 << rsize;
            }
            break;
        }

        if (n == -1) { /* no hits */
            if (!pmsav7_use_background_region(cpu, mmu_idx, is_user)) {
                /* background fault */
                fi->type = ARMFault_Background;
                return true;
            }
            get_phys_addr_pmsav7_default(env, mmu_idx, address, prot);
        } else { /* a MPU hit! */
            uint32_t ap = extract32(env->pmsav7.dracr[n], 8, 3);
            uint32_t xn = extract32(env->pmsav7.dracr[n], 12, 1);

            if (m_is_system_region(env, address)) {
                /* System space is always execute never */
                xn = 1;
            }

            if (is_user) { /* User mode AP bit decoding */
                switch (ap) {
                case 0:
                case 1:
                case 5:
                    break; /* no access */
                case 3:
                    *prot |= PAGE_WRITE;
                    /* fall through */
                case 2:
                case 6:
                    *prot |= PAGE_READ | PAGE_EXEC;
                    break;
                case 7:
                    /* for v7M, same as 6; for R profile a reserved value */
                    if (arm_feature(env, ARM_FEATURE_M)) {
                        *prot |= PAGE_READ | PAGE_EXEC;
                        break;
                    }
                    /* fall through */
                default:
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "DRACR[%d]: Bad value for AP bits: 0x%"
                                  PRIx32 "\n", n, ap);
                }
            } else { /* Priv. mode AP bits decoding */
                switch (ap) {
                case 0:
                    break; /* no access */
                case 1:
                case 2:
                case 3:
                    *prot |= PAGE_WRITE;
                    /* fall through */
                case 5:
                case 6:
                    *prot |= PAGE_READ | PAGE_EXEC;
                    break;
                case 7:
                    /* for v7M, same as 6; for R profile a reserved value */
                    if (arm_feature(env, ARM_FEATURE_M)) {
                        *prot |= PAGE_READ | PAGE_EXEC;
                        break;
                    }
                    /* fall through */
                default:
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "DRACR[%d]: Bad value for AP bits: 0x%"
                                  PRIx32 "\n", n, ap);
                }
            }

            /* execute never */
            if (xn) {
                *prot &= ~PAGE_EXEC;
            }
        }
    }

    fi->type = ARMFault_Permission;
    fi->level = 1;
    /*
     * Core QEMU code can't handle execution from small pages yet, so
     * don't try it. This way we'll get an MPU exception, rather than
     * eventually causing QEMU to exit in get_page_addr_code().
     */
    if (*page_size < TARGET_PAGE_SIZE && (*prot & PAGE_EXEC)) {
        qemu_log_mask(LOG_UNIMP,
                      "MPU: No support for execution from regions "
                      "smaller than 1K\n");
        *prot &= ~PAGE_EXEC;
    }
    return !(*prot & (1 << access_type));
}

static bool v8m_is_sau_exempt(CPUARMState *env,
                              uint32_t address, MMUAccessType access_type)
{
    /* The architecture specifies that certain address ranges are
     * exempt from v8M SAU/IDAU checks.
     */
    return
        (access_type == MMU_INST_FETCH && m_is_system_region(env, address)) ||
        (address >= 0xe0000000 && address <= 0xe0002fff) ||
        (address >= 0xe000e000 && address <= 0xe000efff) ||
        (address >= 0xe002e000 && address <= 0xe002efff) ||
        (address >= 0xe0040000 && address <= 0xe0041fff) ||
        (address >= 0xe00ff000 && address <= 0xe00fffff);
}

void v8m_security_lookup(CPUARMState *env, uint32_t address,
                         MMUAccessType access_type, ARMMMUIdx mmu_idx,
                         V8M_SAttributes *sattrs)
{
    /* Look up the security attributes for this address. Compare the
     * pseudocode SecurityCheck() function.
     * We assume the caller has zero-initialized *sattrs.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    int r;
    bool idau_exempt = false, idau_ns = true, idau_nsc = true;
    int idau_region = IREGION_NOTVALID;
    uint32_t addr_page_base = address & TARGET_PAGE_MASK;
    uint32_t addr_page_limit = addr_page_base + (TARGET_PAGE_SIZE - 1);

    if (cpu->idau) {
        IDAUInterfaceClass *iic = IDAU_INTERFACE_GET_CLASS(cpu->idau);
        IDAUInterface *ii = IDAU_INTERFACE(cpu->idau);

        iic->check(ii, address, &idau_region, &idau_exempt, &idau_ns,
                   &idau_nsc);
    }

    if (access_type == MMU_INST_FETCH && extract32(address, 28, 4) == 0xf) {
        /* 0xf0000000..0xffffffff is always S for insn fetches */
        return;
    }

    if (idau_exempt || v8m_is_sau_exempt(env, address, access_type)) {
        sattrs->ns = !regime_is_secure(env, mmu_idx);
        return;
    }

    if (idau_region != IREGION_NOTVALID) {
        sattrs->irvalid = true;
        sattrs->iregion = idau_region;
    }

    switch (env->sau.ctrl & 3) {
    case 0: /* SAU.ENABLE == 0, SAU.ALLNS == 0 */
        break;
    case 2: /* SAU.ENABLE == 0, SAU.ALLNS == 1 */
        sattrs->ns = true;
        break;
    default: /* SAU.ENABLE == 1 */
        for (r = 0; r < cpu->sau_sregion; r++) {
            if (env->sau.rlar[r] & 1) {
                uint32_t base = env->sau.rbar[r] & ~0x1f;
                uint32_t limit = env->sau.rlar[r] | 0x1f;

                if (base <= address && limit >= address) {
                    if (base > addr_page_base || limit < addr_page_limit) {
                        sattrs->subpage = true;
                    }
                    if (sattrs->srvalid) {
                        /* If we hit in more than one region then we must report
                         * as Secure, not NS-Callable, with no valid region
                         * number info.
                         */
                        sattrs->ns = false;
                        sattrs->nsc = false;
                        sattrs->sregion = 0;
                        sattrs->srvalid = false;
                        break;
                    } else {
                        if (env->sau.rlar[r] & 2) {
                            sattrs->nsc = true;
                        } else {
                            sattrs->ns = true;
                        }
                        sattrs->srvalid = true;
                        sattrs->sregion = r;
                    }
                } else {
                    /*
                     * Address not in this region. We must check whether the
                     * region covers addresses in the same page as our address.
                     * In that case we must not report a size that covers the
                     * whole page for a subsequent hit against a different MPU
                     * region or the background region, because it would result
                     * in incorrect TLB hits for subsequent accesses to
                     * addresses that are in this MPU region.
                     */
                    if (limit >= base &&
                        ranges_overlap(base, limit - base + 1,
                                       addr_page_base,
                                       TARGET_PAGE_SIZE)) {
                        sattrs->subpage = true;
                    }
                }
            }
        }

        /* The IDAU will override the SAU lookup results if it specifies
         * higher security than the SAU does.
         */
        if (!idau_ns) {
            if (sattrs->ns || (!idau_nsc && sattrs->nsc)) {
                sattrs->ns = false;
                sattrs->nsc = idau_nsc;
            }
        }
        break;
    }
}

bool pmsav8_mpu_lookup(CPUARMState *env, uint32_t address,
                       MMUAccessType access_type, ARMMMUIdx mmu_idx,
                       hwaddr *phys_ptr, MemTxAttrs *txattrs,
                       int *prot, bool *is_subpage,
                       ARMMMUFaultInfo *fi, uint32_t *mregion)
{
    /* Perform a PMSAv8 MPU lookup (without also doing the SAU check
     * that a full phys-to-virt translation does).
     * mregion is (if not NULL) set to the region number which matched,
     * or -1 if no region number is returned (MPU off, address did not
     * hit a region, address hit in multiple regions).
     * We set is_subpage to true if the region hit doesn't cover the
     * entire TARGET_PAGE the address is within.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    bool is_user = regime_is_user(env, mmu_idx);
    uint32_t secure = regime_is_secure(env, mmu_idx);
    int n;
    int matchregion = -1;
    bool hit = false;
    uint32_t addr_page_base = address & TARGET_PAGE_MASK;
    uint32_t addr_page_limit = addr_page_base + (TARGET_PAGE_SIZE - 1);

    *is_subpage = false;
    *phys_ptr = address;
    *prot = 0;
    if (mregion) {
        *mregion = -1;
    }

    /* Unlike the ARM ARM pseudocode, we don't need to check whether this
     * was an exception vector read from the vector table (which is always
     * done using the default system address map), because those accesses
     * are done in arm_v7m_load_vector(), which always does a direct
     * read using address_space_ldl(), rather than going via this function.
     */
    if (regime_translation_disabled(env, mmu_idx)) { /* MPU disabled */
        hit = true;
    } else if (m_is_ppb_region(env, address)) {
        hit = true;
    } else if (pmsav7_use_background_region(cpu, mmu_idx, is_user)) {
        hit = true;
    } else {
        for (n = (int)cpu->pmsav7_dregion - 1; n >= 0; n--) {
            /* region search */
            /* Note that the base address is bits [31:5] from the register
             * with bits [4:0] all zeroes, but the limit address is bits
             * [31:5] from the register with bits [4:0] all ones.
             */
            uint32_t base = env->pmsav8.rbar[secure][n] & ~0x1f;
            uint32_t limit = env->pmsav8.rlar[secure][n] | 0x1f;

            if (!(env->pmsav8.rlar[secure][n] & 0x1)) {
                /* Region disabled */
                continue;
            }

            if (address < base || address > limit) {
                /*
                 * Address not in this region. We must check whether the
                 * region covers addresses in the same page as our address.
                 * In that case we must not report a size that covers the
                 * whole page for a subsequent hit against a different MPU
                 * region or the background region, because it would result in
                 * incorrect TLB hits for subsequent accesses to addresses that
                 * are in this MPU region.
                 */
                if (limit >= base &&
                    ranges_overlap(base, limit - base + 1,
                                   addr_page_base,
                                   TARGET_PAGE_SIZE)) {
                    *is_subpage = true;
                }
                continue;
            }

            if (base > addr_page_base || limit < addr_page_limit) {
                *is_subpage = true;
            }

            if (hit) {
                /* Multiple regions match -- always a failure (unlike
                 * PMSAv7 where highest-numbered-region wins)
                 */
                fi->type = ARMFault_Permission;
                fi->level = 1;
                return true;
            }

            matchregion = n;
            hit = true;
        }
    }

    if (!hit) {
        /* background fault */
        fi->type = ARMFault_Background;
        return true;
    }

    if (matchregion == -1) {
        /* hit using the background region */
        get_phys_addr_pmsav7_default(env, mmu_idx, address, prot);
    } else {
        uint32_t ap = extract32(env->pmsav8.rbar[secure][matchregion], 1, 2);
        uint32_t xn = extract32(env->pmsav8.rbar[secure][matchregion], 0, 1);

        if (m_is_system_region(env, address)) {
            /* System space is always execute never */
            xn = 1;
        }

        *prot = simple_ap_to_rw_prot(env, mmu_idx, ap);
        if (*prot && !xn) {
            *prot |= PAGE_EXEC;
        }
        /* We don't need to look the attribute up in the MAIR0/MAIR1
         * registers because that only tells us about cacheability.
         */
        if (mregion) {
            *mregion = matchregion;
        }
    }

    fi->type = ARMFault_Permission;
    fi->level = 1;
    /*
     * Core QEMU code can't handle execution from small pages yet, so
     * don't try it. This means any attempted execution will generate
     * an MPU exception, rather than eventually causing QEMU to exit in
     * get_page_addr_code().
     */
    if (*is_subpage && (*prot & PAGE_EXEC)) {
        qemu_log_mask(LOG_UNIMP,
                      "MPU: No support for execution from regions "
                      "smaller than 1K\n");
        *prot &= ~PAGE_EXEC;
    }
    return !(*prot & (1 << access_type));
}


static bool get_phys_addr_pmsav8(CPUARMState *env, uint32_t address,
                                 MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                 hwaddr *phys_ptr, MemTxAttrs *txattrs,
                                 int *prot, target_ulong *page_size,
                                 ARMMMUFaultInfo *fi)
{
    uint32_t secure = regime_is_secure(env, mmu_idx);
    V8M_SAttributes sattrs = {};
    bool ret;
    bool mpu_is_subpage;

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        v8m_security_lookup(env, address, access_type, mmu_idx, &sattrs);
        if (access_type == MMU_INST_FETCH) {
            /* Instruction fetches always use the MMU bank and the
             * transaction attribute determined by the fetch address,
             * regardless of CPU state. This is painful for QEMU
             * to handle, because it would mean we need to encode
             * into the mmu_idx not just the (user, negpri) information
             * for the current security state but also that for the
             * other security state, which would balloon the number
             * of mmu_idx values needed alarmingly.
             * Fortunately we can avoid this because it's not actually
             * possible to arbitrarily execute code from memory with
             * the wrong security attribute: it will always generate
             * an exception of some kind or another, apart from the
             * special case of an NS CPU executing an SG instruction
             * in S&NSC memory. So we always just fail the translation
             * here and sort things out in the exception handler
             * (including possibly emulating an SG instruction).
             */
            if (sattrs.ns != !secure) {
                if (sattrs.nsc) {
                    fi->type = ARMFault_QEMU_NSCExec;
                } else {
                    fi->type = ARMFault_QEMU_SFault;
                }
                *page_size = sattrs.subpage ? 1 : TARGET_PAGE_SIZE;
                *phys_ptr = address;
                *prot = 0;
                return true;
            }
        } else {
            /* For data accesses we always use the MMU bank indicated
             * by the current CPU state, but the security attributes
             * might downgrade a secure access to nonsecure.
             */
            if (sattrs.ns) {
                txattrs->secure = false;
            } else if (!secure) {
                /* NS access to S memory must fault.
                 * Architecturally we should first check whether the
                 * MPU information for this address indicates that we
                 * are doing an unaligned access to Device memory, which
                 * should generate a UsageFault instead. QEMU does not
                 * currently check for that kind of unaligned access though.
                 * If we added it we would need to do so as a special case
                 * for M_FAKE_FSR_SFAULT in arm_v7m_cpu_do_interrupt().
                 */
                fi->type = ARMFault_QEMU_SFault;
                *page_size = sattrs.subpage ? 1 : TARGET_PAGE_SIZE;
                *phys_ptr = address;
                *prot = 0;
                return true;
            }
        }
    }

    ret = pmsav8_mpu_lookup(env, address, access_type, mmu_idx, phys_ptr,
                            txattrs, prot, &mpu_is_subpage, fi, NULL);
    /*
     * TODO: this is a temporary hack to ignore the fact that the SAU region
     * is smaller than a page if this is an executable region. We never
     * supported small MPU regions, but we did (accidentally) allow small
     * SAU regions, and if we now made small SAU regions not be executable
     * then this would break previously working guest code. We can't
     * remove this until/unless we implement support for execution from
     * small regions.
     */
    if (*prot & PAGE_EXEC) {
        sattrs.subpage = false;
    }
    *page_size = sattrs.subpage || mpu_is_subpage ? 1 : TARGET_PAGE_SIZE;
    return ret;
}

static bool get_phys_addr_pmsav5(CPUARMState *env, uint32_t address,
                                 MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                 hwaddr *phys_ptr, int *prot,
                                 ARMMMUFaultInfo *fi)
{
    int n;
    uint32_t mask;
    uint32_t base;
    bool is_user = regime_is_user(env, mmu_idx);

    if (regime_translation_disabled(env, mmu_idx)) {
        /* MPU disabled.  */
        *phys_ptr = address;
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return false;
    }

    *phys_ptr = address;
    for (n = 7; n >= 0; n--) {
        base = env->cp15.c6_region[n];
        if ((base & 1) == 0) {
            continue;
        }
        mask = 1 << ((base >> 1) & 0x1f);
        /* Keep this shift separate from the above to avoid an
           (undefined) << 32.  */
        mask = (mask << 1) - 1;
        if (((base ^ address) & ~mask) == 0) {
            break;
        }
    }
    if (n < 0) {
        fi->type = ARMFault_Background;
        return true;
    }

    if (access_type == MMU_INST_FETCH) {
        mask = env->cp15.pmsav5_insn_ap;
    } else {
        mask = env->cp15.pmsav5_data_ap;
    }
    mask = (mask >> (n * 4)) & 0xf;
    switch (mask) {
    case 0:
        fi->type = ARMFault_Permission;
        fi->level = 1;
        return true;
    case 1:
        if (is_user) {
            fi->type = ARMFault_Permission;
            fi->level = 1;
            return true;
        }
        *prot = PAGE_READ | PAGE_WRITE;
        break;
    case 2:
        *prot = PAGE_READ;
        if (!is_user) {
            *prot |= PAGE_WRITE;
        }
        break;
    case 3:
        *prot = PAGE_READ | PAGE_WRITE;
        break;
    case 5:
        if (is_user) {
            fi->type = ARMFault_Permission;
            fi->level = 1;
            return true;
        }
        *prot = PAGE_READ;
        break;
    case 6:
        *prot = PAGE_READ;
        break;
    default:
        /* Bad permission.  */
        fi->type = ARMFault_Permission;
        fi->level = 1;
        return true;
    }
    *prot |= PAGE_EXEC;
    return false;
}

/* Combine either inner or outer cacheability attributes for normal
 * memory, according to table D4-42 and pseudocode procedure
 * CombineS1S2AttrHints() of ARM DDI 0487B.b (the ARMv8 ARM).
 *
 * NB: only stage 1 includes allocation hints (RW bits), leading to
 * some asymmetry.
 */
static uint8_t combine_cacheattr_nibble(uint8_t s1, uint8_t s2)
{
    if (s1 == 4 || s2 == 4) {
        /* non-cacheable has precedence */
        return 4;
    } else if (extract32(s1, 2, 2) == 0 || extract32(s1, 2, 2) == 2) {
        /* stage 1 write-through takes precedence */
        return s1;
    } else if (extract32(s2, 2, 2) == 2) {
        /* stage 2 write-through takes precedence, but the allocation hint
         * is still taken from stage 1
         */
        return (2 << 2) | extract32(s1, 0, 2);
    } else { /* write-back */
        return s1;
    }
}

/* Combine S1 and S2 cacheability/shareability attributes, per D4.5.4
 * and CombineS1S2Desc()
 *
 * @s1:      Attributes from stage 1 walk
 * @s2:      Attributes from stage 2 walk
 */
static ARMCacheAttrs combine_cacheattrs(ARMCacheAttrs s1, ARMCacheAttrs s2)
{
    uint8_t s1lo = extract32(s1.attrs, 0, 4), s2lo = extract32(s2.attrs, 0, 4);
    uint8_t s1hi = extract32(s1.attrs, 4, 4), s2hi = extract32(s2.attrs, 4, 4);
    ARMCacheAttrs ret;

    /* Combine shareability attributes (table D4-43) */
    if (s1.shareability == 2 || s2.shareability == 2) {
        /* if either are outer-shareable, the result is outer-shareable */
        ret.shareability = 2;
    } else if (s1.shareability == 3 || s2.shareability == 3) {
        /* if either are inner-shareable, the result is inner-shareable */
        ret.shareability = 3;
    } else {
        /* both non-shareable */
        ret.shareability = 0;
    }

    /* Combine memory type and cacheability attributes */
    if (s1hi == 0 || s2hi == 0) {
        /* Device has precedence over normal */
        if (s1lo == 0 || s2lo == 0) {
            /* nGnRnE has precedence over anything */
            ret.attrs = 0;
        } else if (s1lo == 4 || s2lo == 4) {
            /* non-Reordering has precedence over Reordering */
            ret.attrs = 4;  /* nGnRE */
        } else if (s1lo == 8 || s2lo == 8) {
            /* non-Gathering has precedence over Gathering */
            ret.attrs = 8;  /* nGRE */
        } else {
            ret.attrs = 0xc; /* GRE */
        }

        /* Any location for which the resultant memory type is any
         * type of Device memory is always treated as Outer Shareable.
         */
        ret.shareability = 2;
    } else { /* Normal memory */
        /* Outer/inner cacheability combine independently */
        ret.attrs = combine_cacheattr_nibble(s1hi, s2hi) << 4
                  | combine_cacheattr_nibble(s1lo, s2lo);

        if (ret.attrs == 0x44) {
            /* Any location for which the resultant memory type is Normal
             * Inner Non-cacheable, Outer Non-cacheable is always treated
             * as Outer Shareable.
             */
            ret.shareability = 2;
        }
    }

    return ret;
}


/* get_phys_addr - get the physical address for this virtual address
 *
 * Find the physical address corresponding to the given virtual address,
 * by doing a translation table walk on MMU based systems or using the
 * MPU state on MPU based systems.
 *
 * Returns false if the translation was successful. Otherwise, phys_ptr, attrs,
 * prot and page_size may not be filled in, and the populated fsr value provides
 * information on why the translation aborted, in the format of a
 * DFSR/IFSR fault register, with the following caveats:
 *  * we honour the short vs long DFSR format differences.
 *  * the WnR bit is never set (the caller must do this).
 *  * for PSMAv5 based systems we don't bother to return a full FSR format
 *    value.
 *
 * @env: CPUARMState
 * @address: virtual address to get physical address for
 * @access_type: 0 for read, 1 for write, 2 for execute
 * @mmu_idx: MMU index indicating required translation regime
 * @phys_ptr: set to the physical address corresponding to the virtual address
 * @attrs: set to the memory transaction attributes to use
 * @prot: set to the permissions for the page containing phys_ptr
 * @page_size: set to the size of the page containing phys_ptr
 * @fi: set to fault info if the translation fails
 * @cacheattrs: (if non-NULL) set to the cacheability/shareability attributes
 */
bool get_phys_addr(CPUARMState *env, target_ulong address,
                   MMUAccessType access_type, ARMMMUIdx mmu_idx,
                   hwaddr *phys_ptr, MemTxAttrs *attrs, int *prot,
                   target_ulong *page_size,
                   ARMMMUFaultInfo *fi, ARMCacheAttrs *cacheattrs)
{
    if (mmu_idx == ARMMMUIdx_S12NSE0 || mmu_idx == ARMMMUIdx_S12NSE1) {
        /* Call ourselves recursively to do the stage 1 and then stage 2
         * translations.
         */
        if (arm_feature(env, ARM_FEATURE_EL2)) {
            hwaddr ipa;
            int s2_prot;
            int ret;
            ARMCacheAttrs cacheattrs2 = {};

            ret = get_phys_addr(env, address, access_type,
                                stage_1_mmu_idx(mmu_idx), &ipa, attrs,
                                prot, page_size, fi, cacheattrs);

            /* If S1 fails or S2 is disabled, return early.  */
            if (ret || regime_translation_disabled(env, ARMMMUIdx_S2NS)) {
                *phys_ptr = ipa;
                return ret;
            }

            /* S1 is done. Now do S2 translation.  */
            ret = get_phys_addr_lpae(env, ipa, access_type, ARMMMUIdx_S2NS,
                                     phys_ptr, attrs, &s2_prot,
                                     page_size, fi,
                                     cacheattrs != NULL ? &cacheattrs2 : NULL);
            fi->s2addr = ipa;
            /* Combine the S1 and S2 perms.  */
            *prot &= s2_prot;

            /* Combine the S1 and S2 cache attributes, if needed */
            if (!ret && cacheattrs != NULL) {
                *cacheattrs = combine_cacheattrs(*cacheattrs, cacheattrs2);
            }

            return ret;
        } else {
            /*
             * For non-EL2 CPUs a stage1+stage2 translation is just stage 1.
             */
            mmu_idx = stage_1_mmu_idx(mmu_idx);
        }
    }

    /* The page table entries may downgrade secure to non-secure, but
     * cannot upgrade an non-secure translation regime's attributes
     * to secure.
     */
    attrs->secure = regime_is_secure(env, mmu_idx);
    attrs->user = regime_is_user(env, mmu_idx);

    /* Fast Context Switch Extension. This doesn't exist at all in v8.
     * In v7 and earlier it affects all stage 1 translations.
     */
    if (address < 0x02000000 && mmu_idx != ARMMMUIdx_S2NS
        && !arm_feature(env, ARM_FEATURE_V8)) {
        if (regime_el(env, mmu_idx) == 3) {
            address += env->cp15.fcseidr_s;
        } else {
            address += env->cp15.fcseidr_ns;
        }
    }

    if (arm_feature(env, ARM_FEATURE_PMSA)) {
        bool ret;
        *page_size = TARGET_PAGE_SIZE;

        if (arm_feature(env, ARM_FEATURE_V8)) {
            /* PMSAv8 */
            ret = get_phys_addr_pmsav8(env, address, access_type, mmu_idx,
                                       phys_ptr, attrs, prot, page_size, fi);
        } else if (arm_feature(env, ARM_FEATURE_V7)) {
            /* PMSAv7 */
            ret = get_phys_addr_pmsav7(env, address, access_type, mmu_idx,
                                       phys_ptr, prot, page_size, fi);
        } else {
            /* Pre-v7 MPU */
            ret = get_phys_addr_pmsav5(env, address, access_type, mmu_idx,
                                       phys_ptr, prot, fi);
        }
        qemu_log_mask(CPU_LOG_MMU, "PMSA MPU lookup for %s at 0x%08" PRIx32
                      " mmu_idx %u -> %s (prot %c%c%c)\n",
                      access_type == MMU_DATA_LOAD ? "reading" :
                      (access_type == MMU_DATA_STORE ? "writing" : "execute"),
                      (uint32_t)address, mmu_idx,
                      ret ? "Miss" : "Hit",
                      *prot & PAGE_READ ? 'r' : '-',
                      *prot & PAGE_WRITE ? 'w' : '-',
                      *prot & PAGE_EXEC ? 'x' : '-');

        return ret;
    }

    /* Definitely a real MMU, not an MPU */

    if (regime_translation_disabled(env, mmu_idx)) {
        /* MMU disabled. */
        *phys_ptr = address;
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        *page_size = TARGET_PAGE_SIZE;
        return 0;
    }

    if (regime_using_lpae_format(env, mmu_idx)) {
        return get_phys_addr_lpae(env, address, access_type, mmu_idx,
                                  phys_ptr, attrs, prot, page_size,
                                  fi, cacheattrs);
    } else if (regime_sctlr(env, mmu_idx) & SCTLR_XP) {
        return get_phys_addr_v6(env, address, access_type, mmu_idx,
                                phys_ptr, attrs, prot, page_size, fi);
    } else {
        return get_phys_addr_v5(env, address, access_type, mmu_idx,
                                    phys_ptr, prot, page_size, fi);
    }
}

hwaddr arm_cpu_get_phys_page_attrs_debug(CPUState *cs, vaddr addr,
                                         MemTxAttrs *attrs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    hwaddr phys_addr;
    target_ulong page_size;
    int prot;
    bool ret;
    ARMMMUFaultInfo fi = {};
    ARMMMUIdx mmu_idx = core_to_arm_mmu_idx(env, cpu_mmu_index(env, false));

    *attrs = (MemTxAttrs) {};

    ret = get_phys_addr(env, addr, 0, mmu_idx, &phys_addr,
                        attrs, &prot, &page_size, &fi, NULL);

    if (ret) {
        return -1;
    }
    return phys_addr;
}




#endif

/* VFP support.  We follow the convention used for VFP instructions:
   Single precision routines have a "s" suffix, double precision a
   "d" suffix.  */

/* Convert host exception flags to vfp form.  */
static inline int vfp_exceptbits_from_host(int host_bits)
{
    int target_bits = 0;

    if (host_bits & float_flag_invalid)
        target_bits |= 1;
    if (host_bits & float_flag_divbyzero)
        target_bits |= 2;
    if (host_bits & float_flag_overflow)
        target_bits |= 4;
    if (host_bits & (float_flag_underflow | float_flag_output_denormal))
        target_bits |= 8;
    if (host_bits & float_flag_inexact)
        target_bits |= 0x10;
    if (host_bits & float_flag_input_denormal)
        target_bits |= 0x80;
    return target_bits;
}

uint32_t HELPER(vfp_get_fpscr)(CPUARMState *env)
{
    int i;
    uint32_t fpscr;

    fpscr = (env->vfp.xregs[ARM_VFP_FPSCR] & 0xffc8ffff)
            | (env->vfp.vec_len << 16)
            | (env->vfp.vec_stride << 20);
    i = get_float_exception_flags(&env->vfp.fp_status);
    i |= get_float_exception_flags(&env->vfp.standard_fp_status);
    i |= get_float_exception_flags(&env->vfp.fp_status_f16);
    fpscr |= vfp_exceptbits_from_host(i);
    return fpscr;
}

uint32_t vfp_get_fpscr(CPUARMState *env)
{
    return HELPER(vfp_get_fpscr)(env);
}

/* Convert vfp exception flags to target form.  */
static inline int vfp_exceptbits_to_host(int target_bits)
{
    int host_bits = 0;

    if (target_bits & 1)
        host_bits |= float_flag_invalid;
    if (target_bits & 2)
        host_bits |= float_flag_divbyzero;
    if (target_bits & 4)
        host_bits |= float_flag_overflow;
    if (target_bits & 8)
        host_bits |= float_flag_underflow;
    if (target_bits & 0x10)
        host_bits |= float_flag_inexact;
    if (target_bits & 0x80)
        host_bits |= float_flag_input_denormal;
    return host_bits;
}

void HELPER(vfp_set_fpscr)(CPUARMState *env, uint32_t val)
{
    int i;
    uint32_t changed;

    changed = env->vfp.xregs[ARM_VFP_FPSCR];
    env->vfp.xregs[ARM_VFP_FPSCR] = (val & 0xffc8ffff);
    env->vfp.vec_len = (val >> 16) & 7;
    env->vfp.vec_stride = (val >> 20) & 3;

    changed ^= val;
    if (changed & (3 << 22)) {
        i = (val >> 22) & 3;
        switch (i) {
        case FPROUNDING_TIEEVEN:
            i = float_round_nearest_even;
            break;
        case FPROUNDING_POSINF:
            i = float_round_up;
            break;
        case FPROUNDING_NEGINF:
            i = float_round_down;
            break;
        case FPROUNDING_ZERO:
            i = float_round_to_zero;
            break;
        }
        set_float_rounding_mode(i, &env->vfp.fp_status);
        set_float_rounding_mode(i, &env->vfp.fp_status_f16);
    }
    if (changed & FPCR_FZ16) {
        bool ftz_enabled = val & FPCR_FZ16;
        set_flush_to_zero(ftz_enabled, &env->vfp.fp_status_f16);
        set_flush_inputs_to_zero(ftz_enabled, &env->vfp.fp_status_f16);
    }
    if (changed & FPCR_FZ) {
        bool ftz_enabled = val & FPCR_FZ;
        set_flush_to_zero(ftz_enabled, &env->vfp.fp_status);
        set_flush_inputs_to_zero(ftz_enabled, &env->vfp.fp_status);
    }
    if (changed & FPCR_DN) {
        bool dnan_enabled = val & FPCR_DN;
        set_default_nan_mode(dnan_enabled, &env->vfp.fp_status);
        set_default_nan_mode(dnan_enabled, &env->vfp.fp_status_f16);
    }

    /* The exception flags are ORed together when we read fpscr so we
     * only need to preserve the current state in one of our
     * float_status values.
     */
    i = vfp_exceptbits_to_host(val);
    set_float_exception_flags(i, &env->vfp.fp_status);
    set_float_exception_flags(0, &env->vfp.fp_status_f16);
    set_float_exception_flags(0, &env->vfp.standard_fp_status);
}

void vfp_set_fpscr(CPUARMState *env, uint32_t val)
{
    HELPER(vfp_set_fpscr)(env, val);
}


vaddr arm_adjust_watchpoint_address(CPUState *cs, vaddr addr, int len)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    /* In BE32 system mode, target memory is stored byteswapped (on a
     * little-endian host system), and by the time we reach here (via an
     * opcode helper) the addresses of subword accesses have been adjusted
     * to account for that, which means that watchpoints will not match.
     * Undo the adjustment here.
     */
    if (arm_sctlr_b(env)) {
        if (len == 1) {
            addr ^= 3;
        } else if (len == 2) {
            addr ^= 2;
        }
    }

    return addr;
}

static void aarch64_cpu_dump_state(CPUState *cs, FILE *f,
                            fprintf_function cpu_fprintf, int flags)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    uint32_t psr = pstate_read(env);
    int i;
    int el = arm_current_el(env);
    const char *ns_status;

    cpu_fprintf(f, "PC=%016"PRIx64"  SP=%016"PRIx64"\n",
            env->pc, env->xregs[31]);
    for (i = 0; i < 31; i++) {
        cpu_fprintf(f, "X%02d=%016"PRIx64, i, env->xregs[i]);
        if ((i % 4) == 3) {
            cpu_fprintf(f, "\n");
        } else {
            cpu_fprintf(f, " ");
        }
    }

    if (arm_feature(env, ARM_FEATURE_EL3) && el != 3) {
        ns_status = env->cp15.scr_el3 & SCR_NS ? "NS " : "S ";
    } else {
        ns_status = "";
    }

    cpu_fprintf(f, "\nPSTATE=%08x %c%c%c%c %sEL%d%c\n",
                psr,
                psr & PSTATE_N ? 'N' : '-',
                psr & PSTATE_Z ? 'Z' : '-',
                psr & PSTATE_C ? 'C' : '-',
                psr & PSTATE_V ? 'V' : '-',
                ns_status,
                el,
                psr & PSTATE_SP ? 'h' : 't');

    if (flags & CPU_DUMP_FPU) {
        int numvfpregs = 32;
        for (i = 0; i < numvfpregs; i++) {
            uint64_t *q = aa64_vfp_qreg(env, i);
            uint64_t vlo = q[0];
            uint64_t vhi = q[1];
            cpu_fprintf(f, "q%02d=%016" PRIx64 ":%016" PRIx64 "%c",
                        i, vhi, vlo, (i & 1 ? '\n' : ' '));
        }
        cpu_fprintf(f, "FPCR: %08x  FPSR: %08x\n",
                    vfp_get_fpcr(env), vfp_get_fpsr(env));
    }
}

static const char *cpu_mode_names[16] = {
  "usr", "fiq", "irq", "svc", "???", "???", "mon", "abt",
  "???", "???", "hyp", "und", "???", "???", "???", "sys"
};

void arm_cpu_dump_state(CPUState *cs, FILE *f, fprintf_function cpu_fprintf,
                        int flags)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    int i;

    if (is_a64(env)) {
        aarch64_cpu_dump_state(cs, f, cpu_fprintf, flags);
        return;
    }

    for(i=0;i<16;i++) {
        cpu_fprintf(f, "R%02d=%08x", i, env->regs[i]);
        if ((i % 4) == 3)
            cpu_fprintf(f, "\n");
        else
            cpu_fprintf(f, " ");
    }

    if (arm_feature(env, ARM_FEATURE_M)) {
        uint32_t xpsr = xpsr_read(env);
        const char *mode;
        const char *ns_status = "";

        if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
            ns_status = env->v7m.secure ? "S " : "NS ";
        }

        if (xpsr & XPSR_EXCP) {
            mode = "handler";
        } else {
            if (env->v7m.control[env->v7m.secure] & R_V7M_CONTROL_NPRIV_MASK) {
                mode = "unpriv-thread";
            } else {
                mode = "priv-thread";
            }
        }

        cpu_fprintf(f, "XPSR=%08x %c%c%c%c %c %s%s\n",
                    xpsr,
                    xpsr & XPSR_N ? 'N' : '-',
                    xpsr & XPSR_Z ? 'Z' : '-',
                    xpsr & XPSR_C ? 'C' : '-',
                    xpsr & XPSR_V ? 'V' : '-',
                    xpsr & XPSR_T ? 'T' : 'A',
                    ns_status,
                    mode);
    } else {
        uint32_t psr = cpsr_read(env);
        const char *ns_status = "";

        if (arm_feature(env, ARM_FEATURE_EL3) &&
            (psr & CPSR_M) != ARM_CPU_MODE_MON) {
            ns_status = env->cp15.scr_el3 & SCR_NS ? "NS " : "S ";
        }

        cpu_fprintf(f, "PSR=%08x %c%c%c%c %c %s%s%d\n",
                    psr,
                    psr & CPSR_N ? 'N' : '-',
                    psr & CPSR_Z ? 'Z' : '-',
                    psr & CPSR_C ? 'C' : '-',
                    psr & CPSR_V ? 'V' : '-',
                    psr & CPSR_T ? 'T' : 'A',
                    ns_status,
                    cpu_mode_names[psr & 0xf], (psr & 0x10) ? 32 : 26);
    }

    if (flags & CPU_DUMP_FPU) {
        int numvfpregs = 0;
        if (arm_feature(env, ARM_FEATURE_VFP)) {
            numvfpregs += 16;
        }
        if (arm_feature(env, ARM_FEATURE_VFP3)) {
            numvfpregs += 16;
        }
        for (i = 0; i < numvfpregs; i++) {
            uint64_t v = *aa32_vfp_dreg(env, i);
            cpu_fprintf(f, "s%02d=%08x s%02d=%08x d%02d=%016" PRIx64 "\n",
                        i * 2, (uint32_t)v,
                        i * 2 + 1, (uint32_t)(v >> 32),
                        i, v);
        }
        cpu_fprintf(f, "FPSCR: %08x\n", (int)env->vfp.xregs[ARM_VFP_FPSCR]);
    }
}

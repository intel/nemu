/*
 * Exception and interrupt helpers.
 *
 * This code is licensed under the GNU GPL v2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "trace.h"
#include "cpu.h"
#include "internals.h"
#include "sysemu/sysemu.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "arm_ldst.h"
#include "exec/semihost.h"
#include "sysemu/kvm.h"

static void take_aarch32_exception(CPUARMState *env, int new_mode,
                                   uint32_t mask, uint32_t offset,
                                   uint32_t newpc)
{
    /* Change the CPU state so as to actually take the exception. */
    switch_mode(env, new_mode);
    /*
     * For exceptions taken to AArch32 we must clear the SS bit in both
     * PSTATE and in the old-state value we save to SPSR_<mode>, so zero it now.
     */
    env->uncached_cpsr &= ~PSTATE_SS;
    env->spsr = cpsr_read(env);
    /* Clear IT bits.  */
    env->condexec_bits = 0;
    /* Switch to the new mode, and to the correct instruction set.  */
    env->uncached_cpsr = (env->uncached_cpsr & ~CPSR_M) | new_mode;
    /* Set new mode endianness */
    env->uncached_cpsr &= ~CPSR_E;
    if (env->cp15.sctlr_el[arm_current_el(env)] & SCTLR_EE) {
        env->uncached_cpsr |= CPSR_E;
    }
    /* J and IL must always be cleared for exception entry */
    env->uncached_cpsr &= ~(CPSR_IL | CPSR_J);
    env->daif |= mask;

    if (new_mode == ARM_CPU_MODE_HYP) {
        env->thumb = (env->cp15.sctlr_el[2] & SCTLR_TE) != 0;
        env->elr_el[2] = env->regs[15];
    } else {
        /*
         * this is a lie, as there was no c1_sys on V4T/V5, but who cares
         * and we should just guard the thumb mode on V4
         */
        if (arm_feature(env, ARM_FEATURE_V4T)) {
            env->thumb =
                (A32_BANKED_CURRENT_REG_GET(env, sctlr) & SCTLR_TE) != 0;
        }
        env->regs[14] = env->regs[15] + offset;
    }
    env->regs[15] = newpc;
}

static void arm_cpu_do_interrupt_aarch32_hyp(CPUState *cs)
{
    /*
     * Handle exception entry to Hyp mode; this is sufficiently
     * different to entry to other AArch32 modes that we handle it
     * separately here.
     *
     * The vector table entry used is always the 0x14 Hyp mode entry point,
     * unless this is an UNDEF/HVC/abort taken from Hyp to Hyp.
     * The offset applied to the preferred return address is always zero
     * (see DDI0487C.a section G1.12.3).
     * PSTATE A/I/F masks are set based only on the SCR.EA/IRQ/FIQ values.
     */
    uint32_t addr, mask;
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    switch (cs->exception_index) {
    case EXCP_UDEF:
        addr = 0x04;
        break;
    case EXCP_SWI:
        addr = 0x14;
        break;
    case EXCP_BKPT:
        /* Fall through to prefetch abort.  */
    case EXCP_PREFETCH_ABORT:
        env->cp15.ifar_s = env->exception.vaddress;
        qemu_log_mask(CPU_LOG_INT, "...with HIFAR 0x%x\n",
                      (uint32_t)env->exception.vaddress);
        addr = 0x0c;
        break;
    case EXCP_DATA_ABORT:
        env->cp15.dfar_s = env->exception.vaddress;
        qemu_log_mask(CPU_LOG_INT, "...with HDFAR 0x%x\n",
                      (uint32_t)env->exception.vaddress);
        addr = 0x10;
        break;
    case EXCP_IRQ:
        addr = 0x18;
        break;
    case EXCP_FIQ:
        addr = 0x1c;
        break;
    case EXCP_HVC:
        addr = 0x08;
        break;
    case EXCP_HYP_TRAP:
        addr = 0x14;
    default:
        cpu_abort(cs, "Unhandled exception 0x%x\n", cs->exception_index);
    }

    if (cs->exception_index != EXCP_IRQ && cs->exception_index != EXCP_FIQ) {
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            /*
             * QEMU syndrome values are v8-style. v7 has the IL bit
             * UNK/SBZP for "field not valid" cases, where v8 uses RES1.
             * If this is a v7 CPU, squash the IL bit in those cases.
             */
            if (cs->exception_index == EXCP_PREFETCH_ABORT ||
                (cs->exception_index == EXCP_DATA_ABORT &&
                 !(env->exception.syndrome & ARM_EL_ISV)) ||
                syn_get_ec(env->exception.syndrome) == EC_UNCATEGORIZED) {
                env->exception.syndrome &= ~ARM_EL_IL;
            }
        }
        env->cp15.esr_el[2] = env->exception.syndrome;
    }

    if (arm_current_el(env) != 2 && addr < 0x14) {
        addr = 0x14;
    }

    mask = 0;
    if (!(env->cp15.scr_el3 & SCR_EA)) {
        mask |= CPSR_A;
    }
    if (!(env->cp15.scr_el3 & SCR_IRQ)) {
        mask |= CPSR_I;
    }
    if (!(env->cp15.scr_el3 & SCR_FIQ)) {
        mask |= CPSR_F;
    }

    addr += env->cp15.hvbar;

    take_aarch32_exception(env, ARM_CPU_MODE_HYP, mask, 0, addr);
}

static void arm_cpu_do_interrupt_aarch32(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    uint32_t addr;
    uint32_t mask;
    int new_mode;
    uint32_t offset;
    uint32_t moe;

    /* If this is a debug exception we must update the DBGDSCR.MOE bits */
    switch (syn_get_ec(env->exception.syndrome)) {
    case EC_BREAKPOINT:
    case EC_BREAKPOINT_SAME_EL:
        moe = 1;
        break;
    case EC_WATCHPOINT:
    case EC_WATCHPOINT_SAME_EL:
        moe = 10;
        break;
    case EC_AA32_BKPT:
        moe = 3;
        break;
    case EC_VECTORCATCH:
        moe = 5;
        break;
    default:
        moe = 0;
        break;
    }

    if (moe) {
        env->cp15.mdscr_el1 = deposit64(env->cp15.mdscr_el1, 2, 4, moe);
    }

    if (env->exception.target_el == 2) {
        arm_cpu_do_interrupt_aarch32_hyp(cs);
        return;
    }

    switch (cs->exception_index) {
    case EXCP_UDEF:
        new_mode = ARM_CPU_MODE_UND;
        addr = 0x04;
        mask = CPSR_I;
        if (env->thumb)
            offset = 2;
        else
            offset = 4;
        break;
    case EXCP_SWI:
        new_mode = ARM_CPU_MODE_SVC;
        addr = 0x08;
        mask = CPSR_I;
        /* The PC already points to the next instruction.  */
        offset = 0;
        break;
    case EXCP_BKPT:
        /* Fall through to prefetch abort.  */
    case EXCP_PREFETCH_ABORT:
        A32_BANKED_CURRENT_REG_SET(env, ifsr, env->exception.fsr);
        A32_BANKED_CURRENT_REG_SET(env, ifar, env->exception.vaddress);
        qemu_log_mask(CPU_LOG_INT, "...with IFSR 0x%x IFAR 0x%x\n",
                      env->exception.fsr, (uint32_t)env->exception.vaddress);
        new_mode = ARM_CPU_MODE_ABT;
        addr = 0x0c;
        mask = CPSR_A | CPSR_I;
        offset = 4;
        break;
    case EXCP_DATA_ABORT:
        A32_BANKED_CURRENT_REG_SET(env, dfsr, env->exception.fsr);
        A32_BANKED_CURRENT_REG_SET(env, dfar, env->exception.vaddress);
        qemu_log_mask(CPU_LOG_INT, "...with DFSR 0x%x DFAR 0x%x\n",
                      env->exception.fsr,
                      (uint32_t)env->exception.vaddress);
        new_mode = ARM_CPU_MODE_ABT;
        addr = 0x10;
        mask = CPSR_A | CPSR_I;
        offset = 8;
        break;
    case EXCP_IRQ:
        new_mode = ARM_CPU_MODE_IRQ;
        addr = 0x18;
        /* Disable IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I;
        offset = 4;
        if (env->cp15.scr_el3 & SCR_IRQ) {
            /* IRQ routed to monitor mode */
            new_mode = ARM_CPU_MODE_MON;
            mask |= CPSR_F;
        }
        break;
    case EXCP_FIQ:
        new_mode = ARM_CPU_MODE_FIQ;
        addr = 0x1c;
        /* Disable FIQ, IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I | CPSR_F;
        if (env->cp15.scr_el3 & SCR_FIQ) {
            /* FIQ routed to monitor mode */
            new_mode = ARM_CPU_MODE_MON;
        }
        offset = 4;
        break;
    case EXCP_VIRQ:
        new_mode = ARM_CPU_MODE_IRQ;
        addr = 0x18;
        /* Disable IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I;
        offset = 4;
        break;
    case EXCP_VFIQ:
        new_mode = ARM_CPU_MODE_FIQ;
        addr = 0x1c;
        /* Disable FIQ, IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I | CPSR_F;
        offset = 4;
        break;
    case EXCP_SMC:
        new_mode = ARM_CPU_MODE_MON;
        addr = 0x08;
        mask = CPSR_A | CPSR_I | CPSR_F;
        offset = 0;
        break;
    default:
        cpu_abort(cs, "Unhandled exception 0x%x\n", cs->exception_index);
        return; /* Never happens.  Keep compiler happy.  */
    }

    if (new_mode == ARM_CPU_MODE_MON) {
        addr += env->cp15.mvbar;
    } else if (A32_BANKED_CURRENT_REG_GET(env, sctlr) & SCTLR_V) {
        /* High vectors. When enabled, base address cannot be remapped. */
        addr += 0xffff0000;
    } else {
        /* ARM v7 architectures provide a vector base address register to remap
         * the interrupt vector table.
         * This register is only followed in non-monitor mode, and is banked.
         * Note: only bits 31:5 are valid.
         */
        addr += A32_BANKED_CURRENT_REG_GET(env, vbar);
    }

    if ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_MON) {
        env->cp15.scr_el3 &= ~SCR_NS;
    }

    take_aarch32_exception(env, new_mode, mask, offset, addr);
}

/* Handle exception entry to a target EL which is using AArch64 */
static void arm_cpu_do_interrupt_aarch64(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    unsigned int new_el = env->exception.target_el;
    target_ulong addr = env->cp15.vbar_el[new_el];
    unsigned int new_mode = aarch64_pstate_mode(new_el, true);
    unsigned int cur_el = arm_current_el(env);

    /*
     * Note that new_el can never be 0.  If cur_el is 0, then
     * el0_a64 is is_a64(), else el0_a64 is ignored.
     */
    aarch64_sve_change_el(env, cur_el, new_el, is_a64(env));

    if (cur_el < new_el) {
        /* Entry vector offset depends on whether the implemented EL
         * immediately lower than the target level is using AArch32 or AArch64
         */
        bool is_aa64;

        switch (new_el) {
        case 3:
            is_aa64 = (env->cp15.scr_el3 & SCR_RW) != 0;
            break;
        case 2:
            is_aa64 = (env->cp15.hcr_el2 & HCR_RW) != 0;
            break;
        case 1:
            is_aa64 = is_a64(env);
            break;
        default:
            g_assert_not_reached();
        }

        if (is_aa64) {
            addr += 0x400;
        } else {
            addr += 0x600;
        }
    } else if (pstate_read(env) & PSTATE_SP) {
        addr += 0x200;
    }

    switch (cs->exception_index) {
    case EXCP_PREFETCH_ABORT:
    case EXCP_DATA_ABORT:
        env->cp15.far_el[new_el] = env->exception.vaddress;
        qemu_log_mask(CPU_LOG_INT, "...with FAR 0x%" PRIx64 "\n",
                      env->cp15.far_el[new_el]);
        /* fall through */
    case EXCP_BKPT:
    case EXCP_UDEF:
    case EXCP_SWI:
    case EXCP_HVC:
    case EXCP_HYP_TRAP:
    case EXCP_SMC:
        if (syn_get_ec(env->exception.syndrome) == EC_ADVSIMDFPACCESSTRAP) {
            /*
             * QEMU internal FP/SIMD syndromes from AArch32 include the
             * TA and coproc fields which are only exposed if the exception
             * is taken to AArch32 Hyp mode. Mask them out to get a valid
             * AArch64 format syndrome.
             */
            env->exception.syndrome &= ~MAKE_64BIT_MASK(0, 20);
        }
        env->cp15.esr_el[new_el] = env->exception.syndrome;
        break;
    case EXCP_IRQ:
    case EXCP_VIRQ:
        addr += 0x80;
        break;
    case EXCP_FIQ:
    case EXCP_VFIQ:
        addr += 0x100;
        break;
    case EXCP_SEMIHOST:
        qemu_log_mask(CPU_LOG_INT,
                      "...handling as semihosting call 0x%" PRIx64 "\n",
                      env->xregs[0]);
        env->xregs[0] = do_arm_semihosting(env);
        return;
    default:
        cpu_abort(cs, "Unhandled exception 0x%x\n", cs->exception_index);
    }

    if (is_a64(env)) {
        env->banked_spsr[aarch64_banked_spsr_index(new_el)] = pstate_read(env);
        aarch64_save_sp(env, arm_current_el(env));
        env->elr_el[new_el] = env->pc;
    } else {
        env->banked_spsr[aarch64_banked_spsr_index(new_el)] = cpsr_read(env);
        env->elr_el[new_el] = env->regs[15];

        aarch64_sync_32_to_64(env);

        env->condexec_bits = 0;
    }
    qemu_log_mask(CPU_LOG_INT, "...with ELR 0x%" PRIx64 "\n",
                  env->elr_el[new_el]);

    pstate_write(env, PSTATE_DAIF | new_mode);
    env->aarch64 = 1;
    aarch64_restore_sp(env, new_el);

    env->pc = addr;

    qemu_log_mask(CPU_LOG_INT, "...to EL%d PC 0x%" PRIx64 " PSTATE 0x%x\n",
                  new_el, env->pc, pstate_read(env));
}

static inline bool check_for_semihosting(CPUState *cs)
{
    /* Check whether this exception is a semihosting call; if so
     * then handle it and return true; otherwise return false.
     */
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    if (is_a64(env)) {
        if (cs->exception_index == EXCP_SEMIHOST) {
            /* This is always the 64-bit semihosting exception.
             * The "is this usermode" and "is semihosting enabled"
             * checks have been done at translate time.
             */
            qemu_log_mask(CPU_LOG_INT,
                          "...handling as semihosting call 0x%" PRIx64 "\n",
                          env->xregs[0]);
            env->xregs[0] = do_arm_semihosting(env);
            return true;
        }
        return false;
    } else {
        uint32_t imm;

        /* Only intercept calls from privileged modes, to provide some
         * semblance of security.
         */
        if (cs->exception_index != EXCP_SEMIHOST &&
            (!semihosting_enabled() ||
             ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_USR))) {
            return false;
        }

        switch (cs->exception_index) {
        case EXCP_SEMIHOST:
            /* This is always a semihosting call; the "is this usermode"
             * and "is semihosting enabled" checks have been done at
             * translate time.
             */
            break;
        case EXCP_SWI:
            /* Check for semihosting interrupt.  */
            if (env->thumb) {
                imm = arm_lduw_code(env, env->regs[15] - 2, arm_sctlr_b(env))
                    & 0xff;
                if (imm == 0xab) {
                    break;
                }
            } else {
                imm = arm_ldl_code(env, env->regs[15] - 4, arm_sctlr_b(env))
                    & 0xffffff;
                if (imm == 0x123456) {
                    break;
                }
            }
            return false;
        case EXCP_BKPT:
            /* See if this is a semihosting syscall.  */
            if (env->thumb) {
                imm = arm_lduw_code(env, env->regs[15], arm_sctlr_b(env))
                    & 0xff;
                if (imm == 0xab) {
                    env->regs[15] += 2;
                    break;
                }
            }
            return false;
        default:
            return false;
        }

        qemu_log_mask(CPU_LOG_INT,
                      "...handling as semihosting call 0x%x\n",
                      env->regs[0]);
        env->regs[0] = do_arm_semihosting(env);
        return true;
    }
}

/* Handle a CPU exception for A and R profile CPUs.
 * Do any appropriate logging, handle PSCI calls, and then hand off
 * to the AArch64-entry or AArch32-entry function depending on the
 * target exception level's register width.
 */
void arm_cpu_do_interrupt(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    unsigned int new_el = env->exception.target_el;

    assert(!arm_feature(env, ARM_FEATURE_M));

    arm_log_exception(cs->exception_index);
    qemu_log_mask(CPU_LOG_INT, "...from EL%d to EL%d\n", arm_current_el(env),
                  new_el);
    if (qemu_loglevel_mask(CPU_LOG_INT)
        && !excp_is_internal(cs->exception_index)) {
        qemu_log_mask(CPU_LOG_INT, "...with ESR 0x%x/0x%" PRIx32 "\n",
                      syn_get_ec(env->exception.syndrome),
                      env->exception.syndrome);
    }

    if (arm_is_psci_call(cpu, cs->exception_index)) {
        arm_handle_psci_call(cpu);
        qemu_log_mask(CPU_LOG_INT, "...handled as PSCI call\n");
        return;
    }

    /* Semihosting semantics depend on the register width of the
     * code that caused the exception, not the target exception level,
     * so must be handled here.
     */
    if (check_for_semihosting(cs)) {
        return;
    }

    /* Hooks may change global state so BQL should be held, also the
     * BQL needs to be held for any modification of
     * cs->interrupt_request.
     */
    g_assert(qemu_mutex_iothread_locked());

    arm_call_pre_el_change_hook(cpu);

    assert(!excp_is_internal(cs->exception_index));
    if (arm_el_is_aa64(env, new_el)) {
        arm_cpu_do_interrupt_aarch64(cs);
    } else {
        arm_cpu_do_interrupt_aarch32(cs);
    }

    arm_call_el_change_hook(cpu);

    if (!kvm_enabled()) {
        cs->interrupt_request |= CPU_INTERRUPT_EXITTB;
    }
}

/*
 * ARM M profile helpers.
 *
 * This code is licensed under the GNU GPL v2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/helper-proto.h"
#include "exec/exec-all.h"
#include "arm_ldst.h"
#include "exec/semihost.h"
#include "fpu/softfloat.h"

#if defined(CONFIG_USER_ONLY)

/* These should probably raise undefined insn exceptions.  */
void HELPER(v7m_msr)(CPUARMState *env, uint32_t reg, uint32_t val)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    cpu_abort(CPU(cpu), "v7m_msr %d\n", reg);
}

uint32_t HELPER(v7m_mrs)(CPUARMState *env, uint32_t reg)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    cpu_abort(CPU(cpu), "v7m_mrs %d\n", reg);
    return 0;
}

void HELPER(v7m_bxns)(CPUARMState *env, uint32_t dest)
{
    /* translate.c should never generate calls here in user-only mode */
    g_assert_not_reached();
}

void HELPER(v7m_blxns)(CPUARMState *env, uint32_t dest)
{
    /* translate.c should never generate calls here in user-only mode */
    g_assert_not_reached();
}

uint32_t HELPER(v7m_tt)(CPUARMState *env, uint32_t addr, uint32_t op)
{
    /* The TT instructions can be used by unprivileged code, but in
     * user-only emulation we don't have the MPU.
     * Luckily since we know we are NonSecure unprivileged (and that in
     * turn means that the A flag wasn't specified), all the bits in the
     * register must be zero:
     *  IREGION: 0 because IRVALID is 0
     *  IRVALID: 0 because NS
     *  S: 0 because NS
     *  NSRW: 0 because NS
     *  NSR: 0 because NS
     *  RW: 0 because unpriv and A flag not set
     *  R: 0 because unpriv and A flag not set
     *  SRVALID: 0 because NS
     *  MRVALID: 0 because unpriv and A flag not set
     *  SREGION: 0 becaus SRVALID is 0
     *  MREGION: 0 because MRVALID is 0
     */
    return 0;
}

#else

void HELPER(v7m_bxns)(CPUARMState *env, uint32_t dest)
{
    /* Handle v7M BXNS:
     *  - if the return value is a magic value, do exception return (like BX)
     *  - otherwise bit 0 of the return value is the target security state
     */
    uint32_t min_magic;

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        /* Covers FNC_RETURN and EXC_RETURN magic */
        min_magic = FNC_RETURN_MIN_MAGIC;
    } else {
        /* EXC_RETURN magic only */
        min_magic = EXC_RETURN_MIN_MAGIC;
    }

    if (dest >= min_magic) {
        /* This is an exception return magic value; put it where
         * do_v7m_exception_exit() expects and raise EXCEPTION_EXIT.
         * Note that if we ever add gen_ss_advance() singlestep support to
         * M profile this should count as an "instruction execution complete"
         * event (compare gen_bx_excret_final_code()).
         */
        env->regs[15] = dest & ~1;
        env->thumb = dest & 1;
        HELPER(exception_internal)(env, EXCP_EXCEPTION_EXIT);
        /* notreached */
    }

    /* translate.c should have made BXNS UNDEF unless we're secure */
    assert(env->v7m.secure);

    switch_v7m_security_state(env, dest & 1);
    env->thumb = 1;
    env->regs[15] = dest & ~1;
}

void HELPER(v7m_blxns)(CPUARMState *env, uint32_t dest)
{
    /* Handle v7M BLXNS:
     *  - bit 0 of the destination address is the target security state
     */

    /* At this point regs[15] is the address just after the BLXNS */
    uint32_t nextinst = env->regs[15] | 1;
    uint32_t sp = env->regs[13] - 8;
    uint32_t saved_psr;

    /* translate.c will have made BLXNS UNDEF unless we're secure */
    assert(env->v7m.secure);

    if (dest & 1) {
        /* target is Secure, so this is just a normal BLX,
         * except that the low bit doesn't indicate Thumb/not.
         */
        env->regs[14] = nextinst;
        env->thumb = 1;
        env->regs[15] = dest & ~1;
        return;
    }

    /* Target is non-secure: first push a stack frame */
    if (!QEMU_IS_ALIGNED(sp, 8)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BLXNS with misaligned SP is UNPREDICTABLE\n");
    }

    if (sp < v7m_sp_limit(env)) {
        raise_exception(env, EXCP_STKOF, 0, 1);
    }

    saved_psr = env->v7m.exception;
    if (env->v7m.control[M_REG_S] & R_V7M_CONTROL_SFPA_MASK) {
        saved_psr |= XPSR_SFPA;
    }

    /* Note that these stores can throw exceptions on MPU faults */
    cpu_stl_data(env, sp, nextinst);
    cpu_stl_data(env, sp + 4, saved_psr);

    env->regs[13] = sp;
    env->regs[14] = 0xfeffffff;
    if (arm_v7m_is_handler_mode(env)) {
        /* Write a dummy value to IPSR, to avoid leaking the current secure
         * exception number to non-secure code. This is guaranteed not
         * to cause write_v7m_exception() to actually change stacks.
         */
        write_v7m_exception(env, 1);
    }
    switch_v7m_security_state(env, 0);
    env->thumb = 1;
    env->regs[15] = dest;
}

void arm_log_exception(int idx)
{
    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        const char *exc = NULL;
        static const char * const excnames[] = {
            [EXCP_UDEF] = "Undefined Instruction",
            [EXCP_SWI] = "SVC",
            [EXCP_PREFETCH_ABORT] = "Prefetch Abort",
            [EXCP_DATA_ABORT] = "Data Abort",
            [EXCP_IRQ] = "IRQ",
            [EXCP_FIQ] = "FIQ",
            [EXCP_BKPT] = "Breakpoint",
            [EXCP_EXCEPTION_EXIT] = "QEMU v7M exception exit",
            [EXCP_KERNEL_TRAP] = "QEMU intercept of kernel commpage",
            [EXCP_HVC] = "Hypervisor Call",
            [EXCP_HYP_TRAP] = "Hypervisor Trap",
            [EXCP_SMC] = "Secure Monitor Call",
            [EXCP_VIRQ] = "Virtual IRQ",
            [EXCP_VFIQ] = "Virtual FIQ",
            [EXCP_SEMIHOST] = "Semihosting call",
            [EXCP_NOCP] = "v7M NOCP UsageFault",
            [EXCP_INVSTATE] = "v7M INVSTATE UsageFault",
            [EXCP_STKOF] = "v8M STKOF UsageFault",
        };

        if (idx >= 0 && idx < ARRAY_SIZE(excnames)) {
            exc = excnames[idx];
        }
        if (!exc) {
            exc = "unknown";
        }
        qemu_log_mask(CPU_LOG_INT, "Taking exception %d [%s]\n", idx, exc);
    }
}

uint32_t HELPER(v7m_mrs)(CPUARMState *env, uint32_t reg)
{
    uint32_t mask;
    unsigned el = arm_current_el(env);

    /* First handle registers which unprivileged can read */

    switch (reg) {
    case 0 ... 7: /* xPSR sub-fields */
        mask = 0;
        if ((reg & 1) && el) {
            mask |= XPSR_EXCP; /* IPSR (unpriv. reads as zero) */
        }
        if (!(reg & 4)) {
            mask |= XPSR_NZCV | XPSR_Q; /* APSR */
        }
        /* EPSR reads as zero */
        return xpsr_read(env) & mask;
        break;
    case 20: /* CONTROL */
        return env->v7m.control[env->v7m.secure];
    case 0x94: /* CONTROL_NS */
        /* We have to handle this here because unprivileged Secure code
         * can read the NS CONTROL register.
         */
        if (!env->v7m.secure) {
            return 0;
        }
        return env->v7m.control[M_REG_NS];
    }

    if (el == 0) {
        return 0; /* unprivileged reads others as zero */
    }

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        switch (reg) {
        case 0x88: /* MSP_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.other_ss_msp;
        case 0x89: /* PSP_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.other_ss_psp;
        case 0x8a: /* MSPLIM_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.msplim[M_REG_NS];
        case 0x8b: /* PSPLIM_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.psplim[M_REG_NS];
        case 0x90: /* PRIMASK_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.primask[M_REG_NS];
        case 0x91: /* BASEPRI_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.basepri[M_REG_NS];
        case 0x93: /* FAULTMASK_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.faultmask[M_REG_NS];
        case 0x98: /* SP_NS */
        {
            /* This gives the non-secure SP selected based on whether we're
             * currently in handler mode or not, using the NS CONTROL.SPSEL.
             */
            bool spsel = env->v7m.control[M_REG_NS] & R_V7M_CONTROL_SPSEL_MASK;

            if (!env->v7m.secure) {
                return 0;
            }
            if (!arm_v7m_is_handler_mode(env) && spsel) {
                return env->v7m.other_ss_psp;
            } else {
                return env->v7m.other_ss_msp;
            }
        }
        default:
            break;
        }
    }

    switch (reg) {
    case 8: /* MSP */
        return v7m_using_psp(env) ? env->v7m.other_sp : env->regs[13];
    case 9: /* PSP */
        return v7m_using_psp(env) ? env->regs[13] : env->v7m.other_sp;
    case 10: /* MSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        return env->v7m.msplim[env->v7m.secure];
    case 11: /* PSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        return env->v7m.psplim[env->v7m.secure];
    case 16: /* PRIMASK */
        return env->v7m.primask[env->v7m.secure];
    case 17: /* BASEPRI */
    case 18: /* BASEPRI_MAX */
        return env->v7m.basepri[env->v7m.secure];
    case 19: /* FAULTMASK */
        return env->v7m.faultmask[env->v7m.secure];
    default:
    bad_reg:
        qemu_log_mask(LOG_GUEST_ERROR, "Attempt to read unknown special"
                                       " register %d\n", reg);
        return 0;
    }
}

void HELPER(v7m_msr)(CPUARMState *env, uint32_t maskreg, uint32_t val)
{
    /* We're passed bits [11..0] of the instruction; extract
     * SYSm and the mask bits.
     * Invalid combinations of SYSm and mask are UNPREDICTABLE;
     * we choose to treat them as if the mask bits were valid.
     * NB that the pseudocode 'mask' variable is bits [11..10],
     * whereas ours is [11..8].
     */
    uint32_t mask = extract32(maskreg, 8, 4);
    uint32_t reg = extract32(maskreg, 0, 8);

    if (arm_current_el(env) == 0 && reg > 7) {
        /* only xPSR sub-fields may be written by unprivileged */
        return;
    }

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        switch (reg) {
        case 0x88: /* MSP_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.other_ss_msp = val;
            return;
        case 0x89: /* PSP_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.other_ss_psp = val;
            return;
        case 0x8a: /* MSPLIM_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.msplim[M_REG_NS] = val & ~7;
            return;
        case 0x8b: /* PSPLIM_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.psplim[M_REG_NS] = val & ~7;
            return;
        case 0x90: /* PRIMASK_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.primask[M_REG_NS] = val & 1;
            return;
        case 0x91: /* BASEPRI_NS */
            if (!env->v7m.secure || !arm_feature(env, ARM_FEATURE_M_MAIN)) {
                return;
            }
            env->v7m.basepri[M_REG_NS] = val & 0xff;
            return;
        case 0x93: /* FAULTMASK_NS */
            if (!env->v7m.secure || !arm_feature(env, ARM_FEATURE_M_MAIN)) {
                return;
            }
            env->v7m.faultmask[M_REG_NS] = val & 1;
            return;
        case 0x94: /* CONTROL_NS */
            if (!env->v7m.secure) {
                return;
            }
            write_v7m_control_spsel_for_secstate(env,
                                                 val & R_V7M_CONTROL_SPSEL_MASK,
                                                 M_REG_NS);
            if (arm_feature(env, ARM_FEATURE_M_MAIN)) {
                env->v7m.control[M_REG_NS] &= ~R_V7M_CONTROL_NPRIV_MASK;
                env->v7m.control[M_REG_NS] |= val & R_V7M_CONTROL_NPRIV_MASK;
            }
            return;
        case 0x98: /* SP_NS */
        {
            /* This gives the non-secure SP selected based on whether we're
             * currently in handler mode or not, using the NS CONTROL.SPSEL.
             */
            bool spsel = env->v7m.control[M_REG_NS] & R_V7M_CONTROL_SPSEL_MASK;
            bool is_psp = !arm_v7m_is_handler_mode(env) && spsel;
            uint32_t limit;

            if (!env->v7m.secure) {
                return;
            }

            limit = is_psp ? env->v7m.psplim[false] : env->v7m.msplim[false];

            if (val < limit) {
                CPUState *cs = CPU(arm_env_get_cpu(env));

                cpu_restore_state(cs, GETPC(), true);
                raise_exception(env, EXCP_STKOF, 0, 1);
            }

            if (is_psp) {
                env->v7m.other_ss_psp = val;
            } else {
                env->v7m.other_ss_msp = val;
            }
            return;
        }
        default:
            break;
        }
    }

    switch (reg) {
    case 0 ... 7: /* xPSR sub-fields */
        /* only APSR is actually writable */
        if (!(reg & 4)) {
            uint32_t apsrmask = 0;

            if (mask & 8) {
                apsrmask |= XPSR_NZCV | XPSR_Q;
            }
            if ((mask & 4) && arm_feature(env, ARM_FEATURE_THUMB_DSP)) {
                apsrmask |= XPSR_GE;
            }
            xpsr_write(env, val, apsrmask);
        }
        break;
    case 8: /* MSP */
        if (v7m_using_psp(env)) {
            env->v7m.other_sp = val;
        } else {
            env->regs[13] = val;
        }
        break;
    case 9: /* PSP */
        if (v7m_using_psp(env)) {
            env->regs[13] = val;
        } else {
            env->v7m.other_sp = val;
        }
        break;
    case 10: /* MSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        env->v7m.msplim[env->v7m.secure] = val & ~7;
        break;
    case 11: /* PSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        env->v7m.psplim[env->v7m.secure] = val & ~7;
        break;
    case 16: /* PRIMASK */
        env->v7m.primask[env->v7m.secure] = val & 1;
        break;
    case 17: /* BASEPRI */
        if (!arm_feature(env, ARM_FEATURE_M_MAIN)) {
            goto bad_reg;
        }
        env->v7m.basepri[env->v7m.secure] = val & 0xff;
        break;
    case 18: /* BASEPRI_MAX */
        if (!arm_feature(env, ARM_FEATURE_M_MAIN)) {
            goto bad_reg;
        }
        val &= 0xff;
        if (val != 0 && (val < env->v7m.basepri[env->v7m.secure]
                         || env->v7m.basepri[env->v7m.secure] == 0)) {
            env->v7m.basepri[env->v7m.secure] = val;
        }
        break;
    case 19: /* FAULTMASK */
        if (!arm_feature(env, ARM_FEATURE_M_MAIN)) {
            goto bad_reg;
        }
        env->v7m.faultmask[env->v7m.secure] = val & 1;
        break;
    case 20: /* CONTROL */
        /* Writing to the SPSEL bit only has an effect if we are in
         * thread mode; other bits can be updated by any privileged code.
         * write_v7m_control_spsel() deals with updating the SPSEL bit in
         * env->v7m.control, so we only need update the others.
         * For v7M, we must just ignore explicit writes to SPSEL in handler
         * mode; for v8M the write is permitted but will have no effect.
         */
        if (arm_feature(env, ARM_FEATURE_V8) ||
            !arm_v7m_is_handler_mode(env)) {
            write_v7m_control_spsel(env, (val & R_V7M_CONTROL_SPSEL_MASK) != 0);
        }
        if (arm_feature(env, ARM_FEATURE_M_MAIN)) {
            env->v7m.control[env->v7m.secure] &= ~R_V7M_CONTROL_NPRIV_MASK;
            env->v7m.control[env->v7m.secure] |= val & R_V7M_CONTROL_NPRIV_MASK;
        }
        break;
    default:
    bad_reg:
        qemu_log_mask(LOG_GUEST_ERROR, "Attempt to write unknown special"
                                       " register %d\n", reg);
        return;
    }
}

uint32_t HELPER(v7m_tt)(CPUARMState *env, uint32_t addr, uint32_t op)
{
    /* Implement the TT instruction. op is bits [7:6] of the insn. */
    bool forceunpriv = op & 1;
    bool alt = op & 2;
    V8M_SAttributes sattrs = {};
    uint32_t tt_resp;
    bool r, rw, nsr, nsrw, mrvalid;
    int prot;
    ARMMMUFaultInfo fi = {};
    MemTxAttrs attrs = {};
    hwaddr phys_addr;
    ARMMMUIdx mmu_idx;
    uint32_t mregion;
    bool targetpriv;
    bool targetsec = env->v7m.secure;
    bool is_subpage;

    /* Work out what the security state and privilege level we're
     * interested in is...
     */
    if (alt) {
        targetsec = !targetsec;
    }

    if (forceunpriv) {
        targetpriv = false;
    } else {
        targetpriv = arm_v7m_is_handler_mode(env) ||
            !(env->v7m.control[targetsec] & R_V7M_CONTROL_NPRIV_MASK);
    }

    /* ...and then figure out which MMU index this is */
    mmu_idx = arm_v7m_mmu_idx_for_secstate_and_priv(env, targetsec, targetpriv);

    /* We know that the MPU and SAU don't care about the access type
     * for our purposes beyond that we don't want to claim to be
     * an insn fetch, so we arbitrarily call this a read.
     */

    /* MPU region info only available for privileged or if
     * inspecting the other MPU state.
     */
    if (arm_current_el(env) != 0 || alt) {
        /* We can ignore the return value as prot is always set */
        pmsav8_mpu_lookup(env, addr, MMU_DATA_LOAD, mmu_idx,
                          &phys_addr, &attrs, &prot, &is_subpage,
                          &fi, &mregion);
        if (mregion == -1) {
            mrvalid = false;
            mregion = 0;
        } else {
            mrvalid = true;
        }
        r = prot & PAGE_READ;
        rw = prot & PAGE_WRITE;
    } else {
        r = false;
        rw = false;
        mrvalid = false;
        mregion = 0;
    }

    if (env->v7m.secure) {
        v8m_security_lookup(env, addr, MMU_DATA_LOAD, mmu_idx, &sattrs);
        nsr = sattrs.ns && r;
        nsrw = sattrs.ns && rw;
    } else {
        sattrs.ns = true;
        nsr = false;
        nsrw = false;
    }

    tt_resp = (sattrs.iregion << 24) |
        (sattrs.irvalid << 23) |
        ((!sattrs.ns) << 22) |
        (nsrw << 21) |
        (nsr << 20) |
        (rw << 19) |
        (r << 18) |
        (sattrs.srvalid << 17) |
        (mrvalid << 16) |
        (sattrs.sregion << 8) |
        mregion;

    return tt_resp;
}

#endif /* CONFIG_USER_ONLY */

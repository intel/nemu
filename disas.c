/* General "disassemble this chunk" code.  Used for debugging. */
#include "qemu/osdep.h"
#include "qemu-common.h"
#include "disas/bfd.h"
#include "elf.h"

#include "cpu.h"
#include "disas/disas.h"
#include "disas/capstone.h"

typedef struct CPUDebug {
    struct disassemble_info info;
    CPUState *cpu;
} CPUDebug;

/* Filled in by elfload.c.  Simplistic, but will do for now. */
struct syminfo *syminfos = NULL;

/* Get LENGTH bytes from info's buffer, at target address memaddr.
   Transfer them to myaddr.  */
int
buffer_read_memory(bfd_vma memaddr, bfd_byte *myaddr, int length,
                   struct disassemble_info *info)
{
    if (memaddr < info->buffer_vma
        || memaddr + length > info->buffer_vma + info->buffer_length)
        /* Out of bounds.  Use EIO because GDB uses it.  */
        return EIO;
    memcpy (myaddr, info->buffer + (memaddr - info->buffer_vma), length);
    return 0;
}

/* Get LENGTH bytes from info's buffer, at target address memaddr.
   Transfer them to myaddr.  */
static int
target_read_memory (bfd_vma memaddr,
                    bfd_byte *myaddr,
                    int length,
                    struct disassemble_info *info)
{
    CPUDebug *s = container_of(info, CPUDebug, info);

    cpu_memory_rw_debug(s->cpu, memaddr, myaddr, length, 0);
    return 0;
}

/* Print an error message.  We can assume that this is in response to
   an error return from buffer_read_memory.  */
void
perror_memory (int status, bfd_vma memaddr, struct disassemble_info *info)
{
  if (status != EIO)
    /* Can't happen.  */
    (*info->fprintf_func) (info->stream, "Unknown error %d\n", status);
  else
    /* Actually, address between memaddr and memaddr + len was
       out of bounds.  */
    (*info->fprintf_func) (info->stream,
			   "Address 0x%" PRIx64 " is out of bounds.\n", memaddr);
}

/* This could be in a separate file, to save minuscule amounts of space
   in statically linked executables.  */

/* Just print the address is hex.  This is included for completeness even
   though both GDB and objdump provide their own (to print symbolic
   addresses).  */

void
generic_print_address (bfd_vma addr, struct disassemble_info *info)
{
    (*info->fprintf_func) (info->stream, "0x%" PRIx64, addr);
}

/* Print address in hex, truncated to the width of a host virtual address. */
static void
generic_print_host_address(bfd_vma addr, struct disassemble_info *info)
{
    uint64_t mask = ~0ULL >> (64 - (sizeof(void *) * 8));
    generic_print_address(addr & mask, info);
}

/* Just return the given address.  */

int
generic_symbol_at_address (bfd_vma addr, struct disassemble_info *info)
{
  return 1;
}

bfd_vma bfd_getl64 (const bfd_byte *addr)
{
  unsigned long long v;

  v = (unsigned long long) addr[0];
  v |= (unsigned long long) addr[1] << 8;
  v |= (unsigned long long) addr[2] << 16;
  v |= (unsigned long long) addr[3] << 24;
  v |= (unsigned long long) addr[4] << 32;
  v |= (unsigned long long) addr[5] << 40;
  v |= (unsigned long long) addr[6] << 48;
  v |= (unsigned long long) addr[7] << 56;
  return (bfd_vma) v;
}

bfd_vma bfd_getl32 (const bfd_byte *addr)
{
  unsigned long v;

  v = (unsigned long) addr[0];
  v |= (unsigned long) addr[1] << 8;
  v |= (unsigned long) addr[2] << 16;
  v |= (unsigned long) addr[3] << 24;
  return (bfd_vma) v;
}

bfd_vma bfd_getb32 (const bfd_byte *addr)
{
  unsigned long v;

  v = (unsigned long) addr[0] << 24;
  v |= (unsigned long) addr[1] << 16;
  v |= (unsigned long) addr[2] << 8;
  v |= (unsigned long) addr[3];
  return (bfd_vma) v;
}

bfd_vma bfd_getl16 (const bfd_byte *addr)
{
  unsigned long v;

  v = (unsigned long) addr[0];
  v |= (unsigned long) addr[1] << 8;
  return (bfd_vma) v;
}

bfd_vma bfd_getb16 (const bfd_byte *addr)
{
  unsigned long v;

  v = (unsigned long) addr[0] << 24;
  v |= (unsigned long) addr[1] << 16;
  return (bfd_vma) v;
}

static int print_insn_objdump(bfd_vma pc, disassemble_info *info,
                              const char *prefix)
{
    int i, n = info->buffer_length;
    uint8_t *buf = g_malloc(n);

    info->read_memory_func(pc, buf, n, info);

    for (i = 0; i < n; ++i) {
        if (i % 32 == 0) {
            info->fprintf_func(info->stream, "\n%s: ", prefix);
        }
        info->fprintf_func(info->stream, "%02x", buf[i]);
    }

    g_free(buf);
    return n;
}

static int print_insn_od_host(bfd_vma pc, disassemble_info *info)
{
    return print_insn_objdump(pc, info, "OBJD-H");
}

static int print_insn_od_target(bfd_vma pc, disassemble_info *info)
{
    return print_insn_objdump(pc, info, "OBJD-T");
}

# define cap_disas_target(i, p, s)  false
# define cap_disas_host(i, p, s)  false
# define cap_disas_monitor(i, p, c)  false

/* Disassemble this for me please... (debugging).  */
void target_disas(FILE *out, CPUState *cpu, target_ulong code,
                  target_ulong size)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);
    target_ulong pc;
    int count;
    CPUDebug s;

    INIT_DISASSEMBLE_INFO(s.info, out, fprintf);

    s.cpu = cpu;
    s.info.read_memory_func = target_read_memory;
    s.info.buffer_vma = code;
    s.info.buffer_length = size;
    s.info.print_address_func = generic_print_address;
    s.info.cap_arch = -1;
    s.info.cap_mode = 0;
    s.info.cap_insn_unit = 4;
    s.info.cap_insn_split = 4;

#ifdef TARGET_WORDS_BIGENDIAN
    s.info.endian = BFD_ENDIAN_BIG;
#else
    s.info.endian = BFD_ENDIAN_LITTLE;
#endif

    if (cc->disas_set_info) {
        cc->disas_set_info(cpu, &s.info);
    }

    if (s.info.cap_arch >= 0 && cap_disas_target(&s.info, code, size)) {
        return;
    }

    if (s.info.print_insn == NULL) {
        s.info.print_insn = print_insn_od_target;
    }

    for (pc = code; size > 0; pc += count, size -= count) {
	fprintf(out, "0x" TARGET_FMT_lx ":  ", pc);
	count = s.info.print_insn(pc, &s.info);
	fprintf(out, "\n");
	if (count < 0)
	    break;
        if (size < count) {
            fprintf(out,
                    "Disassembler disagrees with translator over instruction "
                    "decoding\n"
                    "Please report this to qemu-devel@nongnu.org\n");
            break;
        }
    }
}

/* Disassemble this for me please... (debugging). */
void disas(FILE *out, void *code, unsigned long size)
{
    uintptr_t pc;
    int count;
    CPUDebug s;
    int (*print_insn)(bfd_vma pc, disassemble_info *info) = NULL;

    INIT_DISASSEMBLE_INFO(s.info, out, fprintf);
    s.info.print_address_func = generic_print_host_address;

    s.info.buffer = code;
    s.info.buffer_vma = (uintptr_t)code;
    s.info.buffer_length = size;
    s.info.cap_arch = -1;
    s.info.cap_mode = 0;
    s.info.cap_insn_unit = 4;
    s.info.cap_insn_split = 4;

#ifdef HOST_WORDS_BIGENDIAN
    s.info.endian = BFD_ENDIAN_BIG;
#else
    s.info.endian = BFD_ENDIAN_LITTLE;
#endif
#if defined(CONFIG_TCG_INTERPRETER)
    print_insn = print_insn_tci;
#elif defined(__i386__)
    s.info.mach = bfd_mach_i386_i386;
    print_insn = print_insn_i386;
    s.info.cap_arch = CS_ARCH_X86;
    s.info.cap_mode = CS_MODE_32;
    s.info.cap_insn_unit = 1;
    s.info.cap_insn_split = 8;
#elif defined(__x86_64__)
    s.info.mach = bfd_mach_x86_64;
    print_insn = print_insn_i386;
    s.info.cap_arch = CS_ARCH_X86;
    s.info.cap_mode = CS_MODE_64;
    s.info.cap_insn_unit = 1;
    s.info.cap_insn_split = 8;
#elif defined(_ARCH_PPC)
    s.info.disassembler_options = (char *)"any";
    print_insn = print_insn_ppc;
    s.info.cap_arch = CS_ARCH_PPC;
#elif defined(__riscv__)
    print_insn = print_insn_riscv;
#elif defined(__aarch64__) && defined(CONFIG_ARM_A64_DIS)
    print_insn = print_insn_arm_a64;
    s.info.cap_arch = CS_ARCH_ARM64;
#elif defined(__sparc__)
    print_insn = print_insn_sparc;
    s.info.mach = bfd_mach_sparc_v9b;
#elif defined(__arm__)
    print_insn = print_insn_arm;
    s.info.cap_arch = CS_ARCH_ARM;
    /* TCG only generates code for arm mode.  */
#elif defined(__MIPSEB__)
    print_insn = print_insn_big_mips;
#elif defined(__MIPSEL__)
    print_insn = print_insn_little_mips;
#elif defined(__m68k__)
    print_insn = print_insn_m68k;
#elif defined(__hppa__)
    print_insn = print_insn_hppa;
#endif

    if (s.info.cap_arch >= 0 && cap_disas_host(&s.info, code, size)) {
        return;
    }

    if (print_insn == NULL) {
        print_insn = print_insn_od_host;
    }
    for (pc = (uintptr_t)code; size > 0; pc += count, size -= count) {
        fprintf(out, "0x%08" PRIxPTR ":  ", pc);
        count = print_insn(pc, &s.info);
	fprintf(out, "\n");
	if (count < 0)
	    break;
    }
}

/* Look up symbol for debugging purpose.  Returns "" if unknown. */
const char *lookup_symbol(target_ulong orig_addr)
{
    const char *symbol = "";
    struct syminfo *s;

    for (s = syminfos; s; s = s->next) {
        symbol = s->lookup_symbol(s, orig_addr);
        if (symbol[0] != '\0') {
            break;
        }
    }

    return symbol;
}


#include "monitor/monitor.h"

static int
physical_read_memory(bfd_vma memaddr, bfd_byte *myaddr, int length,
                     struct disassemble_info *info)
{
    cpu_physical_memory_read(memaddr, myaddr, length);
    return 0;
}

/* Disassembler for the monitor.  */
void monitor_disas(Monitor *mon, CPUState *cpu,
                   target_ulong pc, int nb_insn, int is_physical)
{
    CPUClass *cc = CPU_GET_CLASS(cpu);
    int count, i;
    CPUDebug s;

    INIT_DISASSEMBLE_INFO(s.info, (FILE *)mon, monitor_fprintf);

    s.cpu = cpu;
    s.info.read_memory_func
        = (is_physical ? physical_read_memory : target_read_memory);
    s.info.print_address_func = generic_print_address;
    s.info.buffer_vma = pc;
    s.info.cap_arch = -1;
    s.info.cap_mode = 0;
    s.info.cap_insn_unit = 4;
    s.info.cap_insn_split = 4;

#ifdef TARGET_WORDS_BIGENDIAN
    s.info.endian = BFD_ENDIAN_BIG;
#else
    s.info.endian = BFD_ENDIAN_LITTLE;
#endif

    if (cc->disas_set_info) {
        cc->disas_set_info(cpu, &s.info);
    }

    if (s.info.cap_arch >= 0 && cap_disas_monitor(&s.info, pc, nb_insn)) {
        return;
    }

    if (!s.info.print_insn) {
        monitor_printf(mon, "0x" TARGET_FMT_lx
                       ": Asm output not supported on this arch\n", pc);
        return;
    }

    for(i = 0; i < nb_insn; i++) {
	monitor_printf(mon, "0x" TARGET_FMT_lx ":  ", pc);
        count = s.info.print_insn(pc, &s.info);
	monitor_printf(mon, "\n");
	if (count < 0)
	    break;
        pc += count;
    }
}

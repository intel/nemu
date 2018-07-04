/*
 *
 * Copyright (c) 2018 Intel Corportation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include "hw/boards.h"
#include "hw/loader.h"

#include "hw/i386/acpi.h"
#include "hw/i386/kernel-loader.h"
#include "hw/i386/memory.h"

#include "elf.h"
#include "multiboot.h"

#define BOOT_GDT                0x500
#define BOOT_IDT                0x520
#define BOOT_GDT_NULL           0
#define BOOT_GDT_CODE           1
#define BOOT_GDT_DATA           2
#define BOOT_GDT_TSS            3
#define BOOT_GDT_MAX            4
#define BOOT_GDT_FLAGS_CODE     (DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK | \
                                 DESC_R_MASK | DESC_A_MASK | DESC_G_MASK )
#define BOOT_GDT_FLAGS_DATA     (DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |  \
                                 DESC_A_MASK | DESC_B_MASK | DESC_G_MASK)
#define BOOT_GDT_FLAGS_TSS      DESC_P_MASK | (11 << DESC_TYPE_SHIFT)
#define BOOT_PML4               0x9000
#define BOOT_PDPTE              0xA000
#define BOOT_LOADER_SP          0x8000
#define BOOT_CMDLINE_OFFSET     0x20000
#define BOOT_ZEROPAGE_OFFSET    0x7000

#define GDT_ENTRY(flags, base, limit)               \
       ((((base)  & 0xff000000ULL) << (56-24)) |    \
       (((flags) & 0x0000f0ffULL) << 40) |          \
       (((limit) & 0x000f0000ULL) << (48-16)) |     \
       (((base)  & 0x00ffffffULL) << 16) |          \
       (((limit) & 0x0000ffffULL)))

struct kernel_boot_info {
    uint64_t entry;
    bool protected_mode;
    bool long_mode;
};

static struct kernel_boot_info boot_info;

void kernel_loader_reset_cpu(CPUX86State *env)
{
    unsigned int flags = BOOT_GDT_FLAGS_CODE;

    if (boot_info.long_mode) {
        flags |= DESC_L_MASK;
    }
    cpu_x86_load_seg_cache(env, R_CS, BOOT_GDT_CODE * 8, 0, 0xfffff, flags);

    cpu_x86_load_seg_cache(env, R_DS, BOOT_GDT_DATA * 8, 0, 0xfffff,
                           BOOT_GDT_FLAGS_DATA);
    cpu_x86_load_seg_cache(env, R_ES, BOOT_GDT_DATA * 8, 0, 0xfffff,
                           BOOT_GDT_FLAGS_DATA);
    cpu_x86_load_seg_cache(env, R_FS, BOOT_GDT_DATA * 8, 0, 0xfffff,
                           BOOT_GDT_FLAGS_DATA);
    cpu_x86_load_seg_cache(env, R_GS, BOOT_GDT_DATA * 8, 0, 0xfffff,
                           BOOT_GDT_FLAGS_DATA);
    cpu_x86_load_seg_cache(env, R_SS, BOOT_GDT_DATA * 8, 0, 0xfffff,
                           BOOT_GDT_FLAGS_DATA);

    env->gdt.base = BOOT_GDT;
    env->gdt.limit = BOOT_GDT_MAX * 8 - 1;

    env->idt.base = BOOT_IDT;

    env->tr.selector = BOOT_GDT_TSS * 8;
    env->tr.flags = BOOT_GDT_FLAGS_TSS;

    env->cr[3] = BOOT_PML4;
    env->cr[0] |= (CR0_PG_MASK | CR0_PE_MASK);

    if (boot_info.long_mode) {
        env->cr[4] |= CR4_PAE_MASK;
        cpu_load_efer(env, env->efer | MSR_EFER_LME | MSR_EFER_LMA);
    }

    env->regs[R_ESP] = BOOT_LOADER_SP;
    env->regs[R_ESI] = BOOT_ZEROPAGE_OFFSET;
    env->eip = boot_info.entry;
}

static void setup_seg_desc_tables(void)
{
    uint64_t idt = 0;
    uint64_t gdt[BOOT_GDT_MAX] = {
             [BOOT_GDT_NULL] = GDT_ENTRY(0, 0, 0),
             [BOOT_GDT_CODE] = GDT_ENTRY(BOOT_GDT_FLAGS_CODE, 0, 0xFFFFF),
             [BOOT_GDT_DATA] = GDT_ENTRY(BOOT_GDT_FLAGS_DATA, 0, 0xFFFFF),
             [BOOT_GDT_TSS ] = GDT_ENTRY(BOOT_GDT_FLAGS_TSS, 0, 0xFFFFF)
            };

    if (boot_info.long_mode) {
        gdt[BOOT_GDT_CODE] |= (1UL << (32 + DESC_L_SHIFT));
    }

    cpu_physical_memory_write((hwaddr)BOOT_GDT, gdt, sizeof(gdt));
    cpu_physical_memory_write((hwaddr)BOOT_IDT, &idt, sizeof(idt));
}

static void setup_page_tables(void)
{
    void *p;
    size_t len = 4096;

    p = cpu_physical_memory_map(BOOT_PML4, &len, 1);
    memset(p, 0, 4096);
    *(uint64_t*)p = (uint64_t)(BOOT_PDPTE | 3);
    cpu_physical_memory_unmap(p, len, 1, len);

    len = 4096;
    p = cpu_physical_memory_map(BOOT_PDPTE, &len, 1);
    memset(p, 0, 4096);
    *(uint64_t*)p = 0x83;
    cpu_physical_memory_unmap(p, len, 1, len);
}

static void setup_kernel_zero_page(void)
{
    int i;
    uint8_t *zero_page;
    void *e820_map;
    size_t zero_page_size = 4096;
    MachineState *machine = MACHINE(qdev_get_machine());
    size_t cmdline_size = strlen(machine->kernel_cmdline) + 1;

    cpu_physical_memory_write((hwaddr)BOOT_CMDLINE_OFFSET,
                               machine->kernel_cmdline, cmdline_size);

    zero_page = cpu_physical_memory_map((hwaddr)BOOT_ZEROPAGE_OFFSET,
                                        &zero_page_size, 1);
    memset(zero_page, 0, zero_page_size);

    /* hdr.type_of_loader */
    zero_page[0x210] = 0xFF;
    /* hdr.boot_flag */
    stw_p(zero_page + 0x1fe, 0xAA55);
    /* hdr.header */
    stl_p(zero_page + 0x202, 0x53726448);
    /* hdr.cmd_line_ptr */
    stl_p(zero_page + 0x228, BOOT_CMDLINE_OFFSET);
    /* hdr.cmdline_size */
    stl_p(zero_page + 0x238, cmdline_size);
    /* e820_entries */
    zero_page[0x1e8] = e820_entries;
    /* e820_map */
    e820_map = zero_page + 0x2d0;
    for (i = 0; i < e820_entries; i++) {
        stq_p(e820_map, e820_table[i].address);
        e820_map += 8;
        stq_p(e820_map, e820_table[i].length);
        e820_map += 8;
        stl_p(e820_map, e820_table[i].type);
        e820_map += 4;
    }

    cpu_physical_memory_unmap(zero_page, zero_page_size, 1, zero_page_size);
}

void kernel_loader_setup(void)
{
    setup_seg_desc_tables();
    setup_page_tables();
    setup_kernel_zero_page();
}


/* setup_data types */
#define SETUP_NONE     0
#define SETUP_E820_EXT 1
#define SETUP_DTB      2
#define SETUP_PCI      3
#define SETUP_EFI      4

struct setup_data {
    uint64_t next;
    uint32_t type;
    uint32_t len;
    uint8_t data[0];
} __attribute__((packed));

static long get_file_size(FILE *f)
{
    long where, size;

    /* XXX: on Unix systems, using fstat() probably makes more sense */

    where = ftell(f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, where, SEEK_SET);

    return size;
}

void load_linux_bzimage(MachineState *machine, AcpiConfiguration *conf, FWCfgState *fw_cfg)
{
    uint16_t protocol;
    int setup_size, kernel_size, initrd_size = 0, cmdline_size;
    int dtb_size, setup_data_offset;
    uint32_t initrd_max;
    uint8_t header[8192], *setup, *kernel, *initrd_data;
    hwaddr real_addr, prot_addr, cmdline_addr, initrd_addr = 0;
    FILE *f;
    char *vmode;
    struct setup_data *setup_data;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    const char *dtb_filename = machine->dtb;
    const char *kernel_cmdline = machine->kernel_cmdline;

    /* Align to 16 bytes as a paranoia measure */
    cmdline_size = (strlen(kernel_cmdline)+16) & ~15;

    /* load the kernel header */
    f = fopen(kernel_filename, "rb");
    if (!f || !(kernel_size = get_file_size(f)) ||
        fread(header, 1, MIN(ARRAY_SIZE(header), kernel_size), f) !=
        MIN(ARRAY_SIZE(header), kernel_size)) {
        fprintf(stderr, "qemu: could not load kernel '%s': %s\n",
                kernel_filename, strerror(errno));
        exit(1);
    }

    /* kernel protocol version */
#if 0
    fprintf(stderr, "header magic: %#x\n", ldl_p(header+0x202));
#endif
    if (ldl_p(header+0x202) == 0x53726448) {
        protocol = lduw_p(header+0x206);
    } else {
        /* This looks like a multiboot kernel. If it is, let's stop
           treating it like a Linux kernel. */
        if (load_multiboot(fw_cfg, f, kernel_filename, initrd_filename,
                           kernel_cmdline, kernel_size, header)) {
            return;
        }
        protocol = 0;
    }

    if (protocol < 0x200 || !(header[0x211] & 0x01)) {
        /* Low kernel */
        real_addr    = 0x90000;
        cmdline_addr = 0x9a000 - cmdline_size;
        prot_addr    = 0x10000;
    } else if (protocol < 0x202) {
        /* High but ancient kernel */
        real_addr    = 0x90000;
        cmdline_addr = 0x9a000 - cmdline_size;
        prot_addr    = 0x100000;
    } else {
        /* High and recent kernel */
        real_addr    = 0x10000;
        cmdline_addr = 0x20000;
        prot_addr    = 0x100000;
    }

    /* highest address for loading the initrd */
    if (protocol >= 0x203) {
        initrd_max = ldl_p(header+0x22c);
    } else {
        initrd_max = 0x37ffffff;
    }

    if (initrd_max >= conf->below_4g_mem_size - conf->acpi_data_size) {
        initrd_max = conf->below_4g_mem_size - conf->acpi_data_size - 1;
    }

    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_ADDR, cmdline_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_SIZE, strlen(kernel_cmdline)+1);
    fw_cfg_add_string(fw_cfg, FW_CFG_CMDLINE_DATA, kernel_cmdline);

    if (protocol >= 0x202) {
        stl_p(header+0x228, cmdline_addr);
    } else {
        stw_p(header+0x20, 0xA33F);
        stw_p(header+0x22, cmdline_addr-real_addr);
    }

    /* handle vga= parameter */
    vmode = strstr(kernel_cmdline, "vga=");
    if (vmode) {
        unsigned int video_mode;
        /* skip "vga=" */
        vmode += 4;
        if (!strncmp(vmode, "normal", 6)) {
            video_mode = 0xffff;
        } else if (!strncmp(vmode, "ext", 3)) {
            video_mode = 0xfffe;
        } else if (!strncmp(vmode, "ask", 3)) {
            video_mode = 0xfffd;
        } else {
            video_mode = strtol(vmode, NULL, 0);
        }
        stw_p(header+0x1fa, video_mode);
    }

    /* loader type */
    /* High nybble = B reserved for QEMU; low nybble is revision number.
       If this code is substantially changed, you may want to consider
       incrementing the revision. */
    if (protocol >= 0x200) {
        header[0x210] = 0xB0;
    }
    /* heap */
    if (protocol >= 0x201) {
        header[0x211] |= 0x80;	/* CAN_USE_HEAP */
        stw_p(header+0x224, cmdline_addr-real_addr-0x200);
    }

    /* load initrd */
    if (initrd_filename) {
        if (protocol < 0x200) {
            fprintf(stderr, "qemu: linux kernel too old to load a ram disk\n");
            exit(1);
        }

        initrd_size = get_image_size(initrd_filename);
        if (initrd_size < 0) {
            fprintf(stderr, "qemu: error reading initrd %s: %s\n",
                    initrd_filename, strerror(errno));
            exit(1);
        }

        initrd_addr = (initrd_max-initrd_size) & ~4095;

        initrd_data = g_malloc(initrd_size);
        load_image(initrd_filename, initrd_data);

        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_addr);
        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_INITRD_DATA, initrd_data, initrd_size);

        stl_p(header+0x218, initrd_addr);
        stl_p(header+0x21c, initrd_size);
    }

    /* load kernel and setup */
    setup_size = header[0x1f1];
    if (setup_size == 0) {
        setup_size = 4;
    }
    setup_size = (setup_size+1)*512;
    if (setup_size > kernel_size) {
        fprintf(stderr, "qemu: invalid kernel header\n");
        exit(1);
    }
    kernel_size -= setup_size;

    setup  = g_malloc(setup_size);
    kernel = g_malloc(kernel_size);
    fseek(f, 0, SEEK_SET);
    if (fread(setup, 1, setup_size, f) != setup_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    if (fread(kernel, 1, kernel_size, f) != kernel_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    fclose(f);

    /* append dtb to kernel */
    if (dtb_filename) {
        if (protocol < 0x209) {
            fprintf(stderr, "qemu: Linux kernel too old to load a dtb\n");
            exit(1);
        }

        dtb_size = get_image_size(dtb_filename);
        if (dtb_size <= 0) {
            fprintf(stderr, "qemu: error reading dtb %s: %s\n",
                    dtb_filename, strerror(errno));
            exit(1);
        }

        setup_data_offset = QEMU_ALIGN_UP(kernel_size, 16);
        kernel_size = setup_data_offset + sizeof(struct setup_data) + dtb_size;
        kernel = g_realloc(kernel, kernel_size);

        stq_p(header+0x250, prot_addr + setup_data_offset);

        setup_data = (struct setup_data *)(kernel + setup_data_offset);
        setup_data->next = 0;
        setup_data->type = cpu_to_le32(SETUP_DTB);
        setup_data->len = cpu_to_le32(dtb_size);

        load_image_size(dtb_filename, setup_data->data, dtb_size);
    }

    memcpy(setup, header, MIN(sizeof(header), setup_size));

    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, prot_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_KERNEL_DATA, kernel, kernel_size);

    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_ADDR, real_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_SIZE, setup_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_SETUP_DATA, setup, setup_size);

    option_rom[nb_option_roms].bootindex = 0;
    option_rom[nb_option_roms].name = "linuxboot.bin";
    if (conf->linuxboot_dma_enabled && fw_cfg_dma_enabled(fw_cfg)) {
        option_rom[nb_option_roms].name = "linuxboot_dma.bin";
    }
    nb_option_roms++;
}

void load_linux_efi(MachineState *machine)
{
    unsigned char class;
    FILE *file = fopen(machine->kernel_filename, "rb");

    if (!file) {
        goto err;
    }

    if (fseek(file, EI_CLASS, 0) || fread(&class, 1, 1, file) != 1) {
        fclose(file);
        goto err;
    }
    fclose(file);

    if (load_elf(machine->kernel_filename, NULL, NULL, &boot_info.entry,
                   NULL, NULL, 0, EM_X86_64, 0, 0) < 0) {
        goto err;
    }

    if (class == ELFCLASS64) {
        boot_info.long_mode = true;
    } else if (class != ELFCLASS32) {
        goto err;
    }

    boot_info.protected_mode = true;
    return;

err:
    fprintf(stderr, "qemu: could not load kernel '%s'\n",
                    machine->kernel_filename);
    exit(1);
}

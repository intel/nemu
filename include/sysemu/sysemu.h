#ifndef SYSEMU_H
#define SYSEMU_H
/* Misc. things related to the system emulator.  */

#include "qapi/qapi-types-run-state.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "qemu/notify.h"
#include "qemu/main-loop.h"
#include "qemu/bitmap.h"
#include "qemu/uuid.h"
#include "qom/object.h"

/* vl.c */

extern const char *bios_name;
extern const char *qemu_name;
extern QemuUUID qemu_uuid;
extern bool qemu_uuid_set;

bool runstate_check(RunState state);
void runstate_set(RunState new_state);
int runstate_is_running(void);
bool runstate_needs_reset(void);
bool runstate_store(char *str, size_t size);
typedef struct vm_change_state_entry VMChangeStateEntry;
typedef void VMChangeStateHandler(void *opaque, int running, RunState state);

VMChangeStateEntry *qemu_add_vm_change_state_handler(VMChangeStateHandler *cb,
                                                     void *opaque);
void qemu_del_vm_change_state_handler(VMChangeStateEntry *e);
void vm_state_notify(int running, RunState state);

/* Enumeration of various causes for shutdown. */
typedef enum ShutdownCause {
    SHUTDOWN_CAUSE_NONE,          /* No shutdown request pending */
    SHUTDOWN_CAUSE_HOST_ERROR,    /* An error prevents further use of guest */
    SHUTDOWN_CAUSE_HOST_QMP,      /* Reaction to a QMP command, like 'quit' */
    SHUTDOWN_CAUSE_HOST_SIGNAL,   /* Reaction to a signal, such as SIGINT */
    SHUTDOWN_CAUSE_HOST_UI,       /* Reaction to UI event, like window close */
    SHUTDOWN_CAUSE_GUEST_SHUTDOWN,/* Guest shutdown/suspend request, via
                                     ACPI or other hardware-specific means */
    SHUTDOWN_CAUSE_GUEST_RESET,   /* Guest reset request, and command line
                                     turns that into a shutdown */
    SHUTDOWN_CAUSE_GUEST_PANIC,   /* Guest panicked, and command line turns
                                     that into a shutdown */
    SHUTDOWN_CAUSE__MAX,
} ShutdownCause;

static inline bool shutdown_caused_by_guest(ShutdownCause cause)
{
    return cause >= SHUTDOWN_CAUSE_GUEST_SHUTDOWN;
}

void vm_start(void);
int vm_prepare_start(void);
int vm_stop(RunState state);
int vm_stop_force_state(RunState state);
int vm_shutdown(void);

typedef enum WakeupReason {
    /* Always keep QEMU_WAKEUP_REASON_NONE = 0 */
    QEMU_WAKEUP_REASON_NONE = 0,
    QEMU_WAKEUP_REASON_RTC,
    QEMU_WAKEUP_REASON_PMTIMER,
    QEMU_WAKEUP_REASON_OTHER,
} WakeupReason;

void qemu_system_reset_request(ShutdownCause reason);
void qemu_system_suspend_request(void);
void qemu_register_suspend_notifier(Notifier *notifier);
void qemu_system_wakeup_request(WakeupReason reason);
void qemu_system_wakeup_enable(WakeupReason reason, bool enabled);
void qemu_register_wakeup_notifier(Notifier *notifier);
void qemu_system_shutdown_request(ShutdownCause reason);
void qemu_system_powerdown_request(void);
void qemu_register_powerdown_notifier(Notifier *notifier);
void qemu_system_debug_request(void);
void qemu_system_vmstop_request(RunState reason);
void qemu_system_vmstop_request_prepare(void);
bool qemu_vmstop_requested(RunState *r);
void qemu_system_killed(int signal, pid_t pid);
void qemu_system_reset(ShutdownCause reason);
void qemu_system_guest_panicked(GuestPanicInformation *info);

void qemu_add_exit_notifier(Notifier *notify);
void qemu_remove_exit_notifier(Notifier *notify);

extern bool machine_init_done;

void qemu_add_machine_init_done_notifier(Notifier *notify);
void qemu_remove_machine_init_done_notifier(Notifier *notify);

void qemu_announce_self(void);

extern int autostart;

extern const char *keyboard_layout;
extern int win2k_install_hack;
extern int alt_grab;
extern int ctrl_grab;
extern int no_frame;
extern int smp_cpus;
extern unsigned int max_cpus;
extern int graphic_rotate;
extern int no_quit;
extern int no_shutdown;
extern int old_param;
extern int boot_menu;
extern bool boot_strict;
extern uint8_t *boot_splash_filedata;
extern size_t boot_splash_filedata_size;
extern bool enable_mlock;
extern uint8_t qemu_extra_params_fw[2];
extern QEMUClockType rtc_clock;
extern const char *mem_path;
extern int mem_prealloc;

#define MAX_NODES 128
#define NUMA_NODE_UNASSIGNED MAX_NODES
#define NUMA_DISTANCE_MIN         10
#define NUMA_DISTANCE_DEFAULT     20
#define NUMA_DISTANCE_MAX         254
#define NUMA_DISTANCE_UNREACHABLE 255

#define MAX_OPTION_ROMS 16
typedef struct QEMUOptionRom {
    const char *name;
    int32_t bootindex;
} QEMUOptionRom;
extern QEMUOptionRom option_rom[MAX_OPTION_ROMS];
extern int nb_option_roms;

/* generic hotplug */
void hmp_drive_add(Monitor *mon, const QDict *qdict);

/* pcie aer error injection */
void hmp_pcie_aer_inject_error(Monitor *mon, const QDict *qdict);

/* serial ports */

#define MAX_SERIAL_PORTS 4

extern Chardev *serial_hds[MAX_SERIAL_PORTS];

/* parallel ports */

#define MAX_PARALLEL_PORTS 3

extern Chardev *parallel_hds[MAX_PARALLEL_PORTS];

void hmp_info_usb(Monitor *mon, const QDict *qdict);

void add_boot_device_path(int32_t bootindex, DeviceState *dev,
                          const char *suffix);
char *get_boot_devices_list(size_t *size, bool ignore_suffixes);

void check_boot_index(int32_t bootindex, Error **errp);
void del_boot_device_path(DeviceState *dev, const char *suffix);
void device_add_bootindex_property(Object *obj, int32_t *bootindex,
                                   const char *name, const char *suffix,
                                   DeviceState *dev, Error **errp);
void restore_boot_order(void *opaque);
void validate_bootdevices(const char *devices, Error **errp);

/* handler to set the boot_device order for a specific type of MachineClass */
typedef void QEMUBootSetHandler(void *opaque, const char *boot_order,
                                Error **errp);
void qemu_register_boot_set(QEMUBootSetHandler *func, void *opaque);
void qemu_boot_set(const char *boot_order, Error **errp);

QemuOpts *qemu_get_machine_opts(void);


extern QemuOptsList qemu_legacy_drive_opts;
extern QemuOptsList qemu_common_drive_opts;
extern QemuOptsList qemu_drive_opts;
extern QemuOptsList bdrv_runtime_opts;
extern QemuOptsList qemu_chardev_opts;
extern QemuOptsList qemu_device_opts;
extern QemuOptsList qemu_netdev_opts;
extern QemuOptsList qemu_nic_opts;
extern QemuOptsList qemu_net_opts;
extern QemuOptsList qemu_global_opts;
extern QemuOptsList qemu_mon_opts;

#endif

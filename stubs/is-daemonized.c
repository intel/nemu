#include "qemu/osdep.h"
#include "qemu-common.h"

/* Win32 has its own inline stub */
bool is_daemonized(void)
{
    return false;
}

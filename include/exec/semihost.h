/*
 * Semihosting support
 *
 * Copyright (c) 2015 Imagination Technologies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SEMIHOST_H
#define SEMIHOST_H

typedef enum SemihostingTarget {
    SEMIHOSTING_TARGET_AUTO = 0,
    SEMIHOSTING_TARGET_NATIVE,
    SEMIHOSTING_TARGET_GDB
} SemihostingTarget;

bool semihosting_enabled(void);
SemihostingTarget semihosting_get_target(void);
const char *semihosting_get_arg(int i);
int semihosting_get_argc(void);
const char *semihosting_get_cmdline(void);

#endif

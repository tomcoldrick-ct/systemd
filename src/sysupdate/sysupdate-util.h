/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

int reboot_now(void);

#define SD_SYSTEMD_SYSUPDATE_OFFLINE  (UINT64_C(1) << 0)

/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Installation and recovery: becoming the machine's VDM, reversibly; the recent list; the command line.
 *
 * Declarations only (#335): defined in host_install.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_INSTALL_H
#define NTVDMEX_HOST_INSTALL_H

#include "host_state.h"

UINT RecoveryRead(VOID);
VOID RecoveryWrite(UINT value);
VOID RecoveryOk(VOID);
INT MruLoad(CHAR out[MRU_MAX][MAX_PATH]);
VOID MruAdd(PCSTR path);
INT InstallPerform(INT want, INT force, PSTR message, DWORD cap);
INSTALL_STATE InstallStatusText(PSTR message, DWORD cap);
INT InstallVerb(PCSTR command);
INT CommandLineHasForce(PCSTR command);
INT CommandLineBare(PCSTR command);
INT LaunchShellVdm(VOID);
VOID InstallReport(PCSTR message, INT isOk);
VOID RecoveryUninstall(PSTR *logCursor);
PCSTR CommandLineAfterArgv0(PCSTR cursor);

#endif

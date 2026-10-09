/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The end-of-run report: what the run did, section by section, in the
 * order the log has always had them (ReportEndOfRun), plus the start mode and the DOS
 * output that WinMain reports just before it.
 *
 * Declarations only (#335): defined in host_report.c.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_HOST_REPORT_H
#define NTVDMEX_HOST_REPORT_H

#include "host_state.h"

PSTR ReportStartMode(PSTR cursor);
PSTR ReportStdoutAndDosOutput(PSTR cursor, DOS_MACHINE *machine);
PSTR ReportEndOfRun(
    PSTR cursor,
    PSTR const base,
    PCSTR const reportEnd,
    DOS_MACHINE *machine,
    volatile BYTE * const tib);

#endif

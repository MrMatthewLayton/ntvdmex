/* host_dos.h -- the DOS side of the host: EXEC and termination, INT 24h, disks, stdio and the
 *   console, and the XMS/EMS host calls.
 *
 * Declarations only (#335): defined in host_dos.c. */
#ifndef NTVDMEX_HOST_DOS_H
#define NTVDMEX_HOST_DOS_H
#include "host_state.h"

PDOS_DISK_GEOMETRY DiskFor(UINT drive);
INT DiskIo(UINT drive, UINT32 lba, UINT count, BYTE *guest, INT write);
VOID HostSetFlags(volatile BYTE *tib, BYTE carryFlag, BYTE zeroFlag);
VOID HostXms(volatile BYTE *tib);
VOID HostEms(volatile BYTE *tib);

extern INT g_Routed;
extern INT g_BackToPrompt;
UINT LauncherCompilerVariables(PCSTR environment, DWORD environmentCapacity, PSTR out, DWORD outCapacity);
extern EMU8K_STATE g_Emu8K;
extern NTVDD_DEVICE g_Emu8KDevice;
extern INT g_AweOn;
extern MPU_STATE g_Mpu;
extern NTVDD_DEVICE g_MpuDevice;
extern PVOID g_Hma;
extern DWORD g_HmaError;
extern DWORD g_HmaProtection;
extern DWORD g_HmaState;
VOID HmaTry(VOID);
extern DOS_EMS_STATE g_Ems;
PSTR ExecBegin(DOS_MACHINE *machine, volatile BYTE *tib, PSTR cursor);
VOID CriticalSnapshot(volatile BYTE *tib);
VOID CriticalRaise(DOS_MACHINE *machine, volatile BYTE *tib, PSTR *logCursor);
INT CriticalReturn(DOS_MACHINE *machine, volatile BYTE *tib, PSTR *logCursor);
INT PmRwHardwareFail(DOS_MACHINE *machine, volatile BYTE *tib, BYTE function, DWORD win32Error, PSTR *logCursor);
INT DosPrnOut(PVOID context, BYTE character);
VOID DosAuxOut(PVOID context, BYTE character);
extern INT g_BehaveDos622;
INT DosTerminate(DOS_MACHINE *machine, PVOID tib, PSTR *logCursor, PSTR base);
extern PCSTR g_FloppyImage;
INT HostHasFloppy(VOID);
INT HostHasCdrom(VOID);
extern HANDLE g_Stdio;
VOID StdioFlush(VOID);
PCSTR StdioInitialize(VOID);
PCSTR StdioInitializeVdm(VOID);
VOID HostConsoleOut(PVOID context, BYTE ch);
INT HostConsoleIn(PVOID context);
INT TypeInPush(PCSTR text);
INT HostConsoleInNoBlock(PVOID context);
INT HostConsolePeek(PVOID context);
PVOID XmsHostAllocate(PVOID context, DWORD kilobytes);
VOID XmsHostFree(PVOID context, PVOID memory, DWORD kilobytes);
PVOID EmsHostAllocate(PVOID context, DWORD pages);
VOID EmsHostFree(PVOID context, PVOID memory, DWORD pages);
VOID ExecMachineRestore(INT depth, PSTR *logCursor);
INT CloseProgramNow(DOS_MACHINE *machine, PVOID tib, PSTR *logCursor, PSTR base);
VOID DosWowPublish(volatile BYTE *handlerArea, volatile BYTE *controlTable, UINT currentDrive);
#endif

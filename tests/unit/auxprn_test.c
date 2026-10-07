/* auxprn_test.c -- off-VM battery for DOS's AUX/PRN driver code (GH #251).
 *
 * src/dos/dos_auxprn.h is guest code the host resumes the V86 guest in for INT 21h
 * AH=03h/04h/05h and AH=3Fh/40h on handles 3/4. It is RUN here, in the host's own
 * 8086 interpreter (v86interp.h), with INT 14h and INT 17h pointed at recorders --
 * the same arrangement tests/probes/dos/p_auxprn makes on the oracles -- and the call
 * sequences are held to what MS-DOS 6.22 (QEMU and PCem) logged:
 *
 *   05h:     17h/0200 17h/0200 17h/00cc            AX=05cc
 *   04h:     14h/0300 14h/01cc                     AX=04cc
 *   03h:     14h/0300 14h/02xx (AL from status)    AX=03 + byte
 *   40h h4:  per byte 17h/0200 17h/00cc            AX=CX CF=0
 *   40h h3:  per byte 14h/01cc                     AX=CX CF=0
 *   3Fh h3:  14h/0200 per byte, stop after CR      AX=n  CF=0
 *
 * The interpreter's own names (icpu, istep, V86_CF) and the four callbacks it requires of
 * its includer (V86HostRead8, V86HostWrite8, V86HostIn, V86HostOut) are v86interp.h's, not this test's.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>                     /* v86interp.h needs the fixed-width types ... */
#include "../../src/ntvdmex_types.h"    /* ... and BYTE                                */

#define AUXPRN_TEST_MEMORY_SIZE      0x110000    /* 1MB + HMA                           */
#define AUXPRN_TEST_FLOATING_BUS     0xFF        /* what a port nobody answers reads    */

static BYTE g_Memory[AUXPRN_TEST_MEMORY_SIZE];
static BYTE V86HostRead8(DWORD linear) { return (linear < sizeof g_Memory) ? g_Memory[linear] : 0; }
static VOID V86HostWrite8(DWORD linear, BYTE value)
{ if (linear < sizeof g_Memory) g_Memory[linear] = value; }
static DWORD V86HostIn(WORD port, INT width)
{ (VOID)port; (VOID)width; return AUXPRN_TEST_FLOATING_BUS; }
static VOID V86HostOut(WORD port, INT width, DWORD value) { (VOID)port; (VOID)width; (VOID)value; }

#include "../../src/host/v86interp.h"
#include "dos_layout.h"
#include "dos_auxprn.h"

static INT g_Total = 0, g_Failures = 0;
#define AUXPRN_TEST_CHECK(condition, message) do{ g_Total++; \
    if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

/* The interpreter's register numbering (icpu.r[] and icpu.seg[]). */
#define AUXPRN_TEST_AX               0
#define AUXPRN_TEST_CX               1
#define AUXPRN_TEST_DX               2
#define AUXPRN_TEST_BX               3
#define AUXPRN_TEST_SP               4
#define AUXPRN_TEST_BP               5
#define AUXPRN_TEST_SI               6
#define AUXPRN_TEST_ES               0
#define AUXPRN_TEST_CS               1
#define AUXPRN_TEST_SS               2
#define AUXPRN_TEST_DS               3
#define AUXPRN_TEST_INITIAL_FLAGS    0x0002
#define AUXPRN_TEST_LOW_BYTE         0xFF
#define AUXPRN_TEST_HIGH_WORD        0xFFFF0000u
#define AUXPRN_TEST_AH_SHIFT         8
#define AUXPRN_TEST_PARAGRAPH_SHIFT  4

/* Layout: the driver at DOS_CTAB_SEG:DOS_AUXPRN_OFF; the BIOS "handlers" are a HLT
   (unmodelled -> the interpreter stops there) followed by an IRET; the INT 21h
   caller's return address is another HLT. */
#define AUXPRN_TEST_INT17_SEGMENT    0x2000
#define AUXPRN_TEST_INT14_SEGMENT    0x2100
#define AUXPRN_TEST_RETURN_SEGMENT   0x3000
#define AUXPRN_TEST_DATA_SEGMENT     0x4000
#define AUXPRN_TEST_STACK_SEGMENT    0x5000
#define AUXPRN_TEST_HLT              0xF4
#define AUXPRN_TEST_IRET             0xCF
#define AUXPRN_TEST_HLT_IP           0        /* each handler: HLT at 0 ...            */
#define AUXPRN_TEST_IRET_IP          1        /* ... then IRET                         */
#define AUXPRN_TEST_INT14            0x14
#define AUXPRN_TEST_INT17            0x17
#define AUXPRN_TEST_VECTOR_SIZE      4        /* an IVT entry: offset, then segment    */
#define AUXPRN_TEST_VECTOR_SEGMENT   2
#define AUXPRN_TEST_VECTOR_SEGMENT_HIGH 3

/* The INT 21h frame the stub leaves on the stack: IP, CS, FLAGS, below the top. */
#define AUXPRN_TEST_STACK_TOP        0xFFF0
#define AUXPRN_TEST_FRAME_SIZE       6
#define AUXPRN_TEST_FRAME_IP         6
#define AUXPRN_TEST_FRAME_IP_HIGH    5
#define AUXPRN_TEST_FRAME_CS         4
#define AUXPRN_TEST_FRAME_CS_HIGH    3
#define AUXPRN_TEST_FRAME_FLAGS      2
#define AUXPRN_TEST_FRAME_FLAGS_HIGH 1

/* What the recording BIOS answers. */
#define AUXPRN_TEST_PRINTER_STATUS   0x9000   /* INT 17h: AH = 90h                     */
#define AUXPRN_TEST_SERIAL_SEND      1        /* INT 14h AH=01h: send AL               */
#define AUXPRN_TEST_SERIAL_RECEIVE   2        /* INT 14h AH=02h: receive -> AL         */
#define AUXPRN_TEST_SERIAL_STATUS    3        /* INT 14h AH=03h: status                */
#define AUXPRN_TEST_SEND_STATUS      0x6000
#define AUXPRN_TEST_LINE_STATUS      0x6130
#define AUXPRN_TEST_RECEIVE_SCRIPT   "Qr\rwxyz"

/* AuxPrnTestRunEntry's answers. */
#define AUXPRN_TEST_RETURNED         0
#define AUXPRN_TEST_RAN_AWAY         (-1)
#define AUXPRN_TEST_DERAILED         (-2)
#define AUXPRN_TEST_STEP_LIMIT       100000
#define AUXPRN_TEST_MAX_CALLS        64

/* Registers the driver must give back unchanged. */
#define AUXPRN_TEST_POISON_BX        0xB1B1
#define AUXPRN_TEST_POISON_CX        0xC1C1
#define AUXPRN_TEST_POISON_SI        0x5151
#define AUXPRN_TEST_POISON_BP        0xBBBB
#define AUXPRN_TEST_POISON_ES        0xE5E5
#define AUXPRN_TEST_BUFFER_POISON    0xEE

/* Where the C0h table and the DOS-resident block end. */
#define AUXPRN_TEST_SYSCONF_SIZE     10
#define AUXPRN_TEST_BLOCK_END        0x6F0

typedef struct _AUXPRN_TEST_CALL {
    INT  Vector;
    WORD Ax, Dx;
} AUXPRN_TEST_CALL, *PAUXPRN_TEST_CALL;

static AUXPRN_TEST_CALL g_Calls[AUXPRN_TEST_MAX_CALLS];
static INT g_CallCount;
static PCSTR g_ReceiveScript;
static INT g_ReceiveIndex;

/* Run from the driver entry until the caller's return HLT, answering the BIOS. */
static INT AuxPrnTestRunEntry(V86_CPU *cpu, UINT entry, WORD callerFlags)
{
    DWORD stackTop = ((DWORD)AUXPRN_TEST_STACK_SEGMENT << AUXPRN_TEST_PARAGRAPH_SHIFT)
                     + AUXPRN_TEST_STACK_TOP;
    INT steps = 0;
    g_CallCount = 0;
    /* the INT 21h frame the stub leaves on the stack: IP, CS, FLAGS */
    cpu->Segments[AUXPRN_TEST_SS] = AUXPRN_TEST_STACK_SEGMENT;
    cpu->Registers[AUXPRN_TEST_SP] = AUXPRN_TEST_STACK_TOP - AUXPRN_TEST_FRAME_SIZE;
    g_Memory[stackTop - AUXPRN_TEST_FRAME_IP] = 0;                              /* IP  0000 */
    g_Memory[stackTop - AUXPRN_TEST_FRAME_IP_HIGH] = 0;
    g_Memory[stackTop - AUXPRN_TEST_FRAME_CS] = LOBYTE(AUXPRN_TEST_RETURN_SEGMENT);
    g_Memory[stackTop - AUXPRN_TEST_FRAME_CS_HIGH] = HIBYTE(AUXPRN_TEST_RETURN_SEGMENT);
    g_Memory[stackTop - AUXPRN_TEST_FRAME_FLAGS] = LOBYTE(callerFlags);
    g_Memory[stackTop - AUXPRN_TEST_FRAME_FLAGS_HIGH] = HIBYTE(callerFlags);
    cpu->Segments[AUXPRN_TEST_CS] = DOS_CTAB_SEG; cpu->Ip = (WORD)(DOS_AUXPRN_OFF + entry);
    for (;;) {
        if (++steps > AUXPRN_TEST_STEP_LIMIT) return AUXPRN_TEST_RAN_AWAY;
        if (V86Step(cpu)) continue;
        if (cpu->Segments[AUXPRN_TEST_CS] == AUXPRN_TEST_RETURN_SEGMENT && cpu->Ip == AUXPRN_TEST_HLT_IP)
            return AUXPRN_TEST_RETURNED;                                    /* back home */
        if ((cpu->Segments[AUXPRN_TEST_CS] == AUXPRN_TEST_INT17_SEGMENT
             || cpu->Segments[AUXPRN_TEST_CS] == AUXPRN_TEST_INT14_SEGMENT)
            && cpu->Ip == AUXPRN_TEST_HLT_IP) {
            INT vector = (cpu->Segments[AUXPRN_TEST_CS] == AUXPRN_TEST_INT17_SEGMENT)
                         ? AUXPRN_TEST_INT17 : AUXPRN_TEST_INT14;
            WORD ax = (WORD)cpu->Registers[AUXPRN_TEST_AX], ah = ax >> AUXPRN_TEST_AH_SHIFT;
            if (g_CallCount < AUXPRN_TEST_MAX_CALLS) {
                g_Calls[g_CallCount].Vector = vector; g_Calls[g_CallCount].Ax = ax;
                g_Calls[g_CallCount].Dx = (WORD)cpu->Registers[AUXPRN_TEST_DX]; ++g_CallCount;
            }
            if (vector == AUXPRN_TEST_INT17)
                ax = (WORD)(AUXPRN_TEST_PRINTER_STATUS | (ax & AUXPRN_TEST_LOW_BYTE));
            else if (ah == AUXPRN_TEST_SERIAL_SEND)
                ax = (WORD)(AUXPRN_TEST_SEND_STATUS | (ax & AUXPRN_TEST_LOW_BYTE));
            else if (ah == AUXPRN_TEST_SERIAL_RECEIVE)
                ax = (WORD)(g_ReceiveScript[g_ReceiveIndex++] & AUXPRN_TEST_LOW_BYTE);
            else if (ah == AUXPRN_TEST_SERIAL_STATUS) ax = AUXPRN_TEST_LINE_STATUS;
            cpu->Registers[AUXPRN_TEST_AX] = (cpu->Registers[AUXPRN_TEST_AX] & AUXPRN_TEST_HIGH_WORD) | ax;
            cpu->Ip = AUXPRN_TEST_IRET_IP;                                  /* the IRET */
            continue;
        }
        return AUXPRN_TEST_DERAILED;
    }
}

static DWORD AuxPrnTestLinear(WORD segment, WORD offset)
{
    return ((DWORD)segment << AUXPRN_TEST_PARAGRAPH_SHIFT) + offset;
}

static V86_CPU AuxPrnTestSetup(VOID)
{
    V86_CPU cpu; memset(&cpu, 0, sizeof cpu);
    memset(g_Memory, 0, sizeof g_Memory);
    memcpy(g_Memory + AuxPrnTestLinear(DOS_CTAB_SEG, DOS_AUXPRN_OFF), g_DosAuxPrnCode,
           sizeof g_DosAuxPrnCode);
    g_Memory[AuxPrnTestLinear(AUXPRN_TEST_INT17_SEGMENT, AUXPRN_TEST_HLT_IP)] = AUXPRN_TEST_HLT;
    g_Memory[AuxPrnTestLinear(AUXPRN_TEST_INT17_SEGMENT, AUXPRN_TEST_IRET_IP)] = AUXPRN_TEST_IRET;
    g_Memory[AuxPrnTestLinear(AUXPRN_TEST_INT14_SEGMENT, AUXPRN_TEST_HLT_IP)] = AUXPRN_TEST_HLT;
    g_Memory[AuxPrnTestLinear(AUXPRN_TEST_INT14_SEGMENT, AUXPRN_TEST_IRET_IP)] = AUXPRN_TEST_IRET;
    g_Memory[AuxPrnTestLinear(AUXPRN_TEST_RETURN_SEGMENT, AUXPRN_TEST_HLT_IP)] = AUXPRN_TEST_HLT;
    g_Memory[AUXPRN_TEST_INT17 * AUXPRN_TEST_VECTOR_SIZE + AUXPRN_TEST_VECTOR_SEGMENT] =
        LOBYTE(AUXPRN_TEST_INT17_SEGMENT);
    g_Memory[AUXPRN_TEST_INT17 * AUXPRN_TEST_VECTOR_SIZE + AUXPRN_TEST_VECTOR_SEGMENT_HIGH] =
        HIBYTE(AUXPRN_TEST_INT17_SEGMENT);
    g_Memory[AUXPRN_TEST_INT14 * AUXPRN_TEST_VECTOR_SIZE + AUXPRN_TEST_VECTOR_SEGMENT] =
        LOBYTE(AUXPRN_TEST_INT14_SEGMENT);
    g_Memory[AUXPRN_TEST_INT14 * AUXPRN_TEST_VECTOR_SIZE + AUXPRN_TEST_VECTOR_SEGMENT_HIGH] =
        HIBYTE(AUXPRN_TEST_INT14_SEGMENT);
    cpu.Flags = AUXPRN_TEST_INITIAL_FLAGS;
    cpu.Registers[AUXPRN_TEST_BX] = AUXPRN_TEST_POISON_BX; cpu.Registers[AUXPRN_TEST_CX] = AUXPRN_TEST_POISON_CX;
    cpu.Registers[AUXPRN_TEST_SI] = AUXPRN_TEST_POISON_SI; cpu.Registers[AUXPRN_TEST_BP] = AUXPRN_TEST_POISON_BP;
    cpu.Segments[AUXPRN_TEST_DS] = AUXPRN_TEST_DATA_SEGMENT;
    cpu.Segments[AUXPRN_TEST_ES] = AUXPRN_TEST_POISON_ES;
    g_ReceiveScript = AUXPRN_TEST_RECEIVE_SCRIPT; g_ReceiveIndex = 0;
    return cpu;
}

static BOOL AuxPrnTestCallIs(INT callIndex, INT vector, WORD ax, WORD dx)
{
    return callIndex < g_CallCount && g_Calls[callIndex].Vector == vector
        && g_Calls[callIndex].Ax == ax && g_Calls[callIndex].Dx == dx;
}

/* The registers each case loads, and what it expects back. */
#define AUXPRN_TEST_AX_05H           0x05A5   /* AH=05h, AL poisoned                   */
#define AUXPRN_TEST_AX_04H           0x04A5
#define AUXPRN_TEST_AX_03H           0x03A5
#define AUXPRN_TEST_AX_40H           0x4000
#define AUXPRN_TEST_AX_3FH           0x3F00
#define AUXPRN_TEST_DX_CHAR_P        0xD150   /* DL = 'P'                              */
#define AUXPRN_TEST_DX_CHAR_A        0xD141   /* DL = 'A'                              */
#define AUXPRN_TEST_DX_POISON        0xD1D1
#define AUXPRN_TEST_HANDLE_AUX       3
#define AUXPRN_TEST_HANDLE_PRN       4
#define AUXPRN_TEST_BUFFER           0x0100
#define AUXPRN_TEST_READ_BUFFER      0x0200
#define AUXPRN_TEST_READ_BUFFER_SIZE 8
#define AUXPRN_TEST_FLAGS_CF_SET     0x0003
#define AUXPRN_TEST_FLAGS_CF_CLEAR   0x0002
#define AUXPRN_TEST_CR               0x0D

int main(void)
{
    V86_CPU cpu;
    DWORD dataBase = (DWORD)AUXPRN_TEST_DATA_SEGMENT << AUXPRN_TEST_PARAGRAPH_SHIFT;
    printf("== auxprn_test: DOS AUX/PRN driver code (#251)\n");
    AUXPRN_TEST_CHECK(sizeof g_DosAuxPrnCode <= DOS_AUXPRN_LEN, "fits its reservation");
    AUXPRN_TEST_CHECK(DOS_AUXPRN_OFF >= DOS_SYSCONF_OFF + AUXPRN_TEST_SYSCONF_SIZE
                      && DOS_AUXPRN_OFF + DOS_AUXPRN_LEN <= AUXPRN_TEST_BLOCK_END,
                      "between the C0h table and the block's end");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_05H;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_DX_CHAR_P;
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_PRN_OUTPUT, AUXPRN_TEST_FLAGS_CF_SET)
                      == AUXPRN_TEST_RETURNED, "05h: returns to the caller");
    AUXPRN_TEST_CHECK(g_CallCount == 3 && AuxPrnTestCallIs(0, AUXPRN_TEST_INT17, 0x0200, 0)
                      && AuxPrnTestCallIs(1, AUXPRN_TEST_INT17, 0x0200, 0)
                      && AuxPrnTestCallIs(2, AUXPRN_TEST_INT17, 0x0050, 0),
                      "05h: INT 17h 02h, 02h, 00h AL='P' DX=0 (6.22's sequence)");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_AX] == 0x0550, "05h: AX = 05:char");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_BX] == AUXPRN_TEST_POISON_BX
                      && (WORD)cpu.Registers[AUXPRN_TEST_CX] == AUXPRN_TEST_POISON_CX
                      && (WORD)cpu.Registers[AUXPRN_TEST_DX] == AUXPRN_TEST_DX_CHAR_P,
                      "05h: BX CX DX unchanged");
    AUXPRN_TEST_CHECK(cpu.Registers[AUXPRN_TEST_SP] == AUXPRN_TEST_STACK_TOP,
                      "05h: stack balanced (the INT 21h frame popped)");
    AUXPRN_TEST_CHECK(cpu.Flags & V86_CF,
                      "05h: the caller's flags come back as they were (CF set in)");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_04H;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_DX_CHAR_A;
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_AUX_OUTPUT, AUXPRN_TEST_FLAGS_CF_CLEAR)
                      == AUXPRN_TEST_RETURNED, "04h: returns");
    AUXPRN_TEST_CHECK(g_CallCount == 2 && AuxPrnTestCallIs(0, AUXPRN_TEST_INT14, 0x0300, 0)
                      && AuxPrnTestCallIs(1, AUXPRN_TEST_INT14, 0x0141, 0),
                      "04h: INT 14h 03h, then 01h AL='A' DX=0");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_AX] == 0x0441
                      && (WORD)cpu.Registers[AUXPRN_TEST_DX] == AUXPRN_TEST_DX_CHAR_A,
                      "04h: AX = 04:char, DX kept");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_03H;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_DX_POISON;
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_AUX_INPUT, AUXPRN_TEST_FLAGS_CF_CLEAR)
                      == AUXPRN_TEST_RETURNED, "03h: returns");
    AUXPRN_TEST_CHECK(g_CallCount == 2 && AuxPrnTestCallIs(0, AUXPRN_TEST_INT14, 0x0300, 0)
                      && AuxPrnTestCallIs(1, AUXPRN_TEST_INT14, 0x0230, 0),
                      "03h: INT 14h 03h, then 02h with AL left from the status (0230h)");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_AX] == 0x0351
                      && (WORD)cpu.Registers[AUXPRN_TEST_DX] == AUXPRN_TEST_DX_POISON,
                      "03h: AX = 03:'Q', DX kept");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_40H;
    cpu.Registers[AUXPRN_TEST_BX] = AUXPRN_TEST_HANDLE_PRN; cpu.Registers[AUXPRN_TEST_CX] = 2;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_BUFFER;
    g_Memory[dataBase + AUXPRN_TEST_BUFFER] = 'P';
    g_Memory[dataBase + AUXPRN_TEST_BUFFER + 1] = 'Q';
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_PRN_WRITE, AUXPRN_TEST_FLAGS_CF_SET)
                      == AUXPRN_TEST_RETURNED, "40h h4: returns");
    AUXPRN_TEST_CHECK(g_CallCount == 4 && AuxPrnTestCallIs(0, AUXPRN_TEST_INT17, 0x0200, 0)
                      && AuxPrnTestCallIs(1, AUXPRN_TEST_INT17, 0x0050, 0)
                      && AuxPrnTestCallIs(2, AUXPRN_TEST_INT17, 0x0200, 0)
                      && AuxPrnTestCallIs(3, AUXPRN_TEST_INT17, 0x0051, 0),
                      "40h h4: per byte INT 17h 02h then 00h");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_AX] == 2 && !(cpu.Flags & V86_CF),
                      "40h h4: AX=2, CF cleared");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_CX] == 2
                      && (WORD)cpu.Registers[AUXPRN_TEST_DX] == AUXPRN_TEST_BUFFER
                      && (WORD)cpu.Registers[AUXPRN_TEST_SI] == AUXPRN_TEST_POISON_SI,
                      "40h h4: CX DX SI unchanged");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_40H;
    cpu.Registers[AUXPRN_TEST_BX] = AUXPRN_TEST_HANDLE_AUX; cpu.Registers[AUXPRN_TEST_CX] = 0;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_BUFFER;
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_PRN_WRITE, AUXPRN_TEST_FLAGS_CF_SET)
                      == AUXPRN_TEST_RETURNED && g_CallCount == 0
                      && (WORD)cpu.Registers[AUXPRN_TEST_AX] == 0 && !(cpu.Flags & V86_CF),
                      "40h h4: CX=0 writes nothing, AX=0");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_40H;
    cpu.Registers[AUXPRN_TEST_BX] = AUXPRN_TEST_HANDLE_AUX; cpu.Registers[AUXPRN_TEST_CX] = 2;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_BUFFER;
    g_Memory[dataBase + AUXPRN_TEST_BUFFER] = 'A';
    g_Memory[dataBase + AUXPRN_TEST_BUFFER + 1] = 'B';
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_AUX_WRITE, AUXPRN_TEST_FLAGS_CF_SET)
                      == AUXPRN_TEST_RETURNED, "40h h3: returns");
    AUXPRN_TEST_CHECK(g_CallCount == 2 && AuxPrnTestCallIs(0, AUXPRN_TEST_INT14, 0x0141, 0)
                      && AuxPrnTestCallIs(1, AUXPRN_TEST_INT14, 0x0142, 0),
                      "40h h3: per byte INT 14h 01h, no status");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_AX] == 2 && !(cpu.Flags & V86_CF),
                      "40h h3: AX=2, CF cleared");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_3FH;
    cpu.Registers[AUXPRN_TEST_BX] = AUXPRN_TEST_HANDLE_AUX; cpu.Registers[AUXPRN_TEST_CX] = 6;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_READ_BUFFER; g_ReceiveIndex = 1;
    memset(g_Memory + dataBase + AUXPRN_TEST_READ_BUFFER, AUXPRN_TEST_BUFFER_POISON,
           AUXPRN_TEST_READ_BUFFER_SIZE);
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_AUX_READ, AUXPRN_TEST_FLAGS_CF_SET)
                      == AUXPRN_TEST_RETURNED, "3Fh h3: returns");
    AUXPRN_TEST_CHECK(g_CallCount == 2 && AuxPrnTestCallIs(0, AUXPRN_TEST_INT14, 0x0200, 0)
                      && AuxPrnTestCallIs(1, AUXPRN_TEST_INT14, 0x0200, 0),
                      "3Fh h3: INT 14h 02h per byte, stops after the CR");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_AX] == 2 && !(cpu.Flags & V86_CF),
                      "3Fh h3: AX=2, CF cleared");
    AUXPRN_TEST_CHECK(g_Memory[dataBase + AUXPRN_TEST_READ_BUFFER] == 'r'
                      && g_Memory[dataBase + AUXPRN_TEST_READ_BUFFER + 1] == AUXPRN_TEST_CR
                      && g_Memory[dataBase + AUXPRN_TEST_READ_BUFFER + 2]
                         == AUXPRN_TEST_BUFFER_POISON,
                      "3Fh h3: buffer 'r' CR, nothing after");
    AUXPRN_TEST_CHECK((WORD)cpu.Registers[AUXPRN_TEST_BX] == AUXPRN_TEST_HANDLE_AUX
                      && (WORD)cpu.Registers[AUXPRN_TEST_CX] == 6
                      && (WORD)cpu.Registers[AUXPRN_TEST_DX] == AUXPRN_TEST_READ_BUFFER,
                      "3Fh h3: BX CX DX unchanged");

    cpu = AuxPrnTestSetup(); cpu.Registers[AUXPRN_TEST_AX] = AUXPRN_TEST_AX_3FH;
    cpu.Registers[AUXPRN_TEST_BX] = AUXPRN_TEST_HANDLE_AUX; cpu.Registers[AUXPRN_TEST_CX] = 3;
    cpu.Registers[AUXPRN_TEST_DX] = AUXPRN_TEST_READ_BUFFER; g_ReceiveIndex = 3;
    AUXPRN_TEST_CHECK(AuxPrnTestRunEntry(&cpu, DOS_AUXPRN_AUX_READ, AUXPRN_TEST_FLAGS_CF_CLEAR)
                      == AUXPRN_TEST_RETURNED && g_CallCount == 3
                      && (WORD)cpu.Registers[AUXPRN_TEST_AX] == 3,
                      "3Fh h3: no CR -> stops at CX");

    printf("\n%d checks, %d failed\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}

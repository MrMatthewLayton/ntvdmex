/* dos_extmem.h -- where an INT 15h AH=87h address points.  GH #54.
 *
 * AH=87h copies between two LINEAR addresses the caller puts in a GDT. On a real
 * AT that is physical memory, and above 1 MB it is the same memory HIMEM hands out
 * as XMS. This host has no such memory: an XMS block (EMB) is a buffer on the HOST
 * heap (dos_xms.h), so a guest linear address above the HMA is not memory we own --
 * it is whatever the host process happens to have there. Copying to it blindly
 * would let any DOS program write over the host.
 *
 * So every address is RESOLVED, and one that resolves to nothing is refused:
 *   1. below 0x10FFF0 (conventional memory + the HMA) -> the guest's own memory,
 *      which in the VDM is identity-mapped at host VA 0;
 *   2. inside an allocated EMB's range (the address XMS AH=0Ch lock handed out)
 *      -> that block. This is what makes "lock a block, then move with 87h" work,
 *      as it does on a real machine;
 *   3. otherwise, 0x110000..0x1000000 -> a private buffer standing for the 15 MB
 *      of raw extended memory INT 15h AH=88h already claims. The host allocates it
 *      on first use;
 *   4. anything else -> refused.
 * A range may not straddle two of these.
 *
 * No Windows calls, only Windows types (src/ntvdmex_types.h), like dos_xms.h; tested
 * off-VM by tests/unit/extmem_test.c.
 */
#ifndef NTVDMEX_DOS_EXTMEM_H
#define NTVDMEX_DOS_EXTMEM_H

#include "../ntvdmex_types.h"
#include "dos_xms.h"

#define DOS_EXTMEM_DIRECT_END  0x0010FFF0u   /* FFFF:FFFF + 1: top of the HMA          */
#define DOS_EXTMEM_RAW_BASE    0x00100000u   /* raw buffer index 0 = linear 1 MB        */
#define DOS_EXTMEM_RAW_END     0x01000000u   /* 16 MB: 1 MB + AH=88h's 15 MB            */
#define DOS_EXTMEM_RAW_LENGTH  (DOS_EXTMEM_RAW_END - DOS_EXTMEM_RAW_BASE)

enum { DOS_EXTMEM_REGION_NONE = 0, DOS_EXTMEM_REGION_DIRECT, DOS_EXTMEM_REGION_EMB,
       DOS_EXTMEM_REGION_RAW };

/* An AH=87h GDT descriptor (8 bytes; the GDT holds six) and where its base sits:
   bits 0-23 in bytes 2-4, bits 24-31 in byte 7. */
#define DOS_EXTMEM_DESCRIPTOR_SIZE        8
#define DOS_EXTMEM_DESCRIPTOR_BASE_BYTE0  2
#define DOS_EXTMEM_DESCRIPTOR_BASE_BYTE1  3
#define DOS_EXTMEM_DESCRIPTOR_BASE_BYTE2  4
#define DOS_EXTMEM_DESCRIPTOR_BASE_BYTE3  7
#define DOS_EXTMEM_BYTE1_SHIFT            8
#define DOS_EXTMEM_BYTE2_SHIFT            16
#define DOS_EXTMEM_BYTE3_SHIFT            24

/* Which region [linearAddress, linearAddress+length) lies in. length > 0. */
static inline INT DosExtMemClassify(_In_opt_ PCDOS_XMS_STATE xmsState, _In_ DWORD linearAddress,
                                    _In_ DWORD length)
{
    DWORD end = linearAddress + length;
    INT handleIndex;
    if (length == 0 || end < linearAddress) return DOS_EXTMEM_REGION_NONE;    /* wraps 4 GB */
    if (end <= DOS_EXTMEM_DIRECT_END) return DOS_EXTMEM_REGION_DIRECT;
    if (xmsState) {
        for (handleIndex = 0; handleIndex < DOS_XMS_MAX_HANDLES; ++handleIndex) {
            PCDOS_XMS_HANDLE handleEntry = &xmsState->Handles[handleIndex];
            DWORD blockBase, blockEnd;
            if (!handleEntry->InUse || !handleEntry->Memory || !handleEntry->SizeKb) continue;
            blockBase = (DWORD)(UINT_PTR)handleEntry->Memory;
            blockEnd = blockBase + handleEntry->SizeKb * DOS_XMS_BYTES_PER_KB;
            if (linearAddress >= blockBase && end <= blockEnd && blockEnd > blockBase)
                return DOS_EXTMEM_REGION_EMB;
        }
    }
    if (linearAddress >= DOS_EXTMEM_DIRECT_END && end <= DOS_EXTMEM_RAW_END)
        return DOS_EXTMEM_REGION_RAW;
    return DOS_EXTMEM_REGION_NONE;
}

/* Host pointer for [linearAddress, linearAddress+length), or 0. `conventionalBase` is
   the host address of guest linear 0, `rawBuffer` the raw buffer (DOS_EXTMEM_RAW_LENGTH
   bytes; may be 0 until first needed -- then a RAW range resolves to 0 and the caller
   allocates and asks again). */
static inline PBYTE DosExtMemResolve(_In_opt_ PCDOS_XMS_STATE xmsState,
                                     _In_ UINT_PTR conventionalBase,
                                     _In_opt_ PBYTE rawBuffer, _In_ DWORD linearAddress,
                                     _In_ DWORD length)
{
    switch (DosExtMemClassify(xmsState, linearAddress, length)) {
    case DOS_EXTMEM_REGION_DIRECT: return (PBYTE)(conventionalBase + linearAddress);
    case DOS_EXTMEM_REGION_EMB:    return (PBYTE)(UINT_PTR)linearAddress;
    case DOS_EXTMEM_REGION_RAW:
        return rawBuffer ? rawBuffer + (linearAddress - DOS_EXTMEM_RAW_BASE) : 0;
    default:                       return 0;
    }
}

/* The source and destination bases from an AH=87h GDT (48 bytes at `gdt`):
   descriptor 2 (+0x10) is the source, 3 (+0x18) the destination; base bits 0-23 at
   +2..+4 and 24-31 at +7 (386 BIOSes honour the high byte). */
static inline DWORD DosExtMemDescriptorBase(
    _In_reads_bytes_(DOS_EXTMEM_DESCRIPTOR_SIZE) const volatile BYTE *descriptor)
{
    return (DWORD)descriptor[DOS_EXTMEM_DESCRIPTOR_BASE_BYTE0]
         | ((DWORD)descriptor[DOS_EXTMEM_DESCRIPTOR_BASE_BYTE1] << DOS_EXTMEM_BYTE1_SHIFT)
         | ((DWORD)descriptor[DOS_EXTMEM_DESCRIPTOR_BASE_BYTE2] << DOS_EXTMEM_BYTE2_SHIFT)
         | ((DWORD)descriptor[DOS_EXTMEM_DESCRIPTOR_BASE_BYTE3] << DOS_EXTMEM_BYTE3_SHIFT);
}

#endif /* NTVDMEX_DOS_EXTMEM_H */

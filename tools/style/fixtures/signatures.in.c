/* signatures.c -- a signature fits on one line, or takes one parameter a line. */

#include "signatures.h"

static INT SignatureShort(INT left,
                          INT right)
{
    return left + right;
}

INT SignatureLong(PSIGNATURE_MACHINE machine, DWORD linearAddress, DWORD byteCount, BOOL isWrite, PDWORD faultCode)
{
    return SignatureShort((INT)linearAddress, (INT)byteCount);
}

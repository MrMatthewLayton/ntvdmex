/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * DOS 8.3 aliases for names that have no short name.
 *
 * The function definitions of dos_alias.h, which keeps their declarations and doc comments.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include "dos_alias.h"

#define DOS_ALIAS_SUBSTITUTE        '_'     /* What Windows puts for a character DOS refuses */
#define DOS_ALIAS_DECIMAL_DIGITS    7       /* "~" and up to six digits */
#define DOS_ALIAS_ASCII_LIMIT       0x80 /* An alias keeps to ASCII: the host and the guest code pages differ above it */

static CHAR DosAliasUpper(CHAR character)
{
    return (character >= 'a' && character <= 'z') ? (CHAR)(character - ASCII_CASE_BIT) : character;
}

/* A character no DOS name may hold: controls and space, and DOS's separators. */
static INT DosAliasIsBadCharacter(BYTE character)
{
    if (character <= ASCII_SPACE)
        return 1;

    return character == '"' || character == '*' || character == '+' || character == ',' || character == '/'
        || character == ':' || character == ';' || character == '<' || character == '=' || character == '>'
        || character == '?' || character == '[' || character == '\\' || character == ']' || character == '|';
}

static INT DosAliasSameText(PCSTR first, PCSTR second, INT length)
{
    INT index;

    for (index = 0; length < 0 || index < length; ++index)
    {
        if (DosAliasUpper(first[index]) != DosAliasUpper(second[index]))
            return 0;

        if (!first[index])
            return 1;
    }

    return !second[index];
}

INT DosAliasIsShortName(PCSTR name)
{
    INT baseLength = 0;
    INT extensionLength = -1;
    INT index;

    if (!name[0] || name[0] == '.')
        return 0;

    for (index = 0; name[index]; ++index)
    {
        if (name[index] == '.')
        {
            if (extensionLength >= 0)
                return 0;

            extensionLength = 0;
            continue;
        }

        if (DosAliasIsBadCharacter((BYTE)name[index]))
            return 0;

        if (extensionLength >= 0)
        {
            if (++extensionLength > DOS_ALIAS_EXTENSION_LENGTH)
                return 0;
        }
        else if (++baseLength > DOS_ALIAS_BASE_LENGTH)
            return 0;
    }

    return baseLength > 0;
}

INT DosAliasLooksLikeAlias(PCSTR component, INT length)
{
    INT index;

    for (index = 0; index + 1 < length && component[index + 1]; ++index)
        if (component[index] == '~' && component[index + 1] >= '0' && component[index + 1] <= '9')
            return 1;

    return 0;
}

/* One character of an alias: upper case, ASCII only, DOS-legal ('_' for the rest). */
static CHAR DosAliasCharacter(CHAR character)
{
    if ((BYTE)character >= DOS_ALIAS_ASCII_LIMIT || DosAliasIsBadCharacter((BYTE)character))
        return DOS_ALIAS_SUBSTITUTE;

    return DosAliasUpper(character);
}

VOID DosAliasMake(PCSTR longName, DWORD number, PSTR out)
{
    CHAR base[DOS_ALIAS_BASE_LENGTH + 1];
    CHAR extension[DOS_ALIAS_EXTENSION_LENGTH + 1];
    CHAR suffix[DOS_ALIAS_DECIMAL_DIGITS + 1];
    INT baseLength = 0;
    INT extensionLength = 0;
    INT suffixLength = 0;
    INT lastDot = -1;
    INT first = 0;
    INT stemLength;
    INT length = 0;
    INT index;

    while (longName[first] == '.' || longName[first] == ' ')
        ++first;                                        /* ".bashrc" -> BASHRC~1 */

    for (index = first; longName[index]; ++index)
        if (longName[index] == '.')
            lastDot = index;

    for (index = first; longName[index] && (lastDot < 0 || index < lastDot); ++index)
        if (longName[index] != ' ' && longName[index] != '.' && baseLength < DOS_ALIAS_BASE_LENGTH)
            base[baseLength++] = DosAliasCharacter(longName[index]);

    if (lastDot >= 0)
        for (index = lastDot + 1; longName[index] && extensionLength < DOS_ALIAS_EXTENSION_LENGTH; ++index)
            if (longName[index] != ' ' && longName[index] != '.')
                extension[extensionLength++] = DosAliasCharacter(longName[index]);

    if (!baseLength)
        base[baseLength++] = DOS_ALIAS_SUBSTITUTE;

    if (number > DOS_ALIAS_MAX_NUMBER)
        number = DOS_ALIAS_MAX_NUMBER;

    {   CHAR digits[DOS_ALIAS_DECIMAL_DIGITS];
        INT digitCount = 0;

        do
        {
            digits[digitCount++] = (CHAR)('0' + number % 10);
            number /= 10;
        } while (number);

        suffix[suffixLength++] = '~';

        while (digitCount)
            suffix[suffixLength++] = digits[--digitCount];
    }

    stemLength = DOS_ALIAS_BASE_LENGTH - suffixLength;

    if (stemLength > baseLength)
        stemLength = baseLength;

    for (index = 0; index < stemLength; ++index)
        out[length++] = base[index];

    for (index = 0; index < suffixLength; ++index)
        out[length++] = suffix[index];

    if (extensionLength)
    {
        out[length++] = '.';

        for (index = 0; index < extensionLength; ++index)
            out[length++] = extension[index];
    }

    out[length] = 0;
}

VOID DosAliasTableBegin(PDOS_ALIAS_TABLE table, PCSTR directory)
{
    INT index;

    for (index = 0; directory[index] && index < DOS_ALIAS_DIRECTORY_SIZE - 1; ++index)
        table->Directory[index] = directory[index];

    table->Directory[index] = 0;
    table->TakenCount = 0;
    table->NeedyCount = 0;
    table->PoolUsed = 0;
    table->IsOverflow = 0;
}

static VOID DosAliasTake(PDOS_ALIAS_TABLE table, PCSTR name)
{
    INT index;

    if (table->TakenCount >= DOS_ALIAS_MAX_TAKEN)
    {
        table->IsOverflow = 1;
        return;
    }

    for (index = 0; name[index] && index < DOS_ALIAS_NAME_SIZE - 1; ++index)
        table->Taken[table->TakenCount][index] = DosAliasUpper(name[index]);

    table->Taken[table->TakenCount][index] = 0;
    ++table->TakenCount;
}

VOID DosAliasTableNote(PDOS_ALIAS_TABLE table, PCSTR longName, PCSTR shortName)
{
    DWORD length = 0;
    DWORD index;

    if (longName[0] == '.' && (!longName[1] || (longName[1] == '.' && !longName[2])))
        return;                                         /* "." and ".." */

    if (shortName && shortName[0])
    {
        DosAliasTake(table, shortName);
        return;
    }

    if (DosAliasIsShortName(longName))
    {
        DosAliasTake(table, longName);
        return;
    }

    while (longName[length])
        ++length;

    if (table->NeedyCount >= DOS_ALIAS_MAX_NEEDY || table->PoolUsed + length + 1 > DOS_ALIAS_POOL_SIZE)
    {
        table->IsOverflow = 1;
        return;
    }

    table->NeedyName[table->NeedyCount] = table->PoolUsed;

    for (index = 0; index <= length; ++index)
        table->Pool[table->PoolUsed++] = longName[index];

    table->NeedyAlias[table->NeedyCount][0] = 0;
    ++table->NeedyCount;
}

/* Is `alias` a name the directory already has, or one already handed out? */
static INT DosAliasIsTaken(PCDOS_ALIAS_TABLE table, PCSTR alias, INT assigned)
{
    INT index;

    for (index = 0; index < table->TakenCount; ++index)
        if (DosAliasSameText(table->Taken[index], alias, -1))
            return 1;

    for (index = 0; index < assigned; ++index)
        if (DosAliasSameText(table->NeedyAlias[index], alias, -1))
            return 1;

    return 0;
}

VOID DosAliasTableFinish(PDOS_ALIAS_TABLE table)
{
    INT entry;

    for (entry = 0; entry < table->NeedyCount; ++entry)
    {
        DWORD number;

        for (number = 1; number <= DOS_ALIAS_MAX_NUMBER; ++number)
        {
            DosAliasMake(table->Pool + table->NeedyName[entry], number, table->NeedyAlias[entry]);

            if (!DosAliasIsTaken(table, table->NeedyAlias[entry], entry))
                break;
        }
    }
}

PCSTR DosAliasTableAliasOf(PCDOS_ALIAS_TABLE table, PCSTR longName)
{
    INT index;

    for (index = 0; index < table->NeedyCount; ++index)
        if (table->NeedyAlias[index][0] && DosAliasSameText(table->Pool + table->NeedyName[index], longName, -1))
            return table->NeedyAlias[index];

    return NULL;
}

PCSTR DosAliasTableLongOf(PCDOS_ALIAS_TABLE table, PCSTR alias, INT length)
{
    INT index;

    for (index = 0; index < table->NeedyCount; ++index)
        if (table->NeedyAlias[index][0] && DosAliasSameText(alias, table->NeedyAlias[index], length))
            return table->Pool + table->NeedyName[index];

    return NULL;
}

INT DosAliasTableIsFor(PCDOS_ALIAS_TABLE table, PCSTR directory)
{
    return DosAliasSameText(table->Directory, directory, -1);
}

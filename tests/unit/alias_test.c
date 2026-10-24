/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * alias_test.c -- DOS 8.3 aliases for names with no short name (src/dos/dos_alias.c).
 *
 * The directory the s101 rig report named is the first case: \\server\storage at Z:, a
 * share that gives no short names, holding "Files", "Movies", "Music" (8.3 already) and
 * "Documentaries", "Television", "Movie Featurettes" (which DOS could not see at all).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "dos_alias.h"

#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
                                    else{g_Failed++; printf("  FAIL  %s\n",(message));} }while(0)

static INT g_Total;
static INT g_Failed;
static DOS_ALIAS_TABLE g_Table;

static INT AliasTestIs(PCSTR got, PCSTR wanted)
{
    return got && strcmp(got, wanted) == 0;
}

static VOID AliasTestShortNames(VOID)
{
    printf("which names are already 8.3\n");
    CHECK(DosAliasIsShortName("Files"), "Files: 8.3 (either case)");
    CHECK(DosAliasIsShortName("README.TXT"), "README.TXT");
    CHECK(DosAliasIsShortName("A.B"), "A.B");
    CHECK(!DosAliasIsShortName("Documentaries"), "Documentaries: nine before the dot");
    CHECK(!DosAliasIsShortName("Movie Featurettes"), "a space");
    CHECK(!DosAliasIsShortName("page.html"), "an extension of four");
    CHECK(!DosAliasIsShortName("a.b.c"), "two dots");
    CHECK(!DosAliasIsShortName(".bashrc"), "a leading dot");
    CHECK(!DosAliasIsShortName("a+b"), "a DOS separator");
    CHECK(!DosAliasIsShortName(""), "empty");
    CHECK(DosAliasLooksLikeAlias("DOCUME~1", 8), "DOCUME~1 looks like an alias");
    CHECK(!DosAliasLooksLikeAlias("DOCUME~X", 8), "~ with no digit does not");
    CHECK(!DosAliasLooksLikeAlias("DOCUME~1", 6), "only the given length is looked at");
}

static VOID AliasTestMake(VOID)
{
    CHAR alias[DOS_ALIAS_NAME_SIZE];

    printf("making one alias\n");
    DosAliasMake("Documentaries", 1, alias);
    CHECK(strcmp(alias, "DOCUME~1") == 0, "Documentaries -> DOCUME~1");
    DosAliasMake("Movie Featurettes", 1, alias);
    CHECK(strcmp(alias, "MOVIEF~1") == 0, "spaces dropped: MOVIEF~1");
    DosAliasMake("index.html", 1, alias);
    CHECK(strcmp(alias, "INDEX~1.HTM") == 0, "extension cut to three: INDEX~1.HTM");
    DosAliasMake("my.long.file.name.txt", 1, alias);
    CHECK(strcmp(alias, "MYLONG~1.TXT") == 0, "the LAST dot starts the extension; others dropped");
    DosAliasMake(".bashrc", 1, alias);
    CHECK(strcmp(alias, "BASHRC~1") == 0, "a leading dot is not an extension");
    DosAliasMake("a+b=c;d", 1, alias);
    CHECK(strcmp(alias, "A_B_C_~1") == 0, "DOS separators become '_'");
    DosAliasMake("caf\xe9 menu", 1, alias);
    CHECK(strcmp(alias, "CAF_ME~1") == 0, "outside ASCII becomes '_'");
    DosAliasMake("Documentaries", 10, alias);
    CHECK(strcmp(alias, "DOCUM~10") == 0, "~10 takes a stem letter");
    DosAliasMake("Documentaries", 123456, alias);
    CHECK(strcmp(alias, "D~123456") == 0, "a six-digit number leaves one");
    DosAliasMake("   ", 1, alias);
    CHECK(strcmp(alias, "_~1") == 0, "nothing usable: '_'");
}

static VOID AliasTestTable(VOID)
{
    printf("one directory (the rig's Z:)\n");
    DosAliasTableBegin(&g_Table, "Z:\\");
    DosAliasTableNote(&g_Table, ".", "");
    DosAliasTableNote(&g_Table, "..", "");
    DosAliasTableNote(&g_Table, "Documentaries", "");
    DosAliasTableNote(&g_Table, "Files", "");
    DosAliasTableNote(&g_Table, "Movie Featurettes", "");
    DosAliasTableNote(&g_Table, "Movies", "");
    DosAliasTableNote(&g_Table, "Music", "");
    DosAliasTableNote(&g_Table, "Television", "");
    DosAliasTableNote(&g_Table, "Documents", "");
    DosAliasTableFinish(&g_Table);
    CHECK(AliasTestIs(DosAliasTableAliasOf(&g_Table, "Documentaries"), "DOCUME~1"), "Documentaries -> DOCUME~1");
    CHECK(AliasTestIs(DosAliasTableAliasOf(&g_Table, "Documents"), "DOCUME~2"), "a collision -> DOCUME~2");
    CHECK(AliasTestIs(DosAliasTableAliasOf(&g_Table, "Television"), "TELEVI~1"), "Television -> TELEVI~1");
    CHECK(AliasTestIs(DosAliasTableAliasOf(&g_Table, "movie featurettes"), "MOVIEF~1"), "looked up case-blind");
    CHECK(DosAliasTableAliasOf(&g_Table, "Files") == NULL, "an 8.3 name is given no alias");
    CHECK(AliasTestIs(DosAliasTableLongOf(&g_Table, "docume~2", 8), "Documents"), "back: docume~2 -> Documents");
    CHECK(AliasTestIs(DosAliasTableLongOf(&g_Table, "TELEVI~1\\SHOW.AVI", 8), "Television"), "back: a component of a longer path");
    CHECK(DosAliasTableLongOf(&g_Table, "DOCUME~3", 8) == NULL, "an alias nobody has -> none");
    CHECK(DosAliasTableIsFor(&g_Table, "z:\\"), "the table knows its directory");

    printf("names the volume already has are respected\n");
    DosAliasTableBegin(&g_Table, "C:\\DATA\\");
    DosAliasTableNote(&g_Table, "Program Files", "PROGRA~1");
    DosAliasTableNote(&g_Table, "DOCUME~1", "");
    DosAliasTableNote(&g_Table, "Documentaries", "");
    DosAliasTableNote(&g_Table, "Programs and things", "");
    DosAliasTableFinish(&g_Table);
    CHECK(DosAliasTableAliasOf(&g_Table, "Program Files") == NULL, "an entry with a short name keeps it (no alias)");
    CHECK(AliasTestIs(DosAliasTableAliasOf(&g_Table, "Documentaries"), "DOCUME~2"), "a real file called DOCUME~1 is stepped over");
    CHECK(AliasTestIs(DosAliasTableAliasOf(&g_Table, "Programs and things"), "PROGRA~2"), "and a volume's own short name too");

    printf("the same listing gives the same aliases\n");
    {
        CHAR first[DOS_ALIAS_NAME_SIZE];
        INT pass;

        for (pass = 0; pass < 2; ++pass)
        {
            DosAliasTableBegin(&g_Table, "Z:\\");
            DosAliasTableNote(&g_Table, "Documentaries", "");
            DosAliasTableNote(&g_Table, "Documents", "");
            DosAliasTableFinish(&g_Table);

            if (!pass)
                strcpy(first, DosAliasTableAliasOf(&g_Table, "Documents"));
        }

        CHECK(strcmp(first, DosAliasTableAliasOf(&g_Table, "Documents")) == 0, "rebuilt: Documents is DOCUME~2 both times");
    }

    printf("a crowded directory\n");
    {
        CHAR name[32];
        INT index;
        INT isDistinct = 1;

        DosAliasTableBegin(&g_Table, "Z:\\");

        for (index = 0; index < 15; ++index)
        {
            sprintf(name, "Documentary %02d", index);
            DosAliasTableNote(&g_Table, name, "");
        }

        DosAliasTableFinish(&g_Table);
        sprintf(name, "Documentary %02d", 9);
        CHECK(AliasTestIs(DosAliasTableAliasOf(&g_Table, name), "DOCUM~10"), "the tenth -> DOCUM~10");

        for (index = 0; index < g_Table.NeedyCount; ++index)
        {
            INT other;

            for (other = 0; other < index; ++other)
                if (strcmp(g_Table.NeedyAlias[index], g_Table.NeedyAlias[other]) == 0)
                    isDistinct = 0;
        }

        CHECK(isDistinct && g_Table.NeedyCount == 15, "fifteen entries, fifteen distinct aliases");
    }
}

INT main(VOID)
{
    printf("== DOS 8.3 aliases (dos_alias.c) ==\n");
    AliasTestShortNames();
    AliasTestMake();
    AliasTestTable();
    printf("\n%d checks, %d failed\n", g_Total, g_Failed);
    return g_Failed ? 1 : 0;
}

DECLARE SUB ClearPalette ()
DECLARE SUB DrawPalette ()
DECLARE SUB SetPalette (i AS INTEGER, r AS INTEGER, g AS INTEGER, b AS INTEGER)

DIM count AS INTEGER

RANDOMIZE TIMER

CLS
SCREEN 13

FOR i% = 7 TO 56 STEP 7
    h% = i% / 2

    SetPalette count + 16, 63 - i%, i%, 0
    SetPalette count + 24, 0, 63 - i%, i%

    SetPalette count + 32, 63, 0 + i%, 63 - i%
    SetPalette count + 40, 63 - i%, 63, i%

    SetPalette count + 48, h%, h%, h%
    SetPalette count + 56, 31 + h%, 31 + h%, 31 + h%

    SetPalette count + 64, i%, 0, 0
    SetPalette count + 72, 63, i%, i%

    SetPalette count + 80, i%, h%, 0
    SetPalette count + 88, 63, 31 + h%, i%

    SetPalette count + 96, i%, i%, 0
    SetPalette count + 104, 63, 63, i%

    SetPalette count + 112, h%, i%, 0
    SetPalette count + 120, 31 + h%, 63, i%

    SetPalette count + 128, 0, i%, 0
    SetPalette count + 136, i%, 63, i%

    SetPalette count + 144, 0, i%, h%
    SetPalette count + 152, i%, 63, 31 + h%

    SetPalette count + 160, 0, i%, i%
    SetPalette count + 168, i%, 63, 63

    SetPalette count + 176, 0, h%, i%
    SetPalette count + 184, i%, 31 + h%, 63

    SetPalette count + 192, 0, 0, i%
    SetPalette count + 200, i%, i%, 63

    SetPalette count + 208, h%, 0, i%
    SetPalette count + 216, 31 + h%, i%, 63

    SetPalette count + 224, i%, 0, i%
    SetPalette count + 232, 63, i%, 63

    SetPalette count + 240, i%, 0, h%
    SetPalette count + 248, 63, i%, 31 + h%

    count = count + 1
NEXT i%

DrawPalette

SLEEP
SYSTEM

SUB ClearPalette
    FOR i% = 0 TO 255
        SetPalette i%, 0, 0, 0
    NEXT i%
END SUB

SUB DrawPalette
    FOR i% = 0 TO 255
        x% = INT(i% MOD 16) * 20
        y% = INT(i% / 16)
       
        LINE (x%, y% * 10)-(x% + 19, y% * 10 + 9), i%, BF
    NEXT i%
END SUB

SUB SetPalette (i AS INTEGER, r AS INTEGER, g AS INTEGER, b AS INTEGER)
    DIM value AS LONG
    value = 65536 * b + 256 * g + r
    PALETTE i, value
END SUB


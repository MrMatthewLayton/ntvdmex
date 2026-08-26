DECLARE SUB InitializeDisplay ()
DECLARE SUB DisplayAnimation (delay AS LONG, count AS INTEGER)
DECLARE SUB SetPalette (i AS INTEGER, r AS INTEGER, g AS INTEGER, b AS INTEGER)
DECLARE FUNCTION GetRandomNumber (min AS INTEGER, max AS INTEGER)

TYPE CHAR
    x AS INTEGER
    y AS INTEGER
    c AS INTEGER
END TYPE

CONST count = 32
CONST delay = 40000

OPTION BASE 0
RANDOMIZE TIMER

InitializeDisplay
DisplayAnimation delay, count

SYSTEM

SUB DisplayAnimation (delay AS LONG, count AS INTEGER)
    DIM index AS INTEGER
    DIM counter AS LONG
    DIM chars(count) AS CHAR

    FOR index = 0 TO count - 1
        chars(index).x = GetRandomNumber(1, 81)
        chars(index).y = GetRandomNumber(1, 30)
        chars(index).c = GetRandomNumber(0, 15)
    NEXT

    DO WHILE INKEY$ <> CHR$(27)
        FOR index = 0 TO count - 1
            IF chars(index).c >= 15 THEN
                chars(index).c = 0
                chars(index).x = GetRandomNumber(1, 81)
            ELSE
                chars(index).c = chars(index).c + 1
            END IF

            IF chars(index).y >= 30 THEN
                chars(index).y = 1
            ELSE
                chars(index).y = chars(index).y + 1
            END IF
           
            LOCATE chars(index).y, chars(index).x
            COLOR chars(index).c
            PRINT CHR$(GetRandomNumber(55, 200));

            FOR counter = 0 TO delay
            NEXT
        NEXT
    LOOP
END SUB

FUNCTION GetRandomNumber (min AS INTEGER, max AS INTEGER)
    GetRandomNumber = INT(RND * (max - min)) + min
END FUNCTION

SUB InitializeDisplay
    DIM index AS INTEGER
    DIM attribute AS INTEGER

    CLS
    SCREEN 12

    FOR index = 0 TO 15
      SetPalette index, 0, index * 4, index * 2
    NEXT
END SUB

SUB SetPalette (i AS INTEGER, r AS INTEGER, g AS INTEGER, b AS INTEGER)
    DIM value AS LONG
    value = 65536 * b + 256 * g + r
    PALETTE i, value
END SUB


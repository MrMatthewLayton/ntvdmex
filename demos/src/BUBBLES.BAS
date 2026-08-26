OPTION BASE 0
RANDOMIZE TIMER

TYPE STAR
    x AS INTEGER
    y AS INTEGER
    s AS INTEGER
    c AS INTEGER
    r AS INTEGER
END TYPE

CONST count = 128
DIM stars(count) AS STAR

DIM streak AS INTEGER
streak = 0

DIM inkey AS STRING
inkey = ""

CLS
SCREEN 12

FOR index = 0 TO 15
    pcolor& = index * 4
    PALETTE index, pcolor& + 256 * pcolor& + 65536 * pcolor&
NEXT

PAINT (0, 0), 15

FOR i% = 0 TO count - 1
    stars(i%).x = RND * 639 + 1
    stars(i%).y = RND * 479 + 1
    stars(i%).s = RND * 6 + 1
    stars(i%).c = RND * 15 + 1
    stars(i%).r = RND * 5 + 1
NEXT i%

DO WHILE inkey <> CHR$(27)

    inkey = INKEY$
   
    IF inkey = CHR$(13) THEN
      streak = NOT streak
    END IF

    FOR i% = 0 TO count - 1
       
        IF streak = 0 THEN
          LINE (stars(i%).x, stars(i%).y)-(stars(i%).x + stars(i%).s, stars(i%).y + stars(i%).s), 15, BF
        END IF

        IF stars(i%).y <= 0 THEN
            stars(i%).x = RND * 639 + 1
            stars(i%).y = 479
            stars(i%).s = RND * 6 + 1
            stars(i%).c = RND * 15 + 1
            stars(i%).r = RND * 5 + 1
        ELSE
            stars(i%).y = stars(i%).y - stars(i%).r
        END IF
        LINE (stars(i%).x, stars(i%).y)-(stars(i%).x + stars(i%).s, stars(i%).y + stars(i%).s), stars(i%).c, BF
    NEXT i%
LOOP


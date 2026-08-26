DIM Buffer(19200) AS INTEGER

RANDOMIZE TIMER

CLS
SCREEN 13

LINE (20, 20)-(139, 179), 24, B

PAINT (1, 1), 24

LINE (20, 20)-(139, 179), 20, B
LINE (180, 20)-(299, 179), 20, B
LINE (139, 20)-(139, 179), 15
LINE (20, 179)-(139, 179), 15

DO WHILE INKEY$ <> CHR$(27)
    VIEW (21, 21)-(138, 178)
    LINE (RND * 119, RND * 159)-(RND * 119, RND * 159), RND * 255, B

    GET (0, 0)-(119, 159), Buffer
    
    VIEW (181, 21)-(300, 180)
    PUT (0, 0), Buffer, PSET
LOOP

SLEEP
SYSTEM


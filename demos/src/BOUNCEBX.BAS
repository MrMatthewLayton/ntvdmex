CLS
SCREEN 12

CONST SIZE = 40
CONST XMIN = 0
CONST YMIN = 0
CONST XMAX = 639 - SIZE
CONST YMAX = 479 - SIZE

DIM x, y, dx, dy, c AS INTEGER

x = 0
y = 0
dx = 1
dy = 1
c = (RND * 14) + 1

DO WHILE INKEY$ <> CHR$(27)
    LINE (x, y)-(x + SIZE, y + SIZE), 0, BF

    IF x <= XMIN THEN
        dx = 1
        c = (RND * 14) + 1
    ELSEIF x >= XMAX THEN
        dx = -1
        c = (RND * 14) + 1
    END IF

    IF y < YMIN THEN
        dy = 1
        c = (RND * 14) + 1
    ELSEIF y >= YMAX THEN
        dy = -1
        c = (RND * 14) + 1
    END IF

    x = x + dx
    y = y + dy

    LINE (x, y)-(x + SIZE, y + SIZE), c, BF

    WAIT &H3DA, 8
    WAIT &H3DA, 8
LOOP


DECLARE SUB DrawDesktop ()
DECLARE SUB DrawCursor ()
'Mouse (which)
'      0 = Mouse Init
'      1 = Mouse Driver
'      2 = Mouse Put
'      3 = Mouse Status
'      4 = Mouse Hide
'      5 = Mouse Show
'
DECLARE SUB Mouse (which!)
DIM SHARED mouser$, MouseLeftButton, MouseRightButton, MouseXPos, MouseYPos, AX%, bx%, cx%, dx%
DIM lastx, lasty AS INTEGER
DIM array(1 TO 64)

CLS
SCREEN 12
PAINT (1, 1), 3

Mouse 0
Mouse 5

DATA 55,89,E5,8B,5E,0C,8B,07,50,8B,5E,0A,8B,07,50,8B
DATA 5E,08,8B,0F,8B,5E,06,8B,17,5B,58,1E,07,CD,33,53
DATA 8B,5E,0C,89,07,58,8B,5E,0A,89,07,8B,5E,08,89,0F
DATA 8B,5E,06,89,17,5D,CA,08,00

SUB DrawCursor
  'LINE (MouseXPos - 4, MouseYPos - 4)-(MouseXPos + 4, MouseYPos + 4), 15, BF
END SUB

SUB DrawDesktop
  LINE (0, 0)-(639, 479), 3, BF
END SUB

SUB Mouse (which)
'0 = Mouse Init
'1 = Mouse Driver
'2 = Mouse Put
'3 = Mouse Status
'4 = Mouse Hide
'5 = Mouse Show
IF which = 0 THEN
  mouser$ = SPACE$(57)
  FOR I% = 1 TO 57
    READ A$
    H$ = CHR$(VAL("&H" + A$))
    MID$(mouser$, I%, 1) = H$
  NEXT I%
  AX% = 0
  bx% = 0
  cx% = 0
  dx% = 0
  Mouse 1
  MouseInit% = AX%
  Mouse 5
END IF
IF which = 1 THEN
  DEF SEG = VARSEG(mouser$)
  mouser% = SADD(mouser$)
  CALL Absolute(AX%, bx%, cx%, dx%, mouser%)
END IF
IF which = 2 THEN
  AX% = 4
  cx% = x%
  dx% = y%
  bx% = 0
  Mouse 1
END IF
IF which = 3 THEN
  AX% = 3
  Mouse 1
  MouseLeftButton = ((bx% AND 1) <> 0)
  MouseRightButton = ((bx% AND 2) <> 0)
  MouseXPos = cx%
  MouseYPos = dx%
END IF
IF which = 4 THEN
 AX% = 2
 bx% = 0
 cx% = 0
 dx% = 0
 Mouse 1
END IF
IF which = 5 THEN
  AX% = 1
  bx% = 0
  cx% = 0
  dx% = 0
  Mouse 1
END IF
END SUB


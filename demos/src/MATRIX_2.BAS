DECLARE FUNCTION RandInt (min AS INTEGER, max AS INTEGER)
DECLARE SUB InitializeDisplay ()
DECLARE SUB DisplayAnimation (count AS INTEGER)

TYPE CHAR
	c AS INTEGER
	v AS STRING * 1
END TYPE

CONST count = 48

InitializeDisplay
DisplayAnimation count

SYSTEM

SUB DisplayAnimation (count AS INTEGER)
	DIM x(count) AS INTEGER
	DIM y(count) AS INTEGER
	DIM c(30, 80) AS CHAR
	DIM cx AS INTEGER
	DIM cy AS INTEGER
	DIM index AS INTEGER
	
	FOR index = 1 TO count
		x(index) = RandInt(1, 81)
		y(index) = RandInt(1, 30)
	NEXT
	
	DO WHILE INKEY$ <> CHR$(27)
		FOR cy = 1 TO 30
			FOR cx = 1 TO 80
				IF c(cy, cx).c > 0 THEN
					c(cy, cx).c = c(cy, cx).c - 1
					
					LOCATE cy, cx
					COLOR c(cy, cx).c
					PRINT c(cy, cx).v;
				END IF
			NEXT
		NEXT
		
		FOR index = 1 TO count
			c(y(index), x(index)).c = 15
			c(y(index), x(index)).v = CHR$(RandInt(55, 200))
			
			IF y(index) = 30 THEN
				y(index) = 1
				x(index) = RandInt(1, 81)
			ELSE
				y(index) = y(index) + 1
			END IF
		NEXT
		WAIT &H3DA, 8 
	LOOP
END SUB

SUB InitializeDisplay
	DIM index AS INTEGER
	DIM red AS INTEGER
	DIM green AS INTEGER
	DIM blue AS INTEGER
	DIM value AS LONG

		CLS
		SCREEN 12
		WAIT &H3DA, 8 

		FOR index = 0 TO 15
			red = 0
			green = index * 4
			blue = index * 3
			value = 65536 * blue + 256 * green + red
			PALETTE index, value
		NEXT
END SUB

FUNCTION RandInt (min AS INTEGER, max AS INTEGER)
		RandInt = INT(RND * (max - min)) + min
END FUNCTION


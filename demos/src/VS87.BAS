DECLARE SUB SetPaletteColors ()
DECLARE SUB DrawTitleBar (Title AS STRING)
DECLARE SUB DrawMenuBar ()
DECLARE SUB DrawWindows ()
DECLARE SUB DrawStatusBar ()
DECLARE SUB DrawSolutionExplorer ()
DECLARE SUB DrawEditorWindow ()
DECLARE SUB DrawText (BackColor AS INTEGER, ForeColor AS INTEGER, XLocation AS INTEGER, YLocation AS INTEGER, Text AS STRING)
DECLARE FUNCTION LPAD$ (Text AS STRING, Length AS INTEGER)
CONST TopLeft = 218
CONST TopRight = 191
CONST BottomLeft = 192
CONST BottomRight = 217
CONST Horizontal = 196
CONST Vertical = 179
CONST JoinTop = 194
CONST JoinBottom = 193
CONST TitleLeft = 180
CONST TitleRight = 195
CONST TitleSeparator = 179
	 
SCREEN 0
CLS

CALL SetPaletteColors
CALL DrawTitleBar("Microsoft Visual Studio 87 Professional")
CALL DrawMenuBar
CALL DrawWindows
CALL DrawStatusBar
CALL DrawSolutionExplorer
CALL DrawEditorWindow

SLEEP
END

SUB DrawEditorWindow
	FOR y% = 4 TO 23
		CALL DrawText(5, 8, 20, y%, LPAD$(STR$(y% - 3), 3))
		CALL DrawText(5, 10, 23, y%, CHR$(TitleSeparator))
	NEXT
	FOR y% = 7 TO 9
		CALL DrawText(5, 14, 23, y%, CHR$(TitleSeparator))
	NEXT
	FOR y% = 19 TO 23
		CALL DrawText(5, 14, 23, y%, CHR$(TitleSeparator))
	NEXT
	CALL DrawText(5, 1, 24, 4, "using")
	CALL DrawText(5, 0, 30, 4, "System;")
	CALL DrawText(5, 1, 24, 5, "using")
	CALL DrawText(5, 0, 30, 5, "System.Collections.Generic;")
	CALL DrawText(5, 1, 24, 6, "using")
	CALL DrawText(5, 0, 30, 6, "System.Linq;")
	CALL DrawText(5, 1, 24, 8, "namespace")
	CALL DrawText(5, 0, 34, 8, "Microsoft.ContosoUniversity.Runtime;")
	
	CALL DrawText(5, 1, 24, 10, "internal static class")
	CALL DrawText(5, 3, 46, 10, "Program")
	CALL DrawText(5, 0, 24, 11, "{")
	CALL DrawText(5, 1, 26, 12, "private static void")
		CALL DrawText(5, 0, 46, 12, "Main(")
	CALL DrawText(5, 1, 51, 12, "string")
	CALL DrawText(5, 0, 57, 12, "[] args)")
	CALL DrawText(5, 0, 26, 13, "{")
	CALL DrawText(5, 3, 28, 14, "Console")
	CALL DrawText(5, 0, 35, 14, ".WriteLine(")
	CALL DrawText(5, 4, 46, 14, CHR$(34))
	CALL DrawText(5, 4, 47, 14, "Hello, Contoso University!")
	CALL DrawText(5, 4, 73, 14, CHR$(34))
	CALL DrawText(5, 0, 74, 14, ");")
	CALL DrawText(5, 3, 28, 15, "Console")
	CALL DrawText(5, 0, 35, 15, ".Read();")
	CALL DrawText(5, 0, 26, 16, "}")
	CALL DrawText(5, 0, 24, 17, "}")
					
					
END SUB

SUB DrawMenuBar
	CALL DrawText(7, 0, 1, 2, SPACE$(80))
	CALL DrawText(7, 0, 3, 2, "File  Edit  View  Debug  Run  Tools  Window  Help")
	CALL DrawText(7, 10, 75, 2, CHR$(16))
	CALL DrawText(7, 0, 77, 2, "Run")
END SUB

SUB DrawSolutionExplorer
	CALL DrawText(7, 8, 2, 4, "ContosoUnivers...")
	CALL DrawText(5, 8, 3, 5, "Configur...n.cs")
		CALL DrawText(5, 8, 3, 6, "Course.cs")
	CALL DrawText(5, 8, 3, 7, "Enrollment.cs")
	CALL DrawText(5, 8, 3, 8, "Instructor.cs")
	CALL DrawText(1, 15, 2, 9, SPACE$(17))
	CALL DrawText(1, 15, 3, 9, "Program.cs")
	CALL DrawText(5, 8, 3, 10, "Student.cs")
END SUB

SUB DrawStatusBar
	CALL DrawText(3, 0, 1, 25, SPACE$(80))
	CALL DrawText(3, 11, 2, 25, "Ready")
	CALL DrawText(3, 11, 20, 25, "[C# Version 13] No Errors")
	CALL DrawText(3, 11, 77, 25, "INS")
END SUB

SUB DrawText (BackColor AS INTEGER, ForeColor AS INTEGER, XLocation AS INTEGER, YLocation AS INTEGER, Text AS STRING)
	COLOR ForeColor, BackColor
	LOCATE YLocation, XLocation
	PRINT Text;
END SUB

SUB DrawTitleBar (Title AS STRING)
	CALL DrawText(1, 15, 1, 1, SPACE$(80))
	CALL DrawText(1, 15, 3, 1, Title)
	CALL DrawText(4, 15, 78, 1, SPACE$(3))
	CALL DrawText(4, 15, 79, 1, "x")
END SUB

SUB DrawWindows
	FOR x% = 1 TO 80
		CALL DrawText(7, 8, x%, 3, CHR$(Horizontal))
	NEXT
	 
	CALL DrawText(7, 8, 1, 3, CHR$(TopLeft))
	CALL DrawText(1, 15, 2, 3, "Solution Explorer")
	CALL DrawText(7, 8, 19, 3, CHR$(JoinTop))
	CALL DrawText(7, 8, 20, 3, CHR$(TitleLeft))
	CALL DrawText(2, 0, 21, 3, " Program.cs ")
	CALL DrawText(7, 8, 33, 3, CHR$(TitleSeparator))
	CALL DrawText(7, 8, 34, 3, "Configuration.cs")
	CALL DrawText(7, 8, 50, 3, CHR$(TitleRight))
	CALL DrawText(7, 8, 80, 3, CHR$(TopRight))
	
	FOR y% = 4 TO 23
		CALL DrawText(7, 8, 1, y%, CHR$(Vertical))
		
		FOR x% = 2 TO 18
			CALL DrawText(5, 0, x%, y%, SPACE$(1))
		NEXT
		
		CALL DrawText(7, 8, 19, y%, CHR$(Vertical))
		
		FOR x% = 20 TO 79
			CALL DrawText(5, 0, x%, y%, SPACE$(1))
		NEXT
		
		CALL DrawText(7, 8, 80, y%, CHR$(Vertical))
	NEXT
	
	FOR x% = 1 TO 80
		CALL DrawText(7, 8, x%, 24, CHR$(Horizontal))
	NEXT
	
	CALL DrawText(7, 8, 1, 24, CHR$(BottomLeft))
	CALL DrawText(7, 8, 19, 24, CHR$(JoinBottom))
	CALL DrawText(7, 8, 80, 24, CHR$(BottomRight))
END SUB

FUNCTION LPAD$ (Text AS STRING, Length AS INTEGER)
		DIM PadLength AS INTEGER
		PadLength = Length - LEN(Text)
		
		IF PadLength > 0 THEN
				LPAD$ = SPACE$(PadLength) + Text
		ELSE
				LPAD$ = Text
		END IF
END FUNCTION

SUB SetPaletteColors
	'Bright Yellow at index 2
	OUT &H3C8, 2
		OUT &H3C9, 63
		OUT &H3C9, 63
		OUT &H3C9, 0
	 
	'Bright White at index 5
	OUT &H3C8, 5
		OUT &H3C9, 63
		OUT &H3C9, 63
		OUT &H3C9, 63
END SUB


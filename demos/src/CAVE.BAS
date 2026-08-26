' CAVE.BAS - Underwater cave voxel swimmer for QuickBASIC 4.5
' Mode 13h (320x200x256). Voxel-cast floor + ceiling heightmaps,
' triangular palette (turquoise / brown / black), forward-only swim.
'
' Pre-computed: column ray offsets, distance-to-screenY tables,
' per-distance palette row, bump shading.
'
'$DYNAMIC
DEFINT A-Z

DECLARE SUB BuildPalette ()
DECLARE SUB BuildMaps ()
DECLARE SUB BuildTables ()
DECLARE SUB Render ()

CONST SW = 320
CONST SH = 200
CONST HSH = 100
CONST MAPSZ = 64
CONST MAPMK = 63
CONST MAXD = 64
CONST EYEH = 16
CONST CEILZ = 32
CONST FOCAL = 277

' Coarse 64x64 procedural heightmap + shade (idx = wy*64 + wx).
DIM SHARED FloorMap(0 TO MAPSZ * MAPSZ - 1)
DIM SHARED FloorShade(0 TO MAPSZ * MAPSZ - 1)

' Bilinear-upsampled 128x128 maps used by the renderer.
' FloorMap2 stores h*4 (0..64) so half-cell bilinear stays integer.
DIM SHARED FloorMap2(0 TO 128 * 128 - 1)
DIM SHARED FloorShade2(0 TO 128 * 128 - 1)

' Pre-computed projection tables. Indexed by d2 = 2*d_real (half-step depth),
' and FloorYAt by (d2, h*4) to match FloorMap2's fixed-point heights.
DIM SHARED OffsetX(0 TO SW - 1)
DIM SHARED FloorYAt(0 TO MAXD * 2 * 65 + 64)
DIM SHARED DistRow(0 TO MAXD * 2)

' Y-buffers
DIM SHARED YBufF(0 TO SW - 1)
DIM SHARED YBufC(0 TO SW - 1)

' Back buffer (GET/PUT format)
DIM SHARED ScrBuf(0 TO 32001)
DIM SHARED BufSeg AS LONG
DIM SHARED BufPix AS LONG
DIM SHARED MinD                  ' first distance worth iterating

DIM SHARED Py AS LONG

SCREEN 13
CALL BuildPalette
CALL BuildMaps
CALL BuildTables

ScrBuf(0) = 2560
ScrBuf(1) = 200
BufSeg = VARSEG(ScrBuf(0)) AND 65535&
BufPix = (VARPTR(ScrBuf(0)) AND 65535&) + 4

' With heights now reaching 16 (eye level), even close d shows tall bumps
' (h=16 projects to y=horizon at any d). MinD=4 skips d=1..3 where only
' the tallest possible voxels would be on-screen (almost never).
MinD = 4

Py = 0

DO
  CALL Render
  WAIT &H3DA, 8
  PUT (0, 0), ScrBuf, PSET
  Py = (Py + 1) AND MAPMK
  IF INKEY$ = CHR$(27) THEN EXIT DO
LOOP

SCREEN 0
WIDTH 80, 25
PRINT "Done."
END

'-----------------------------------------------------------------------
SUB BuildPalette
  ' Triangular barycentric:
  ' top vertex = turquoise, bot-left = brown/taupe, bot-right = black.
  OUT &H3C8, 0
  FOR row = 0 TO 15
    FOR col = 0 TO 15
      wt! = (15 - row) / 15
      wbr! = row * (15 - col) / 225!
      wbk! = row * col / 225!
      r = INT(wt! * 0 + wbr! * 35 + wbk! * 0)
      g = INT(wt! * 45 + wbr! * 25 + wbk! * 0)
      b = INT(wt! * 50 + wbr! * 15 + wbk! * 0)
      IF r > 63 THEN r = 63
      IF g > 63 THEN g = 63
      IF b > 63 THEN b = 63
      IF r < 0 THEN r = 0
      IF g < 0 THEN g = 0
      IF b < 0 THEN b = 0
      OUT &H3C9, r
      OUT &H3C9, g
      OUT &H3C9, b
    NEXT col
  NEXT row
END SUB

'-----------------------------------------------------------------------
SUB BuildMaps
  ' Procedural heightmap, 0..16. The -4*COS(x*2pi/MAPSZ) term is a periodic
  ' ridge that tiles with the map -> cave appears to pinch shut as player swims.
  FOR y = 0 TO MAPSZ - 1
    FOR x = 0 TO MAPSZ - 1
      f! = SIN(x * .4) * 2 + COS(y * .3) * 1.5 + SIN((x + y) * .25) + 10 - 4 * COS(x * .098175)
      IF f! > 16 THEN f! = 16
      FloorMap(y * 64 + x) = INT(f!)
    NEXT x
  NEXT y

  ' Bump shade from slope vs forward light (brighter on front-facing slopes).
  FOR y = 0 TO MAPSZ - 1
    yPrev = (y - 1) AND MAPMK
    FOR x = 0 TO MAPSZ - 1
      shd = 8 - (FloorMap(y * 64 + x) - FloorMap(yPrev * 64 + x)) * 2
      IF shd < 0 THEN shd = 0
      IF shd > 15 THEN shd = 15
      FloorShade(y * 64 + x) = shd
    NEXT x
  NEXT y

  ' Bilinear-upsample FloorMap + FloorShade into 128x128.  Runtime renderer
  ' samples these directly -> 2 lookups + 1 lerp per column (vs 4+3 before).
  FOR y2 = 0 TO 127
    y0 = y2 \ 2
    y1 = (y0 + 1) AND MAPMK
    yfH = y2 AND 1                       ' 0 or 1 (sub-cell half)
    FOR x2 = 0 TO 127
      x0 = x2 \ 2
      x1 = (x0 + 1) AND MAPMK
      xfH = x2 AND 1
      h00 = FloorMap(y0 * 64 + x0)
      h01 = FloorMap(y0 * 64 + x1)
      h10 = FloorMap(y1 * 64 + x0)
      h11 = FloorMap(y1 * 64 + x1)
      hT = h00 * 2 + (h01 - h00) * xfH    ' h*2 along top row
      hB = h10 * 2 + (h11 - h10) * xfH    ' h*2 along bot row
      FloorMap2(y2 * 128 + x2) = hT * 2 + (hB - hT) * yfH    ' h*4
      s00 = FloorShade(y0 * 64 + x0)
      s01 = FloorShade(y0 * 64 + x1)
      s10 = FloorShade(y1 * 64 + x0)
      s11 = FloorShade(y1 * 64 + x1)
      sT = s00 * 2 + (s01 - s00) * xfH
      sB = s10 * 2 + (s11 - s10) * xfH
      FloorShade2(y2 * 128 + x2) = (sT * 2 + (sB - sT) * yfH) \ 4
    NEXT x2
  NEXT y2
END SUB

'-----------------------------------------------------------------------
SUB BuildTables
  ' Per-column ray X-offset (×256 fixed-point)
  FOR c = 0 TO SW - 1
    OffsetX(c) = INT((c - 160) * 256& / FOCAL)
  NEXT c

  ' Per-(d2, h*4) projected screenY. h4 = h*4, scale = FOCAL*2/d2.
  ' Start at d2=2; d2=1 would overflow ((EYEH*4 - h4) * sc) and is never used
  ' (renderer starts at MinD * 2 = 8).
  FOR d2 = 2 TO MAXD * 2
    sc = FOCAL * 2 \ d2
    IF sc < 1 THEN sc = 1
    dBase = d2 * 65
    FOR h4 = 0 TO 64
      fy = HSH + (EYEH * 4 - h4) * sc \ 4
      IF fy > SH THEN fy = SH
      IF fy < 0 THEN fy = 0
      FloorYAt(dBase + h4) = fy
    NEXT h4
  NEXT d2

  ' Per-d2 palette row: near = 15, far = 0
  FOR d2 = 1 TO MAXD * 2
    dr = 15 - (d2 * 15 \ (MAXD * 2))
    IF dr < 0 THEN dr = 0
    IF dr > 15 THEN dr = 15
    DistRow(d2) = dr
  NEXT d2

END SUB

'-----------------------------------------------------------------------
SUB Render
  ' Clear back-buffer to fog (palette 0 = turquoise). Two pixels per int.
  FOR i = 2 TO 32001: ScrBuf(i) = 0: NEXT i

  ' Reset single Y-buffer (floor only; ceiling mirrors floor)
  FOR c = 0 TO SW - 1
    YBufF(c) = SH
  NEXT c

  DEF SEG = BufSeg

  FOR d2 = MinD * 2 TO MAXD * 2 STEP 2
    wy2 = (Py + Py + d2) AND 127             ' 2x-Y world row (0..127)
    rowOfs = wy2 * 128
    dBase = d2 * 65
    drow = DistRow(d2) * 16

    FOR c = 0 TO SW - 1
      ' World x in 2x-X grid (128 half-cells), 1/256 fixed point.
      wxFP& = OffsetX(c) * d2 + 1048576
      wx = (wxFP& \ 256) AND 127
      xFrac = (wxFP& \ 16) AND 15
      idx = rowOfs + wx
      idx2 = rowOfs + ((wx + 1) AND 127)

      ' Lerp screen-Y between adjacent half-cells. Y bilinear is precomputed.
      yA = FloorYAt(dBase + FloorMap2(idx))
      yB = FloorYAt(dBase + FloorMap2(idx2))
      fY = yA + ((yB - yA) * xFrac) \ 16

      yF = YBufF(c)
      IF fY < yF THEN
        clr = drow + FloorShade2(idx)
        ofsF& = BufPix + CLNG(fY) * 320& + c
        ofsC& = BufPix + CLNG(SH - 1 - fY) * 320& + c
        FOR y = fY TO yF - 1
          POKE ofsF&, clr                    ' floor pixel
          POKE ofsC&, clr                    ' mirrored ceiling pixel
          ofsF& = ofsF& + 320
          ofsC& = ofsC& - 320
        NEXT y
        YBufF(c) = fY
      END IF
    NEXT c
  NEXT d2

  DEF SEG
END SUB

# doomref.py — a pixel-exact oracle for the video path

Every fixed screen Doom draws is a lump in `DOOM1.WAD`, stored in a documented format
with a documented palette, so the bytes that *should* reach the framebuffer are
computable exactly. That makes the IWAD a better oracle than a screenshot: a screenshot
off another machine is a photograph of an answer, the WAD is the answer. It also needs
no rig run of its own — the host already self-captures frames to `shotNN.bmp` with
`capture.flag` — and it localises a fault to the pixel.

```
./doomref.py DOOM1.WAD dump TITLEPIC ref.bmp        # the reference image
./doomref.py DOOM1.WAD cmp  TITLEPIC shot02.bmp     # exact diff; nonzero exit on mismatch
./doomref.py DOOM1.WAD cmp  STBAR    shot09.bmp --at 0,168
```

Captures must be the 8bpp BMPs the host writes: the comparison is on **palette indices**,
so a converted PNG would compare colours that have already been through a transform.

## What it replaced, and why that matters

The video path had been judged by "even-column match" — a duplication detector. That
metric actively misled: when the plane-remap fix landed the score went **up**
(0.55 → 0.70), because real Doom content has many equal neighbours and the lower number
had been our own misattributed columns scoring as detail. A metric that moves the wrong
way when a bug is fixed is worse than no metric.

## What it established immediately

* `TITLEPIC` vs a captured title screen: **0 of 64000 pixels differ**. The whole video
  path — mode-Y plane backing, page flip, palette, presentation, capture — is exact for
  a full-screen image.
* The status bar's `%` sign is at exactly the right place (x=91, y=171, Doom's
  `ST_HEALTHX`) with **95 of 145 opaque pixels wrong**, identically in every frame.
* `STBAR` background: 60% of pixels wrong, and the duplicated-column pattern is
  **identical across frames** — written once, wrong, never corrected.
* Seeding each plane with its own index proved those pixels *are* written per-plane
  (0–3 marker pixels survive), so it is not a gap in coverage but a collapse during the
  write.

That chain ended at `wmode hist: 0x79 in mode 0, 0x78 in mode 1` — Doom uses **VGA write
mode 1, the latch copy**, 120 times a run.

# sbref.py — the same idea for sampled audio

`sbdump.flag` on the share makes the host record every byte `vdd_sb_render()` pulls out
of the guest's DMA ring to `C:\ntvdmex\sb.raw` (no I/O on the audio thread; it is written
at wind-down). That file is the audio equivalent of a screenshot.

```
./sbref.py sb.raw --rate 11025 --stereo --block 256
```

What it established on Doom, where "sound is still glitchy" had survived a resampler fix
and a tick-batching fix:

* 41.5 s of audio from a 45 s run — **the rate is right**, so it is not a pitch or clock
  error.
* `corr(L,R) = 0.978` — the two interleaved channels really are two channels of one
  effect, so **the stereo framing is right**. (Doom asks for stereo: DSP command `0xC6`,
  mode byte `0x20`. A 256-byte block is 128 FRAMES, not 256 samples — mistaking that led
  to a confident, wrong "the DMA runs 1.87x too fast".)
* Discontinuities are **12.8× over-represented at offset 2 of every 128-frame block**
  (183 against a mean of 14.3). A jump rate that peaks at one fixed offset in every block
  is a defect at the DMA block boundary — at 86 blocks/s, which is audibly a buzz.

That last number is what a raw count of "glitches" could never give: game audio has sharp
attacks, so the count alone says nothing. The *periodicity* is the evidence.

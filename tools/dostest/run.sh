#!/usr/bin/env bash
#
# Build + run the off-VM DOS-kernel unit battery on the build host (Layer 1 of
# the M2.4 test plan). Pure C, no Windows/VM dependency -- uses the native cc.
#
#   ./tools/dostest/run.sh
#
# Exits nonzero if any case fails, so it can gate a commit or CI step.
#
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/mcb_test" "$DIR/mcb_test.c"

"$DIR/mcb_test"

# M3 slice-1: the VDD device bus battery (vdd_bus.c + ntvdd.h).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/vdd_test" "$DIR/vdd_test.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/vdd_test"

# Session 11: the 8259A PIC. Its in-service rules are what stop an injected handler
# being re-entered before it EOIs, so they get pinned down off-VM.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/pic_test" "$DIR/pic_test.c" "$DIR/../../src/vdd/vdd_pic.c"

"$DIR/pic_test"

# M3 slice-2: the PIT timer VDD battery (vdd_pit.c on the bus).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/pit_test" "$DIR/pit_test.c" \
   "$DIR/../../src/vdd/vdd_pit.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/pit_test"

# M3 slice-4: the text-mode video VDD battery (vdd_video.c on the bus).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/video_test" "$DIR/video_test.c" \
   "$DIR/../../src/vdd/vdd_video.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/video_test"

# M3 slice-6: the keyboard input VDD battery (vdd_input.c on the bus).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/input_test" "$DIR/input_test.c" \
   "$DIR/../../src/vdd/vdd_input.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/input_test"

# M3 PC-speaker VDD battery (vdd_speaker.c + vdd_pit.c on the bus).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/speaker_test" "$DIR/speaker_test.c" \
   "$DIR/../../src/vdd/vdd_speaker.c" "$DIR/../../src/vdd/vdd_pit.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/speaker_test"

# Sound epic slice-1: the ISA DMA controller battery (vdd_dma.c on the bus).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/dma_test" "$DIR/dma_test.c" \
   "$DIR/../../src/vdd/vdd_dma.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/dma_test"

# Sound epic slice-5: the AdLib/OPL2 register + timer battery (vdd_opl.c on the bus).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/opl_test" "$DIR/opl_test.c" \
   "$DIR/../../src/vdd/vdd_opl.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/opl_test"

# Sound epic slice-5b: the OPL2 FM synthesis core (property tests: pitch, envelope, FM).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/opl_synth_test" "$DIR/opl_synth_test.c" \
   "$DIR/../../src/vdd/vdd_opl_synth.c" "$DIR/../../src/vdd/vdd_opl.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/opl_synth_test"

# Sound epic slice-3: the Sound Blaster 16 DSP/mixer/DMA battery.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/sb_test" "$DIR/sb_test.c" \
   "$DIR/../../src/vdd/vdd_sb.c" "$DIR/../../src/vdd/vdd_dma.c" \
   "$DIR/../../src/vdd/vdd_opl.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/sb_test"

# Sound epic slice-6: the MPU-401 MIDI battery.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/mpu_test" "$DIR/mpu_test.c" \
   "$DIR/../../src/vdd/vdd_mpu.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/mpu_test"

# Sound epic slice-4: the audio mixer (resampling + SB transport).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/vdd" \
   -o "$DIR/audio_test" "$DIR/audio_test.c" \
   "$DIR/../../src/vdd/vdd_audio.c" "$DIR/../../src/vdd/vdd_sb.c" \
   "$DIR/../../src/vdd/vdd_dma.c" "$DIR/../../src/vdd/vdd_opl.c" \
   "$DIR/../../src/vdd/vdd_opl_synth.c" "$DIR/../../src/vdd/vdd_bus.c"

"$DIR/audio_test"

# M3 mode-12h fill-loop interpreter battery (src/host/v86interp.h, flat memory).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -o "$DIR/interp_test" "$DIR/interp_test.c"

"$DIR/interp_test"

# M4 slice-1: the XMS core battery (dos_xms.h, host heap + a conventional buffer).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/xms_test" "$DIR/xms_test.c"

"$DIR/xms_test"

# M4 slice-2: the EMS core battery (dos_ems.h, page-frame shadowing).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/ems_test" "$DIR/ems_test.c"

"$DIR/ems_test"

# Session 21: the x86 instruction-LENGTH decoder and boundary test (src/host/x86len.h).
# It decides which `CD nn` byte pairs the DPMI host may rewrite into a BOP, and getting
# that wrong is silent and fatal in both directions -- a mid-instruction rewrite is what
# killed Doom inside R_InitTextureMapping for five sessions.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -o "$DIR/x86len_test" "$DIR/x86len_test.c"

"$DIR/x86len_test"

# GH #128/#4: the 16-bit New Executable loader (src/wow/ne.h) -- first brick of the
# WOW layer. Two halves: a synthetic image exercising every relocation shape (chains
# and moveable/entry-ordinal targets especially, both easy to get subtly wrong), plus
# assertions against REAL binaries if you have supplied them in guest/ne/. Those are
# Microsoft's and are not in the repo, so they SKIP when absent rather than fail --
# see tools/ne/nedump.py for where the expected numbers came from.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -o "$DIR/ne_test" "$DIR/ne_test.c"

"$DIR/ne_test"

# GH #128: the WOW32 TRANSLATION LAYER -- the part this host actually writes, and
# until session 51 the only major subsystem with no off-VM coverage at all. Three
# parts: duplicate `*_ARG_*` macro detection (a collision there is a SILENT wrong
# answer, and two shipped), argument-offset tables checked against the widths
# tools/ne/neneeds.py reads out of the real Microsoft binaries, and the
# Win16/Win32 semantic deltas in src/wow/wowconv.h.
#
# ⚠ It reads the headers as TEXT, so it needs the repo root; $DIR/../.. is it.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -o "$DIR/wow_test" "$DIR/wow_test.c"

"$DIR/wow_test" "$DIR/../.."

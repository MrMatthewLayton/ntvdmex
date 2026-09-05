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

# GH #133/#131: the DOS handle table's TWO RULES (src/dos/dos_fh.h). Allocation is
# the lowest FREE slot -- which is the entire mechanism by which `>` works -- and a
# BOUND handle is a file whatever its number, which is what lets `>>` seek to
# end-of-file on handle 1. Both rules were spelled five different ways across
# dos_int21.c and four of the spellings were wrong; they are one function now, and
# every expectation below is a CASE= line from tools/dostest/p_redir.asm run on the
# genuine MS-DOS 6.22 oracle rather than a memory of what DOS does.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/fh_test" "$DIR/fh_test.c"

"$DIR/fh_test"

# GH #34: INT 21h AH=59h's class/action/locus table (src/dos/dos_err.h). Only the
# error CODE is obvious; class, action and locus are exactly the values that get
# written from memory and are wrong, so every row is a CASE= line from
# tools/dostest/p_err.asm run on the 6.22 oracle. The last check is the one that
# keeps it honest: a code we have NOT provoked must report UNMEASURED and zero the
# fields rather than offer a plausible guess.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/err_test" "$DIR/err_test.c"

"$DIR/err_test"

# GH #48: the AH=52h List of Lists layout (src/dos/dos_sysvars.h). Every offset and
# every structure size is decoded from a BUF= line that tools/dostest/p_sysvar.asm
# brought back from genuine MS-DOS 6.22 -- the raw dump is embedded in the test so
# the decoding can be re-checked rather than trusted. The DPB being 33 bytes, for
# instance, is arithmetic on the oracle's own chain pointer, not a recollection.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/sysvars_test" "$DIR/sysvars_test.c"

"$DIR/sysvars_test"

# GH #44: INT 13h geometry and CHS<->LBA (src/dos/dos_disk.h). The arithmetic is
# where this interface goes wrong -- sector numbers are 1-BASED while cylinder and
# head are not -- so every conversion is pinned, along with the refusals: sector 0,
# a head or cylinder past the geometry, and an image whose BPB cannot be trusted.
# Expectations are from tools/dostest/p_disk.asm on the 6.22 oracle (CX=0x4F12,
# BL=4 for a real 1.44MB floppy).
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/disk_test" "$DIR/disk_test.c"

"$DIR/disk_test"

# GH #132: the startup-failure recovery policy (src/dos/dos_recovery.h). Its value
# is entirely in the edges -- too eager and a machine loses its VDM after one bad
# day, too lazy and a wedged host leaves every 16-bit program broken until someone
# edits the registry by hand. A CORRUPT counter must never be able to uninstall us.
cc -std=c99 -Wall -Wextra -Wno-unused-function -O0 -g \
   -I "$DIR/../../src/dos" \
   -o "$DIR/recovery_test" "$DIR/recovery_test.c"

"$DIR/recovery_test"

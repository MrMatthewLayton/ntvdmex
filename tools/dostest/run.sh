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

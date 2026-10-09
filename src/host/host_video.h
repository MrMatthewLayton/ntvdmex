/* host_video.h -- what host_video.c offers the host's other files.
 *
 * Declarations only (#335): defined in host_video.c. */
#ifndef NTVDMEX_HOST_VIDEO_H
#define NTVDMEX_HOST_VIDEO_H
#include "host_state.h"

VOID ModeYRingNoteIrq(UINT vector, WORD cs, WORD ip, WORD ss, WORD sp);

VOID ModeYTimelineReport(VOID);
VOID VideoTrapSync(VOID);
#endif

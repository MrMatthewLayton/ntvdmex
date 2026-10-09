/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The sample format every audio path shares (#333). Defines only.
 *
 * The mixer, the wave output, the WAV recorder and the GUS all move interleaved L/R
 * frames; each had its own name for the 2. One meaning, one name.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#ifndef NTVDMEX_VDD_AUDIO_FORMAT_H
#define NTVDMEX_VDD_AUDIO_FORMAT_H

#define AUDIO_STEREO_CHANNELS   2   /* A frame is an L/R pair of samples */
#define AUDIO_MONO              0   /* A renderer's stereo flag */
#define AUDIO_STEREO            1

#endif /* NTVDMEX_VDD_AUDIO_FORMAT_H */

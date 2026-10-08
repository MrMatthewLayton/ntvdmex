/* audio_format.h -- the sample format every audio path shares (#333). Defines only.
 *
 * The mixer, the wave output, the WAV recorder and the GUS all move interleaved L/R
 * frames; each had its own name for the 2. One meaning, one name.
 */
#ifndef NTVDMEX_VDD_AUDIO_FORMAT_H
#define NTVDMEX_VDD_AUDIO_FORMAT_H

#define AUDIO_STEREO_CHANNELS 2         /* a frame is an L/R pair of samples */
#define AUDIO_MONO            0         /* a renderer's stereo flag          */
#define AUDIO_STEREO          1

#endif /* NTVDMEX_VDD_AUDIO_FORMAT_H */

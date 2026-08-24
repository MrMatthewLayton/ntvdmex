/*
 * audio_wave.h -- host audio + MIDI output for the sound stack (XP waveOut/midiOut).
 *
 * The only part of the sound epic that touches Windows. winmm is bound at runtime
 * with LoadLibrary/GetProcAddress, the same way present_ddraw.c binds DirectDraw,
 * so the host keeps its short import list and a machine without audio simply gets
 * no sound instead of failing to start.
 *
 * IMPORTANT: the pump thread runs even when the sound card cannot be opened. The
 * mixer is not just the audible path -- it is what walks the Sound Blaster's DMA
 * buffer and raises the block-completion IRQ a game waits on. If a failure to
 * open waveOut stopped the pump, every SB game would hang on a silent machine.
 * So on failure we keep calling the fill callback at real-time pace and discard
 * the samples.
 */
#ifndef NTVDMEX_AUDIO_WAVE_H
#define NTVDMEX_AUDIO_WAVE_H

#include <windows.h>
#include <stdint.h>

#define AW_BUFFERS   6          /* buffers in flight (~70 ms of slack at 44100)   */
#define AW_FRAMES  512          /* frames per buffer (~11.6ms at 44100)          */
/* ► THE BUFFER COUNT IS THE AUDIO LEAD, AND THE LEAD IS A SUSPECT. Every queued
     buffer is 11.6 ms that our DMA read pointer runs AHEAD of what is audible, and
     the guest has to refill a block before we reach it. Doom's longest protected-mode
     stretch with no host turn was measured at 62.8 ms against a 70 ms lead -- close
     enough that the lead cannot be assumed innocent. So it is a RUNTIME field, set
     from awbufs.txt before the pump starts, and the SB's replay counter is scored
     against it: change one number, read one number back. Clamped to [2, AW_BUFFERS];
     0 means "use them all". */

/* Fill `frames` mono 16-bit samples. Called on the audio thread; the host wraps
   it in the same lock the exec thread uses for the device bus. */
typedef void (*aw_fill_fn)(void *ctx, int16_t *out, uint32_t frames);

typedef struct audio_wave {
    HMODULE   mod;
    HANDLE    thread, event;
    volatile LONG running;
    void     *hwo;                      /* HWAVEOUT, opaque here                 */
    void     *hmidi;                    /* HMIDIOUT, opaque here                 */
    uint32_t  hz;
    aw_fill_fn fill; void *ctx;
    int       silent;                   /* 1 = no device; pump but discard       */
    uint32_t  underruns;
    uint32_t  nbufs;                    /* buffers actually queued: the LEAD     */

    /* WAVEHDR + sample storage, allocated inline to avoid a heap dependency */
    unsigned char hdr[AW_BUFFERS][32];  /* WAVEHDR is 32 bytes on win32          */
    int16_t   buf[AW_BUFFERS][AW_FRAMES];
} audio_wave;

/* Start the audio pump. Returns 0 on success, 1 if it fell back to silent pumping
   (still a success as far as the guest is concerned). */
int  audio_wave_start(audio_wave *aw, uint32_t hz, aw_fill_fn fill, void *ctx);
void audio_wave_stop(audio_wave *aw);

/* Send one packed MIDI short message (status | d1<<8 | d2<<16) to the host synth.
   Safe to call when MIDI never opened -- it is simply dropped. */
void audio_wave_midi(audio_wave *aw, uint32_t msg);

#endif /* NTVDMEX_AUDIO_WAVE_H */

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

#define AW_BUFFERS      24      /* CAP on buffers in flight (storage is sized to it) */
#define AW_FRAMES      512      /* CAP on frames per buffer                          */
/* ⚠ THE CAP AND THE DEFAULT MUST BE SEPARATE CONSTANTS. They used to be one: the
     clamp read `if (want_bufs < 2) want_bufs = AW_BUFFERS;`, i.e. "0 means use them
     all", which was 6 and therefore also the default. Raising the cap to 24 without
     splitting these would have silently moved the DEFAULT lead from 70 ms to 280 ms
     and every later run would have been measuring a different machine. */
#define AW_DEF_BUFFERS   6      /* default lead   = 6 x 512 / 44100 = ~70 ms       */
#define AW_DEF_FRAMES  512      /* default step   = 11.6 ms per mixer burst        */
#define AW_MIN_FRAMES   64      /* below this the per-buffer callback overhead wins */
/* ── ⚠ THE BUFFER SIZE IS THE DMA POSITION'S GRANULARITY, WHICH IS A SEPARATE
     SUSPECT FROM THE LEAD. ────────────────────────────────────────────────────────
     The guest's DMA read pointer ONLY advances while the mixer runs, and the mixer
     runs one whole waveOut buffer at a time. At 512 frames that is 241 source bytes
     -- 0.94 of a 256-byte block -- consumed in ONE burst every 11.6 ms, after which
     the position is frozen. DMX polls it every ~7.1 ms and steers its refills by it,
     so it sees `0, 0, +241` where real hardware would creep ~148 bytes per poll: a
     staircase where the hardware gives a ramp. Its writes then land in the wrong
     block, which is what the ring shows -- 20% of blocks silent, 30% of the rest a
     verbatim repeat of the previous lap.
   ► SMALLER BUFFERS, MORE OF THEM, SAME TOTAL LEAD. nframes x nbufs is what matters
     for the lead; nframes alone is the granularity. Making both runtime lets ONE
     binary produce baseline and treatment with no rebuild, so the comparison cannot
     be confounded by anything else that changed -- and lets the difference be A/B'd
     by ear. 128 frames x 24 buffers is the same 70 ms lead at a quarter of the step. */
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
    /* ── ⚠ `underruns` COUNTS waveOutWrite FAILURES, WHICH IS NOT STARVATION. ────────
         It has read 0 in every run ever made, including runs the user describes as
         audibly broken, because waveOutWrite does not fail when the QUEUE runs dry --
         it succeeds, having been called too late. When the last queued buffer finishes
         before we hand the driver the next one, the DEVICE plays silence, and that is a
         gap at the speaker that nothing here can see. Every audio counter in this
         project measures either the guest's ring or our sample stream; neither is what
         reaches the ear.
       ► COUNT THE QUEUE DEPTH INSTEAD, AND COUNT IT AT ITS WORST. On each pass, how
         many buffers has the driver already handed back? That many are NOT queued. If
         it equals nbufs the queue was completely empty and the device definitely ran
         dry. The histogram gives the MARGIN rather than a pass/fail: `drain` sitting at
         nbufs-1 means we are one buffer from silence the whole time, which a "starved=0"
         would have reported as healthy. */
    uint32_t  starved;                  /* passes with EVERY buffer handed back    */
    uint32_t  drain_max;                /* worst simultaneous handed-back count    */
    uint32_t  drain_hist[AW_BUFFERS + 1];
    uint32_t  nbufs;                    /* buffers actually queued: the LEAD     */
    uint32_t  nframes;                  /* frames per buffer: the GRANULARITY    */

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

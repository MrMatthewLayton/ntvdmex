/*
 * audio_rec.h -- record what the host plays, to a .WAV file.  (s81)
 *
 * WHY. Two jobs, one mechanism. Tools > Capture > Record Audio is a product feature the
 * user asked for; and "Doom's quit sound is glitchy" is a claim about what reaches the
 * speaker that no counter in this project can see -- every audio statistic measures the
 * guest's ring or our sample stream in aggregate, never the waveform itself. A recording
 * of the exact samples handed to waveOut makes it something to look at.
 *
 * SHAPE. The mixer's output is mono 16-bit at the device rate (audio_wave.c opens it that
 * way), and it is fed from the audio thread, which runs at TIME_CRITICAL on an ~11.6 ms
 * deadline. File I/O does not belong there: a disk-cache flush would become an audible
 * underrun only while recording, the one time someone is listening closely. So the audio
 * thread only copies into a lock-free single-producer/single-consumer ring, and a writer
 * thread at normal priority drains it to disk. A full ring DROPS samples and counts them,
 * rather than blocking the audio thread; `dropped` is reported so a recording with holes
 * says so.
 *
 * One recorder per process -- so this header is included by ONE translation unit only,
 * audio_wave.c (the feeder), which exports aw_rec_* for everyone else. Including it a
 * second time would silently create a second, unfed recorder.
 */
#ifndef NTVDMEX_AUDIO_REC_H
#define NTVDMEX_AUDIO_REC_H

#include <windows.h>
#include <stdint.h>

#define AREC_RING (1u << 18)                 /* 262144 samples: ~3 s of stereo at 44.1 kHz */

typedef struct audio_rec {
    HANDLE          file, thread;
    volatile LONG   active;                  /* 1 = the audio thread may feed      */
    volatile LONG   stopping;                /* 1 = writer: drain, finish, exit    */
    volatile LONG   head, tail;              /* ring indices (mod AREC_RING)       */
    uint32_t        hz;
    uint32_t        data_bytes;              /* written to disk so far             */
    uint32_t        dropped;                 /* samples lost to a full ring        */
    int16_t         ring[AREC_RING];
} audio_rec;

static audio_rec g_arec;

static void arec_put32(BYTE *b, uint32_t v)
{ b[0] = (BYTE)v; b[1] = (BYTE)(v >> 8); b[2] = (BYTE)(v >> 16); b[3] = (BYTE)(v >> 24); }
static void arec_put16(BYTE *b, uint32_t v) { b[0] = (BYTE)v; b[1] = (BYTE)(v >> 8); }

/* The canonical 44-byte PCM header; sizes are patched when the recording stops. */
static void arec_header(BYTE h[44], uint32_t hz, uint32_t data)
{
    static const char tag[] = "RIFF....WAVEfmt ";
    int i;
    for (i = 0; i < 16; ++i) h[i] = (BYTE)tag[i];
    arec_put32(h + 4, 36 + data);
    arec_put32(h + 16, 16);                  /* fmt chunk size   */
    arec_put16(h + 20, 1);                   /* PCM              */
    arec_put16(h + 22, 2);                   /* stereo (#189)    */
    arec_put32(h + 24, hz);
    arec_put32(h + 28, hz * 4);              /* bytes per second */
    arec_put16(h + 32, 4);                   /* block align: L+R */
    arec_put16(h + 34, 16);                  /* bits per sample  */
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    arec_put32(h + 40, data);
}

/* Drain whatever is in the ring to the file. Writer thread only. */
static void arec_drain(audio_rec *r)
{
    for (;;) {
        LONG t = r->tail, h = r->head;
        DWORD n, wr = 0;
        if (t == h) return;
        n = (DWORD)((h > t) ? (h - t) : ((LONG)AREC_RING - t));   /* contiguous run */
        WriteFile(r->file, &r->ring[t], n * sizeof(int16_t), &wr, NULL);
        r->data_bytes += wr;
        InterlockedExchange(&r->tail, (LONG)((t + (LONG)n) & (AREC_RING - 1)));
    }
}

static DWORD WINAPI arec_writer(LPVOID pv)
{
    audio_rec *r = (audio_rec *)pv;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!r->stopping) { arec_drain(r); Sleep(50); }
    arec_drain(r);
    return 0;
}

static int audio_rec_active(void) { return g_arec.active != 0; }

/* Start recording to `path`. 0 = started; -1 = already recording or cannot create. */
static int audio_rec_start(const char *path, uint32_t hz)
{
    audio_rec *r = &g_arec;
    BYTE h[44]; DWORD wr = 0;
    if (r->active || r->file) return -1;
    r->file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (r->file == INVALID_HANDLE_VALUE) { r->file = 0; return -1; }
    r->hz = hz ? hz : 44100;
    r->head = r->tail = 0; r->data_bytes = 0; r->dropped = 0; r->stopping = 0;
    arec_header(h, r->hz, 0);
    WriteFile(r->file, h, sizeof h, &wr, NULL);
    r->thread = CreateThread(NULL, 0, arec_writer, r, 0, NULL);
    if (!r->thread) { CloseHandle(r->file); r->file = 0; return -1; }
    InterlockedExchange(&r->active, 1);
    return 0;
}

/* Stop, flush, patch the sizes, close. Safe to call when not recording. Returns the
   number of samples written (0 if nothing was recording). */
static uint32_t audio_rec_stop(void)
{
    audio_rec *r = &g_arec;
    BYTE h[44]; DWORD wr = 0;
    if (!r->file) return 0;
    InterlockedExchange(&r->active, 0);        /* the audio thread stops feeding   */
    InterlockedExchange(&r->stopping, 1);
    WaitForSingleObject(r->thread, 2000);
    CloseHandle(r->thread); r->thread = 0;
    arec_drain(r);                             /* anything fed after the last pass */
    arec_header(h, r->hz, r->data_bytes);
    SetFilePointer(r->file, 0, NULL, FILE_BEGIN);
    WriteFile(r->file, h, sizeof h, &wr, NULL);
    CloseHandle(r->file); r->file = 0;
    return r->data_bytes / 4;                  /* frames (L/R pairs) */
}

/* Audio thread: copy `n` samples in, or drop them if the ring is full. Never blocks. */
static void audio_rec_feed(const int16_t *s, uint32_t n)
{
    audio_rec *r = &g_arec;
    uint32_t i;
    LONG h, t, room;
    if (!r->active) return;
    h = r->head; t = r->tail;
    room = (LONG)(AREC_RING - 1) - ((h - t) & (LONG)(AREC_RING - 1));
    if ((LONG)n > room) { r->dropped += n; return; }
    for (i = 0; i < n; ++i) r->ring[(h + (LONG)i) & (AREC_RING - 1)] = s[i];
    InterlockedExchange(&r->head, (LONG)((h + (LONG)n) & (AREC_RING - 1)));
}

#endif

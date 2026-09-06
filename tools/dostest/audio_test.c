/* audio_test.c -- off-VM battery for the audio mixer (vdd_audio.c).
 *
 * The mixer's job is two-fold and the second half is easy to forget: it makes
 * sound audible, AND it is the transport that pulls samples through the Sound
 * Blaster, which is what raises the block-completion IRQ a game waits on. So the
 * battery checks both -- that resampling preserves pitch and level, and that
 * merely mixing drives an SB transfer to its IRQ.
 */
#include <stdio.h>
#include <string.h>
#include "vdd_audio.h"
#include "vdd_dma.h"

static int total = 0, fails = 0;
#define CHECK(c,m) do{ total++; if(c){printf("  PASS  %s\n",(m));} \
    else{printf("  FAIL  %s\n",(m)); fails++;} }while(0)

static uint8_t g_flat[0x100000];
static vdd_bus bus;
static dma_state dma;
static opl_state opl;
static sb_state  sb;
static audio_state mix;
static int g_irq_count;

static void irq_sink(void *ctx, uint8_t irq) { (void)ctx; (void)irq; g_irq_count++; }
static void wr(uint16_t p, uint8_t v){ uint32_t x=v; vdd_bus_io(&bus,p,1,0,&x); }

#define BASE 0x220
static int16_t buf[44100];

static double measure_hz(const int16_t *s, int n, int rate)
{
    int i, cross = 0, prev = 0;
    for (i = 0; i < n; ++i) { int cur = s[i] > 0; if (cur && !prev) cross++; prev = cur; }
    return (double)cross * rate / (double)n;
}
static long rms(const int16_t *s, int n)
{ long long a = 0; int i; for (i = 0; i < n; ++i) a += (long long)s[i]*s[i]; return (long)(a/(n?n:1)); }

/* Program DMA channel 1 (mode byte carries the channel in bits 0-1). */
static void dma_program(uint32_t phys, uint16_t len, int autoinit)
{
    uint32_t v;
    v = 0;                       vdd_bus_io(&bus, 0x0C, 1, 0, &v);
    v = phys & 0xFF;             vdd_bus_io(&bus, 0x02, 1, 0, &v);
    v = (phys >> 8) & 0xFF;      vdd_bus_io(&bus, 0x02, 1, 0, &v);
    v = (len - 1) & 0xFF;        vdd_bus_io(&bus, 0x03, 1, 0, &v);
    v = ((len - 1) >> 8) & 0xFF; vdd_bus_io(&bus, 0x03, 1, 0, &v);
    v = (phys >> 16) & 0xFF;     vdd_bus_io(&bus, 0x83, 1, 0, &v);
    v = 0x48 | 0x01 | (autoinit ? 0x10u : 0u); vdd_bus_io(&bus, 0x0B, 1, 0, &v);
    v = 0x01;                    vdd_bus_io(&bus, 0x0A, 1, 0, &v);
}

int main(void)
{
    uint32_t i;

    printf("== sound epic: audio mixer battery ==\n");

    memset(&dma,0,sizeof dma); memset(&opl,0,sizeof opl); memset(&sb,0,sizeof sb);
    memset(g_flat,0,sizeof g_flat);
    vdd_bus_init(&bus, g_flat);
    vdd_bus_set_sinks(&bus, irq_sink, 0, 0, 0);
    { ntvdd d = vdd_dma_device(&dma); vdd_bus_add(&bus, &d); }
    { ntvdd d = vdd_opl_device(&opl); vdd_bus_add(&bus, &d); }
    sb.dma = &dma; sb.opl = &opl; sb.base = BASE;
    { ntvdd d = vdd_sb_device(&sb);  CHECK(vdd_bus_add(&bus, &d) == 0, "add: devices on the bus"); }
    vdd_audio_init(&mix, &opl, &sb, AUDIO_OUT_HZ);

    /* T1: silence in, silence out ------------------------------------------ */
    vdd_audio_mix(&mix, buf, 1024);
    CHECK(rms(buf, 1024) == 0, "idle: mixes silence");
    CHECK(mix.frames_mixed == 1024, "idle: still produced the requested frames");

    /* T2: an FM note survives resampling at the right PITCH ----------------- */
    /* fnum 0x200 block 4 = 388.4 Hz; the OPL renders at 49716 and the mixer
       resamples to 44100, so a pitch error here means the resampler is wrong. */
    {
        int m = vdd_opl_op_index(0,0), c = vdd_opl_op_index(0,1);
        uint8_t mr = (uint8_t)(m + 2*(m/6)), cr = (uint8_t)(c + 2*(c/6));
        double hz;
        vdd_opl_write_reg(&opl, (uint8_t)(0x20+mr), 0x21);
        vdd_opl_write_reg(&opl, (uint8_t)(0x40+mr), 0x3F);   /* modulator silent */
        vdd_opl_write_reg(&opl, (uint8_t)(0x20+cr), 0x21);
        vdd_opl_write_reg(&opl, (uint8_t)(0x40+cr), 0x00);
        vdd_opl_write_reg(&opl, (uint8_t)(0x60+cr), 0xF0);
        vdd_opl_write_reg(&opl, (uint8_t)(0x80+cr), 0x0F);
        vdd_opl_write_reg(&opl, 0xC0, 0x01);
        vdd_opl_write_reg(&opl, 0xA0, 0x00);
        vdd_opl_write_reg(&opl, 0xB0, (uint8_t)(0x20 | (4 << 2) | 0x02));
        vdd_audio_mix(&mix, buf, 22050);                     /* half a second    */
        hz = measure_hz(buf, 22050, AUDIO_OUT_HZ);
        printf("        resampled pitch %.1f Hz, expected 388.4 Hz\n", hz);
        CHECK(hz > 380.0 && hz < 397.0, "FM: pitch survives 49716 -> 44100 resampling");
        CHECK(rms(buf, 22050) > 10000, "FM: audible level after mixing");
    }

    /* T3: mixing DRIVES an SB transfer to its IRQ ---------------------------- */
    /* This is the bit that unblocks a game: nothing else pulls DMA data.       */
    for (i = 0; i < 512; ++i) g_flat[0x40000 + i] = 0x80;    /* silence, 8-bit   */
    dma_program(0x40000, 512, 0);
    wr(BASE + 0xC, 0x40); wr(BASE + 0xC, 165);               /* ~11 kHz          */
    g_irq_count = 0;
    wr(BASE + 0xC, 0x14); wr(BASE + 0xC, 0xFF); wr(BASE + 0xC, 0x01);  /* 512 B  */
    CHECK(vdd_sb_active(&sb), "SB: transfer armed");
    CHECK(g_irq_count == 0, "SB: no IRQ before the mixer runs");
    /* 512 source samples at ~11 kHz is ~46ms; mix a comfortable margin of it.  */
    vdd_audio_mix(&mix, buf, 4096);
    CHECK(g_irq_count >= 1, "SB: mixing alone drives the block to completion IRQ  <-- THE TEST");
    CHECK(!vdd_sb_active(&sb), "SB: single-cycle transfer finished");

    /* T4: SB audio actually reaches the output ------------------------------ */
    {
        long r;
        for (i = 0; i < 512; ++i) g_flat[0x41000 + i] = (uint8_t)((i & 32) ? 0xE0 : 0x20);
        dma_program(0x41000, 512, 1);
        wr(BASE + 0xC, 0x48); wr(BASE + 0xC, 0xFF); wr(BASE + 0xC, 0x01);
        wr(BASE + 0xC, 0x1C);                                /* auto-init        */
        vdd_opl_write_reg(&opl, 0xB0, 0x00);                 /* silence the FM   */
        vdd_audio_mix(&mix, buf, 8192);
        r = rms(buf, 8192);
        printf("        SB-only rms=%ld\n", r);
        CHECK(r > 1000, "SB: sampled audio is present in the mix");
    }

    /* T5: an auto-init ring keeps raising IRQs while mixing continues -------- */
    g_irq_count = 0;
    vdd_audio_mix(&mix, buf, 16384);
    CHECK(g_irq_count >= 2, "SB: auto-init keeps producing IRQs as the mixer runs");
    CHECK(vdd_sb_active(&sb), "SB: auto-init still streaming");

    /* T6: rate changes are picked up mid-stream ----------------------------- */
    wr(BASE + 0xC, 0x41); wr(BASE + 0xC, 0x56); wr(BASE + 0xC, 0x22);   /* 22050 */
    vdd_audio_mix(&mix, buf, 2048);
    CHECK(mix.r_sb.src_hz == 22050, "resampler: follows a mid-stream rate change");

    /* T7: a realistic mix has headroom; a pathological one clamps cleanly --- */
    /* Nine full-volume FM channels PLUS full-scale digital audio saturates a real
       SB16 too, so asserting "never clips" there would be asserting something
       false. What must hold is that a normal score stays clear of the rail, and
       that overload clamps rather than wrapping (which would sound like a bang). */
    {
        int clipped = 0, k;
        vdd_opl_write_reg(&opl, 0xB0, 0x00);                 /* all notes off    */
        for (k = 0; k < 9; ++k) vdd_opl_write_reg(&opl, (uint8_t)(0xB0+k), 0x00);
        for (k = 0; k < 3; ++k) {                            /* three voices,    */
            int c2 = vdd_opl_op_index(k,1); uint8_t cr2 = (uint8_t)(c2 + 2*(c2/6));
            int m2 = vdd_opl_op_index(k,0); uint8_t mr2 = (uint8_t)(m2 + 2*(m2/6));
            vdd_opl_write_reg(&opl, (uint8_t)(0x40+mr2), 0x3F);
            vdd_opl_write_reg(&opl, (uint8_t)(0x20+cr2), 0x21);
            vdd_opl_write_reg(&opl, (uint8_t)(0x40+cr2), 0x10);   /* moderate TL */
            vdd_opl_write_reg(&opl, (uint8_t)(0x60+cr2), 0xF0);
            vdd_opl_write_reg(&opl, (uint8_t)(0x80+cr2), 0x0F);
            vdd_opl_write_reg(&opl, (uint8_t)(0xA0+k), 0x40);
            vdd_opl_write_reg(&opl, (uint8_t)(0xB0+k), (uint8_t)(0x20 | (4 << 2) | 1));
        }
        vdd_audio_mix(&mix, buf, 8192);
        for (i = 0; i < 8192; ++i) if (buf[i] == 32767 || buf[i] == -32768) clipped++;
        printf("        realistic mix: %d of 8192 at the rail\n", clipped);
        CHECK(clipped == 0, "mix: a realistic score plus sampled audio has headroom");
        CHECK(rms(buf, 8192) > 1000, "mix: ...and is still clearly audible");

        /* now overload it deliberately and check it clamps, never wraps */
        for (k = 0; k < 9; ++k) {
            int c2 = vdd_opl_op_index(k,1); uint8_t cr2 = (uint8_t)(c2 + 2*(c2/6));
            vdd_opl_write_reg(&opl, (uint8_t)(0x20+cr2), 0x21);
            vdd_opl_write_reg(&opl, (uint8_t)(0x40+cr2), 0x00);   /* full volume */
            vdd_opl_write_reg(&opl, (uint8_t)(0x60+cr2), 0xF0);
            vdd_opl_write_reg(&opl, (uint8_t)(0x80+cr2), 0x0F);
            vdd_opl_write_reg(&opl, (uint8_t)(0xA0+k), 0x40);
            vdd_opl_write_reg(&opl, (uint8_t)(0xB0+k), (uint8_t)(0x20 | (5 << 2) | 1));
        }
        vdd_audio_mix(&mix, buf, 4096);
        { int wrapped = 0;
          for (i = 1; i < 4096; ++i)
              if ((buf[i-1] > 30000 && buf[i] < -30000) || (buf[i-1] < -30000 && buf[i] > 30000))
                  wrapped++;
          /* a genuine waveform can cross fast; a WRAP shows up as many such jumps */
          printf("        overload: %d fast rail-to-rail transitions\n", wrapped);
          CHECK(wrapped < 4096 / 20, "mix: overload clamps rather than wrapping"); }
    }

    /* ── THE TRANSPORT MUST NOT EAT SAMPLES IT DOES NOT PLAY. ───────────────────────
         vdd_sb_render() pulls out of the guest's DMA ring, so any sample the mixer asks
         for and then discards is data the game wrote and nobody hears. rs_need() used
         to ask for two extra every chunk "for the pair being interpolated between",
         which at Doom's rate is 172 dropped PCM samples a second: the read pointer
         walks away from the guest's write pointer and you hear a click at chunk rate.
         Drive a whole number of chunks at an exact 4:1 ratio and check the ring
         advanced by the arithmetic amount and no more. */
    {
        uint32_t before, after, want, chunks = 8;
        for (i = 0; i < 4096; ++i) g_flat[0x50000 + i] = 0x80;
        dma_program(0x50000, 4096, 1);                       /* auto-init ring   */
        wr(BASE + 0xC, 0x41); wr(BASE + 0xC, 0x2B); wr(BASE + 0xC, 0x11);  /* 11025 Hz */
        wr(BASE + 0xC, 0xC6); wr(BASE + 0xC, 0x00);          /* 8-bit auto, mono */
        wr(BASE + 0xC, 0xFF); wr(BASE + 0xC, 0x0F);          /* 4096-byte block  */
        vdd_audio_mix(&mix, buf, 512);                       /* prime the pair   */
        before = sb.block_left;
        for (i = 0; i < chunks; ++i) vdd_audio_mix(&mix, buf, 512);
        after = sb.block_left;
        want = chunks * 512u * 11025u / AUDIO_OUT_HZ;        /* exactly 1:4      */
        printf("        ring consumed %u over %u chunks, arithmetic says %u\n",
               before - after, chunks, want);
        CHECK(before - after == want,
              "resampler pulls exactly what it plays (no DMA samples discarded)");
    }

    /* ── THE PC SPEAKER, WHICH MADE NO SOUND AT ALL UNTIL NOW. ─────────────────────
         vdd_speaker.c has modelled port 0x61 and reported the tone since M3, and
         nothing ever turned that into a sample -- so every beep a guest made went
         nowhere while the score line read "SB16 PCM, OPL2/3 FM, MPU-401, speaker".
         The gate is TWO bits, and a program that sets only one of them is clicking
         the cone rather than sounding a tone, so that distinction is measured here
         rather than assumed. */
    {
        pit_state pit;      ntvdd pdev;
        speaker_state spk;  ntvdd sdev;
        int16_t sbuf[4410];
        double hz;
        long full, half;
        uint32_t v;

        memset(&pit, 0, sizeof pit); memset(&spk, 0, sizeof spk);
        spk.pit = &pit;
        pdev = vdd_pit_device(&pit);  sdev = vdd_speaker_device(&spk);
        CHECK(vdd_bus_add(&bus, &pdev) == 0, "speaker: PIT joined the mixer's bus");
        CHECK(vdd_bus_add(&bus, &sdev) == 0, "speaker: and the speaker VDD did too");

        /* Programmed THROUGH the chip, not by poking the struct, so what this
           measures is what a guest that writes 0x43/0x42 actually hears. */
        v = 0xB6;         vdd_bus_io(&bus, 0x43, 1, 0, &v);
        v = 1193 & 0xFF;  vdd_bus_io(&bus, 0x42, 1, 0, &v);
        v = 1193 >> 8;    vdd_bus_io(&bus, 0x42, 1, 0, &v);
        CHECK(pit_ch2_hz(&pit) == PIT_INPUT_HZ / 1193,
              "speaker: PIT channel 2 divisor 1193 gives ~1000 Hz");

        mix.opl = NULL; mix.sb = NULL;              /* the speaker alone in the mix */
        vdd_audio_set_speaker(&mix, &spk, 1);
        v = 0x03; vdd_bus_io(&bus, 0x61, 1, 0, &v); /* gate + data = sounding       */
        memset(sbuf, 0, sizeof sbuf);
        vdd_audio_mix(&mix, sbuf, 4410);
        hz = measure_hz(sbuf, 4410, AUDIO_OUT_HZ);
        printf("        speaker tone measured %.0f Hz (the chip is at %u)\n",
               hz, (unsigned)pit_ch2_hz(&pit));
        CHECK(hz > 960 && hz < 1040, "speaker: an active gate produces the PIT's tone");
        CHECK(rms(sbuf, 4410) > 1000, "speaker: and it is actually audible");

        v = 0x01; vdd_bus_io(&bus, 0x61, 1, 0, &v); /* gate only, no data enable    */
        memset(sbuf, 0, sizeof sbuf);
        vdd_audio_mix(&mix, sbuf, 512);
        CHECK(rms(sbuf, 512) == 0, "speaker: the gate bit ALONE is silent (both bits gate)");

        v = 0x03; vdd_bus_io(&bus, 0x61, 1, 0, &v);
        vdd_audio_set_speaker(&mix, &spk, 0);       /* fitted, switched off         */
        memset(sbuf, 0, sizeof sbuf);
        vdd_audio_mix(&mix, sbuf, 512);
        CHECK(rms(sbuf, 512) == 0, "speaker: the setting silences it without unfitting it");

        /* ⚠ A "tone" above the output rate's Nyquist point is not a tone, it is
             alias noise, so it is refused rather than synthesised. */
        vdd_audio_set_speaker(&mix, &spk, 1);
        v = 0xB6; vdd_bus_io(&bus, 0x43, 1, 0, &v);
        v = 0x01; vdd_bus_io(&bus, 0x42, 1, 0, &v);
        v = 0x00; vdd_bus_io(&bus, 0x42, 1, 0, &v); /* divisor 1 -> 1.19 MHz        */
        memset(sbuf, 0, sizeof sbuf);
        vdd_audio_mix(&mix, sbuf, 512);
        CHECK(rms(sbuf, 512) == 0, "speaker: a tone past 20 kHz is refused, not aliased");

        /* ── THE MASTER ATTENUATOR SITS AFTER THE SUM. ─────────────────────────────
             That is where a volume control lives on a real machine: the guest's own
             mixer registers still do their work underneath it. Measured on the
             SPEAKER because its level is exactly known -- a square wave at a fixed
             amplitude -- so "half" is arithmetic, not a judgement about loudness. */
        v = 0xB6;         vdd_bus_io(&bus, 0x43, 1, 0, &v);
        v = 1193 & 0xFF;  vdd_bus_io(&bus, 0x42, 1, 0, &v);
        v = 1193 >> 8;    vdd_bus_io(&bus, 0x42, 1, 0, &v);

        vdd_audio_set_master(&mix, 100, 0);
        memset(sbuf, 0, sizeof sbuf); vdd_audio_mix(&mix, sbuf, 512);
        full = rms(sbuf, 512);
        vdd_audio_set_master(&mix, 50, 0);
        memset(sbuf, 0, sizeof sbuf); vdd_audio_mix(&mix, sbuf, 512);
        half = rms(sbuf, 512);
        printf("        mean square at 100%% = %ld, at 50%% = %ld (power, so ~4x)\n",
               full, half);
        CHECK(half * 3 < full && half * 5 > full,
              "master volume 50 halves the amplitude (a quarter of the power)");

        vdd_audio_set_master(&mix, 100, 1);
        memset(sbuf, 0, sizeof sbuf); vdd_audio_mix(&mix, sbuf, 512);
        CHECK(rms(sbuf, 512) == 0, "mute outputs silence");
        vdd_audio_set_master(&mix, 100, 0);
        memset(sbuf, 0, sizeof sbuf); vdd_audio_mix(&mix, sbuf, 512);
        CHECK(rms(sbuf, 512) > 0, "...and unmuting restores the volume it kept");

        vdd_audio_set_master(&mix, 4000, 0);
        CHECK(mix.master == 100, "a volume past 100 is clamped, never wrapped into a gain");
        vdd_audio_set_speaker(&mix, NULL, 0);
    }

    /* ── vdd_audio_init MUST NOT LEAVE THE MASTER AT ZERO. ─────────────────────────
         It zeroes the whole struct, which is right for every other field and would
         be SILENCE for this one -- the exact shape of bug that ships as "no sound on
         a clean machine" and is invisible to any test that sets the volume first. */
    {
        audio_state fresh;
        vdd_audio_init(&fresh, NULL, NULL, 0);
        CHECK(fresh.master == 100, "a freshly initialised mixer is at full volume, not zero");
        CHECK(fresh.muted == 0,    "...and unmuted");
        CHECK(fresh.out_hz == AUDIO_OUT_HZ, "...and out_hz 0 means the default rate");
        CHECK(fresh.spk_level == 0, "...and the speaker is unfitted until it is fitted");
        vdd_audio_init(&fresh, NULL, NULL, 22050);
        CHECK(fresh.out_hz == 22050, "a requested output rate is honoured");
    }

    printf("-- %d checks, %d failures --\n", total, fails);
    return fails ? 1 : 0;
}

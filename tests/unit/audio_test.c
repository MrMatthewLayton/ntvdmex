/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * Off-VM battery for the audio mixer (vdd_audio.c).
 *
 * The mixer's job is two-fold and the second half is easy to forget: it makes
 * sound audible, AND it is the transport that pulls samples through the Sound
 * Blaster, which is what raises the block-completion IRQ a game waits on. So the
 * battery checks both -- that resampling preserves pitch and level, and that
 * merely mixing drives an SB transfer to its IRQ.
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

#include <stdio.h>
#include <string.h>
#include "vdd_audio.h"
#include "vdd_dma.h"
#define CHECK(condition,message) do{ g_Total++; if(condition){printf("  PASS  %s\n",(message));} \
    else{printf("  FAIL  %s\n",(message)); g_Failures++;} }while(0)

#define BASE    0x220

static INT g_Total = 0;
static INT g_Failures = 0;

static BYTE g_GuestMemory[0x100000];
static VDD_BUS g_Bus;
static DMA_STATE g_Dma;
static OPL_STATE g_Opl;
static SB_STATE  g_Sb;
static AUDIO_STATE g_Mixer;
static INT g_IrqCount;
static INT16 g_Samples[44100];

static VOID AudioTestIrqSink(PVOID context, BYTE irq)
{
    (VOID)context;
    (VOID)irq;
    g_IrqCount++;
}

static VOID AudioTestWrite(WORD port, BYTE byteValue)
{
    UINT32 value=byteValue;

    VddBusIo(&g_Bus,port,1,0,&value);
}

static double AudioTestMeasureHz(const INT16 *samples, INT count, INT rate)
{
    INT index;
    INT crossings = 0;
    INT wasPositive = 0;

    for (index = 0; index < count; ++index)
    {
        INT isPositive = samples[index] > 0;

        if (isPositive && !wasPositive)
            crossings++;

        wasPositive = isPositive;
    }

    return (double)crossings * rate / (double)count;
}

static long AudioTestRms(const INT16 *samples, INT count)
{
    long long sumOfSquares = 0;
    INT index;

    for (index = 0; index < count; ++index)
        sumOfSquares += (long long)samples[index]*samples[index];

    return (long)(sumOfSquares/(count?count:1));
}

/* Program DMA channel 1 (mode byte carries the channel in bits 0-1). */
static VOID AudioTestDmaProgram(UINT32 physical, WORD length, INT isAutoInit)
{
    UINT32 value;

    value = 0;
    VddBusIo(&g_Bus, 0x0C, 1, 0, &value);
    value = physical & 0xFF;
    VddBusIo(&g_Bus, 0x02, 1, 0, &value);
    value = (physical >> 8) & 0xFF;
    VddBusIo(&g_Bus, 0x02, 1, 0, &value);
    value = (length - 1) & 0xFF;
    VddBusIo(&g_Bus, 0x03, 1, 0, &value);
    value = ((length - 1) >> 8) & 0xFF;
    VddBusIo(&g_Bus, 0x03, 1, 0, &value);
    value = (physical >> 16) & 0xFF;
    VddBusIo(&g_Bus, 0x83, 1, 0, &value);
    value = 0x48 | 0x01 | (isAutoInit ? 0x10u : 0u);
    VddBusIo(&g_Bus, 0x0B, 1, 0, &value);
    value = 0x01;
    VddBusIo(&g_Bus, 0x0A, 1, 0, &value);
}

INT main(VOID)
{
    UINT32 index;

    printf("== sound epic: audio mixer battery ==\n");

    memset(&g_Dma,0,sizeof g_Dma);
    memset(&g_Opl,0,sizeof g_Opl);
    memset(&g_Sb,0,sizeof g_Sb);
    memset(g_GuestMemory,0,sizeof g_GuestMemory);
    VddBusInitialize(&g_Bus, g_GuestMemory);
    VddBusSetSinks(&g_Bus, AudioTestIrqSink, 0, 0, 0);
    {
        NTVDD_DEVICE device = VddDmaDevice(&g_Dma);
        VddBusAdd(&g_Bus, &device);
    }
    {
        NTVDD_DEVICE device = VddOplDevice(&g_Opl);
        VddBusAdd(&g_Bus, &device);
    }
    g_Sb.Dma = &g_Dma;
    g_Sb.Opl = &g_Opl;
    g_Sb.BasePort = BASE;
    {
        NTVDD_DEVICE device = VddSbDevice(&g_Sb);
        CHECK(VddBusAdd(&g_Bus, &device) == 0, "add: devices on the bus");
    }
    VddAudioInitialize(&g_Mixer, &g_Opl, &g_Sb, AUDIO_OUTPUT_HZ);

    /* T1: silence in, silence out ------------------------------------------ */
    VddAudioMix(&g_Mixer, g_Samples, 1024);
    CHECK(AudioTestRms(g_Samples, 1024) == 0, "idle: mixes silence");
    CHECK(g_Mixer.FramesMixed == 1024, "idle: still produced the requested frames");

    /* T2: an FM note survives resampling at the right PITCH ----------------- */
    /* fnum 0x200 block 4 = 388.4 Hz; the OPL renders at 49716 and the mixer
     * resamples to 44100, so a pitch error here means the resampler is wrong.
     */
    {
        INT modulator = VddOplOperatorIndex(0,0);
        INT carrier = VddOplOperatorIndex(0,1);
        BYTE modulatorRegister = (BYTE)(modulator + 2*(modulator/6));
        BYTE carrierRegister = (BYTE)(carrier + 2*(carrier/6));
        double frequency;
        VddOplWriteRegister(&g_Opl, (BYTE)(0x20+modulatorRegister), 0x21);
        VddOplWriteRegister(&g_Opl, (BYTE)(0x40+modulatorRegister), 0x3F);   /* modulator silent */
        VddOplWriteRegister(&g_Opl, (BYTE)(0x20+carrierRegister), 0x21);
        VddOplWriteRegister(&g_Opl, (BYTE)(0x40+carrierRegister), 0x00);
        VddOplWriteRegister(&g_Opl, (BYTE)(0x60+carrierRegister), 0xF0);
        VddOplWriteRegister(&g_Opl, (BYTE)(0x80+carrierRegister), 0x0F);
        VddOplWriteRegister(&g_Opl, 0xC0, 0x01);
        VddOplWriteRegister(&g_Opl, 0xA0, 0x00);
        VddOplWriteRegister(&g_Opl, 0xB0, (BYTE)(0x20 | (4 << 2) | 0x02));
        VddAudioMix(&g_Mixer, g_Samples, 22050);                     /* half a second */
        frequency = AudioTestMeasureHz(g_Samples, 22050, AUDIO_OUTPUT_HZ);
        printf("        resampled pitch %.1f Hz, expected 388.4 Hz\n", frequency);
        CHECK(frequency > 380.0 && frequency < 397.0, "FM: pitch survives 49716 -> 44100 resampling");
        CHECK(AudioTestRms(g_Samples, 22050) > 10000, "FM: audible level after mixing");
    }

    /* T3: mixing DRIVES an SB transfer to its IRQ ---------------------------- */
    /* This is the bit that unblocks a game: nothing else pulls DMA data. */
    for (index = 0; index < 512; ++index)
        g_GuestMemory[0x40000 + index] = 0x80;                                      /* silence, 8-bit */

    AudioTestDmaProgram(0x40000, 512, 0);
    AudioTestWrite(BASE + 0xC, 0x40);
    AudioTestWrite(BASE + 0xC, 165);               /* ~11 kHz */
    g_IrqCount = 0;
    AudioTestWrite(BASE + 0xC, 0x14);
    AudioTestWrite(BASE + 0xC, 0xFF);
    AudioTestWrite(BASE + 0xC, 0x01);  /* 512 B */
    CHECK(VddSbIsActive(&g_Sb), "SB: transfer armed");
    CHECK(g_IrqCount == 0, "SB: no IRQ before the mixer runs");
    /* 512 source samples at ~11 kHz is ~46ms; mix a comfortable margin of it. */
    VddAudioMix(&g_Mixer, g_Samples, 4096);
    CHECK(g_IrqCount >= 1, "SB: mixing alone drives the block to completion IRQ  <-- THE TEST");
    CHECK(!VddSbIsActive(&g_Sb), "SB: single-cycle transfer finished");

    /* T4: SB audio actually reaches the output ------------------------------ */
    {
        long level;

        for (index = 0; index < 512; ++index)
            g_GuestMemory[0x41000 + index] = (BYTE)((index & 32) ? 0xE0 : 0x20);

        AudioTestDmaProgram(0x41000, 512, 1);
        AudioTestWrite(BASE + 0xC, 0x48);
        AudioTestWrite(BASE + 0xC, 0xFF);
        AudioTestWrite(BASE + 0xC, 0x01);
        AudioTestWrite(BASE + 0xC, 0x1C);                                /* auto-init */
        VddOplWriteRegister(&g_Opl, 0xB0, 0x00);                 /* silence the FM */
        VddAudioMix(&g_Mixer, g_Samples, 8192);
        level = AudioTestRms(g_Samples, 8192);
        printf("        SB-only rms=%ld\n", level);
        CHECK(level > 1000, "SB: sampled audio is present in the mix");
    }

    /* T5: an auto-init ring keeps raising IRQs while mixing continues -------- */
    g_IrqCount = 0;
    VddAudioMix(&g_Mixer, g_Samples, 16384);
    CHECK(g_IrqCount >= 2, "SB: auto-init keeps producing IRQs as the mixer runs");
    CHECK(VddSbIsActive(&g_Sb), "SB: auto-init still streaming");

    /* T6: rate changes are picked up mid-stream ----------------------------- */
    AudioTestWrite(BASE + 0xC, 0x41);
    AudioTestWrite(BASE + 0xC, 0x56);
    AudioTestWrite(BASE + 0xC, 0x22);   /* 22050 */
    VddAudioMix(&g_Mixer, g_Samples, 2048);
    CHECK(g_Mixer.SbResampler.SourceHz == 22050, "resampler: follows a mid-stream rate change");

    /* T7: a realistic mix has headroom; a pathological one clamps cleanly --- */
    /* Nine full-volume FM channels PLUS full-scale digital audio saturates a real
     * SB16 too, so asserting "never clips" there would be asserting something
     * false. What must hold is that a normal score stays clear of the rail, and
     * that overload clamps rather than wrapping (which would sound like a bang).
     */
    {
        INT clipped = 0;
        INT voice;
        VddOplWriteRegister(&g_Opl, 0xB0, 0x00);                 /* all notes off */

        for (voice = 0; voice < 9; ++voice)
            VddOplWriteRegister(&g_Opl, (BYTE)(0xB0+voice), 0x00);

        for (voice = 0; voice < 3; ++voice)                              /* three voices, */
        {
            INT voiceCarrier = VddOplOperatorIndex(voice,1);
            BYTE voiceCarrierRegister = (BYTE)(voiceCarrier + 2*(voiceCarrier/6));
            INT voiceModulator = VddOplOperatorIndex(voice,0);
            BYTE voiceModulatorRegister = (BYTE)(voiceModulator + 2*(voiceModulator/6));
            VddOplWriteRegister(&g_Opl, (BYTE)(0x40+voiceModulatorRegister), 0x3F);
            VddOplWriteRegister(&g_Opl, (BYTE)(0x20+voiceCarrierRegister), 0x21);
            VddOplWriteRegister(&g_Opl, (BYTE)(0x40+voiceCarrierRegister), 0x10);   /* moderate TL */
            VddOplWriteRegister(&g_Opl, (BYTE)(0x60+voiceCarrierRegister), 0xF0);
            VddOplWriteRegister(&g_Opl, (BYTE)(0x80+voiceCarrierRegister), 0x0F);
            VddOplWriteRegister(&g_Opl, (BYTE)(0xA0+voice), 0x40);
            VddOplWriteRegister(&g_Opl, (BYTE)(0xB0+voice), (BYTE)(0x20 | (4 << 2) | 1));
        }

        VddAudioMix(&g_Mixer, g_Samples, 8192);

        for (index = 0; index < 8192; ++index)
            if (g_Samples[index] == 32767 || g_Samples[index] == -32768)
                clipped++;

        printf("        realistic mix: %d of 8192 at the rail\n", clipped);
        CHECK(clipped == 0, "mix: a realistic score plus sampled audio has headroom");
        CHECK(AudioTestRms(g_Samples, 8192) > 1000, "mix: ...and is still clearly audible");

        /* now overload it deliberately and check it clamps, never wraps */
        for (voice = 0; voice < 9; ++voice)
        {
            INT voiceCarrier = VddOplOperatorIndex(voice,1);
            BYTE voiceCarrierRegister = (BYTE)(voiceCarrier + 2*(voiceCarrier/6));
            VddOplWriteRegister(&g_Opl, (BYTE)(0x20+voiceCarrierRegister), 0x21);
            VddOplWriteRegister(&g_Opl, (BYTE)(0x40+voiceCarrierRegister), 0x00);   /* full volume */
            VddOplWriteRegister(&g_Opl, (BYTE)(0x60+voiceCarrierRegister), 0xF0);
            VddOplWriteRegister(&g_Opl, (BYTE)(0x80+voiceCarrierRegister), 0x0F);
            VddOplWriteRegister(&g_Opl, (BYTE)(0xA0+voice), 0x40);
            VddOplWriteRegister(&g_Opl, (BYTE)(0xB0+voice), (BYTE)(0x20 | (5 << 2) | 1));
        }

        VddAudioMix(&g_Mixer, g_Samples, 4096);
        { INT wrapped = 0;

          for (index = 1; index < 4096; ++index)
              if ((g_Samples[index-1] > 30000 && g_Samples[index] < -30000) || (g_Samples[index-1] < -30000 && g_Samples[index] > 30000))
                  wrapped++;

          /* a genuine waveform can cross fast; a WRAP shows up as many such jumps */
          printf("        overload: %d fast rail-to-rail transitions\n", wrapped);
          CHECK(wrapped < 4096 / 20, "mix: overload clamps rather than wrapping"); }
    }

    /* THE TRANSPORT MUST NOT EAT SAMPLES IT DOES NOT PLAY:
     * VddSbRender() pulls out of the guest's DMA ring, so any sample the mixer asks
     * for and then discards is data the game wrote and nobody hears. AudioResamplerNeed() used
     * to ask for two extra every chunk "for the pair being interpolated between",
     * which at Doom's rate is 172 dropped PCM samples a second: the read pointer
     * walks away from the guest's write pointer and you hear a click at chunk rate.
     * Drive a whole number of chunks at an exact 4:1 ratio and check the ring
     * advanced by the arithmetic amount and no more.
     */
    {
        UINT32 before;
        UINT32 after;
        UINT32 expected;
        UINT32 chunks = 8;

        for (index = 0; index < 4096; ++index)
            g_GuestMemory[0x50000 + index] = 0x80;

        AudioTestDmaProgram(0x50000, 4096, 1);                       /* auto-init ring */
        AudioTestWrite(BASE + 0xC, 0x41);
        AudioTestWrite(BASE + 0xC, 0x2B);
        AudioTestWrite(BASE + 0xC, 0x11);  /* 11025 Hz */
        AudioTestWrite(BASE + 0xC, 0xC6);
        AudioTestWrite(BASE + 0xC, 0x00);          /* 8-bit auto, mono */
        AudioTestWrite(BASE + 0xC, 0xFF);
        AudioTestWrite(BASE + 0xC, 0x0F);          /* 4096-byte block */
        VddAudioMix(&g_Mixer, g_Samples, 512);                       /* prime the pair */
        before = g_Sb.BlockRemaining;

        for (index = 0; index < chunks; ++index)
            VddAudioMix(&g_Mixer, g_Samples, 512);

        after = g_Sb.BlockRemaining;
        expected = chunks * 512u * 11025u / AUDIO_OUTPUT_HZ;        /* exactly 1:4 */
        printf("        ring consumed %u over %u chunks, arithmetic says %u\n",
               before - after, chunks, expected);
        CHECK(before - after == expected,
              "resampler pulls exactly what it plays (no DMA samples discarded)");
    }

    /* THE PC SPEAKER, WHICH MADE NO SOUND AT ALL UNTIL NOW:
     * vdd_speaker.c has modelled port 0x61 and reported the tone since M3, and
     * nothing ever turned that into a sample -- so every beep a guest made went
     * nowhere while the score line read "SB16 PCM, OPL2/3 FM, MPU-401, speaker".
     * The gate is TWO bits, and a program that sets only one of them is clicking
     * the cone rather than sounding a tone, so that distinction is measured here
     * rather than assumed.
     */
    {
        PIT_STATE pit;
        NTVDD_DEVICE pitDevice;
        SPEAKER_STATE speaker;
        NTVDD_DEVICE speakerDevice;
        INT16 speakerSamples[4410];
        double frequency;
        long full;
        long half;
        UINT32 value;

        memset(&pit, 0, sizeof pit);
        memset(&speaker, 0, sizeof speaker);
        speaker.Pit = &pit;
        pitDevice = VddPitDevice(&pit);
        speakerDevice = VddSpeakerDevice(&speaker);
        CHECK(VddBusAdd(&g_Bus, &pitDevice) == 0, "speaker: PIT joined the mixer's bus");
        CHECK(VddBusAdd(&g_Bus, &speakerDevice) == 0, "speaker: and the speaker VDD did too");

        /* Programmed THROUGH the chip, not by poking the struct, so what this
         * measures is what a guest that writes 0x43/0x42 actually hears.
         */
        value = 0xB6;
        VddBusIo(&g_Bus, 0x43, 1, 0, &value);
        value = 1193 & 0xFF;
        VddBusIo(&g_Bus, 0x42, 1, 0, &value);
        value = 1193 >> 8;
        VddBusIo(&g_Bus, 0x42, 1, 0, &value);
        CHECK(VddPitCounter2Hz(&pit) == PIT_INPUT_HZ / 1193,
              "speaker: PIT channel 2 divisor 1193 gives ~1000 Hz");

        g_Mixer.Opl = NULL;
        g_Mixer.Sb = NULL;              /* the speaker alone in the mix */
        VddAudioSetSpeaker(&g_Mixer, &speaker, 1);
        value = 0x03;
        VddBusIo(&g_Bus, 0x61, 1, 0, &value); /* gate + data = sounding */
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 4410);
        frequency = AudioTestMeasureHz(speakerSamples, 4410, AUDIO_OUTPUT_HZ);
        printf("        speaker tone measured %.0f Hz (the chip is at %u)\n",
               frequency, (UINT)VddPitCounter2Hz(&pit));
        CHECK(frequency > 960 && frequency < 1040, "speaker: an active gate produces the PIT's tone");
        CHECK(AudioTestRms(speakerSamples, 4410) > 1000, "speaker: and it is actually audible");

        value = 0x01;
        VddBusIo(&g_Bus, 0x61, 1, 0, &value); /* gate only, no data enable */
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 512);
        CHECK(AudioTestRms(speakerSamples, 512) == 0, "speaker: the gate bit ALONE is silent (both bits gate)");

        value = 0x03;
        VddBusIo(&g_Bus, 0x61, 1, 0, &value);
        VddAudioSetSpeaker(&g_Mixer, &speaker, 0);       /* fitted, switched off */
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 512);
        CHECK(AudioTestRms(speakerSamples, 512) == 0, "speaker: the setting silences it without unfitting it");

        /* [CAUTION]: A "tone" above the output rate's Nyquist point is not a tone, it is
         * alias noise, so it is refused rather than synthesised.
         */
        VddAudioSetSpeaker(&g_Mixer, &speaker, 1);
        value = 0xB6;
        VddBusIo(&g_Bus, 0x43, 1, 0, &value);
        value = 0x01;
        VddBusIo(&g_Bus, 0x42, 1, 0, &value);
        value = 0x00;
        VddBusIo(&g_Bus, 0x42, 1, 0, &value); /* divisor 1 -> 1.19 MHz */
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 512);
        CHECK(AudioTestRms(speakerSamples, 512) == 0, "speaker: a tone past 20 kHz is refused, not aliased");

        /* THE MASTER ATTENUATOR SITS AFTER THE SUM:
         * That is where a volume control lives on a real machine: the guest's own
         * mixer registers still do their work underneath it. Measured on the
         * SPEAKER because its level is exactly known -- a square wave at a fixed
         * amplitude -- so "half" is arithmetic, not a judgement about loudness.
         */
        value = 0xB6;
        VddBusIo(&g_Bus, 0x43, 1, 0, &value);
        value = 1193 & 0xFF;
        VddBusIo(&g_Bus, 0x42, 1, 0, &value);
        value = 1193 >> 8;
        VddBusIo(&g_Bus, 0x42, 1, 0, &value);

        VddAudioSetMaster(&g_Mixer, 100, 0);
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 512);
        full = AudioTestRms(speakerSamples, 512);
        VddAudioSetMaster(&g_Mixer, 50, 0);
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 512);
        half = AudioTestRms(speakerSamples, 512);
        printf("        mean square at 100%% = %ld, at 50%% = %ld (power, so ~4x)\n",
               full, half);
        CHECK(half * 3 < full && half * 5 > full,
              "master volume 50 halves the amplitude (a quarter of the power)");

        VddAudioSetMaster(&g_Mixer, 100, 1);
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 512);
        CHECK(AudioTestRms(speakerSamples, 512) == 0, "mute outputs silence");
        VddAudioSetMaster(&g_Mixer, 100, 0);
        memset(speakerSamples, 0, sizeof speakerSamples);
        VddAudioMix(&g_Mixer, speakerSamples, 512);
        CHECK(AudioTestRms(speakerSamples, 512) > 0, "...and unmuting restores the volume it kept");

        VddAudioSetMaster(&g_Mixer, 4000, 0);
        CHECK(g_Mixer.Master == 100, "a volume past 100 is clamped, never wrapped into a gain");
        VddAudioSetSpeaker(&g_Mixer, NULL, 0);
    }

    /* VddAudioInitialize MUST NOT LEAVE THE MASTER AT ZERO:
     * It zeroes the whole struct, which is right for every other field and would
     * be SILENCE for this one -- the exact shape of bug that ships as "no sound on
     * a clean machine" and is invisible to any test that sets the volume first.
     */
    {
        AUDIO_STATE fresh;
        VddAudioInitialize(&fresh, NULL, NULL, 0);
        CHECK(fresh.Master == 100, "a freshly initialised mixer is at full volume, not zero");
        CHECK(fresh.IsMuted == 0,    "...and unmuted");
        CHECK(fresh.OutputHz == AUDIO_OUTPUT_HZ, "...and out_hz 0 means the default rate");
        CHECK(fresh.SpeakerLevel == 0, "...and the speaker is unfitted until it is fitted");
        VddAudioInitialize(&fresh, NULL, NULL, 22050);
        CHECK(fresh.OutputHz == 22050, "a requested output rate is honoured");
    }

    /* ---- #189: AN SB16 STEREO TRANSFER KEEPS ITS TWO CHANNELS. 8-bit unsigned pairs,
     * left at FFh and right at 00h: the stereo mix must put + on the left and - on the
     * right, and the mono mix -- which always averaged them -- must still average.
     */
    {   static INT16 stereo[2 * 1024], mono[1024];
        g_Mixer.Opl = NULL;
        g_Mixer.Sb = &g_Sb;
        VddAudioSetSpeaker(&g_Mixer, NULL, 0);
        VddAudioSetMaster(&g_Mixer, 100, 0);

        for (index = 0; index < 4096; index += 2)
        {
            g_GuestMemory[0x60000 + index] = 0xFF;
            g_GuestMemory[0x60000 + index + 1] = 0x00;
        }

        AudioTestDmaProgram(0x60000, 4096, 1);
        AudioTestWrite(BASE + 0xC, 0x41);
        AudioTestWrite(BASE + 0xC, 22050 >> 8);
        AudioTestWrite(BASE + 0xC, 22050 & 0xFF);
        AudioTestWrite(BASE + 0xC, 0xC6);
        AudioTestWrite(BASE + 0xC, 0x20);           /* 8-bit auto, STEREO */
        AudioTestWrite(BASE + 0xC, 0xFF);
        AudioTestWrite(BASE + 0xC, 0x07);           /* 2048 units */
        VddAudioMixStereo(&g_Mixer, stereo, 1024);
        printf("        SB16 stereo frame 500: L=%d R=%d\n", stereo[1000], stereo[1001]);
        CHECK(stereo[1000] > 10000 && stereo[1001] < -10000,
              "SB16 stereo: left and right come out on their own channels");
        VddAudioMix(&g_Mixer, mono, 1024);
        CHECK(mono[500] > -200 && mono[500] < 200, "SB16 stereo, mono fold: still the average");
        AudioTestWrite(BASE + 0xC, 0xDA);                                  /* exit auto-init 8-bit */
        VddAudioMix(&g_Mixer, g_Samples, 4096);
    }

    /* ---- #189: AN SB PRO STEREO TRANSFER. Mixer 0Eh bit 1 selects stereo for the DSP
     * 1.x-3.x commands, and the time constant counts BOTH channels: TC 233 is ~43.5 kHz
     * of bytes = ~21.7 kHz of L/R frames. High-speed auto-init (0x90), 4096-byte blocks:
     * a second of output is ~10.6 blocks. Treating the byte rate as the frame rate would
     * race through ~21 of them and play an octave high.
     */
    {   static INT16 stereo[2 * 1024];
        INT blocks;
        g_Mixer.Opl = NULL;
        g_Mixer.Sb = &g_Sb;

        for (index = 0; index < 8192; index += 2)
        {
            g_GuestMemory[0x70000 + index] = 0xFF;
            g_GuestMemory[0x70000 + index + 1] = 0x00;
        }

        AudioTestWrite(BASE + 4, 0x0E);
        AudioTestWrite(BASE + 5, 0x02);              /* mixer: stereo on */
        AudioTestDmaProgram(0x70000, 8192, 1);
        AudioTestWrite(BASE + 0xC, 0x40);
        AudioTestWrite(BASE + 0xC, 233);           /* TC: 2 x 21.7 kHz */
        AudioTestWrite(BASE + 0xC, 0x48);
        AudioTestWrite(BASE + 0xC, 0xFF);
        AudioTestWrite(BASE + 0xC, 0x0F);   /* 4096 B */
        g_IrqCount = 0;
        AudioTestWrite(BASE + 0xC, 0x90);                                /* high-speed auto-init */
        VddAudioMixStereo(&g_Mixer, stereo, 1024);
        printf("        SB Pro stereo frame 500: L=%d R=%d\n", stereo[1000], stereo[1001]);
        CHECK(stereo[1000] > 10000 && stereo[1001] < -10000,
              "SB Pro stereo (mixer 0Eh bit 1): left and right on their own channels");

        for (index = 0; index < 43; ++index)
            VddAudioMixStereo(&g_Mixer, stereo, 1024);                                    /* ~1 s in all */

        blocks = g_IrqCount;
        printf("        SB Pro stereo: %d block IRQs in ~1 s (frame rate, ~10.6 expected)\n", blocks);
        CHECK(blocks >= 9 && blocks <= 12,
              "SB Pro stereo: the time constant counts both channels (frames at half the byte rate)");
        AudioTestWrite(BASE + 0xC, 0xDA);                                /* leave auto-init */
        AudioTestWrite(BASE + 4, 0x0E);
        AudioTestWrite(BASE + 5, 0x00);              /* mixer: stereo off */
        VddAudioMix(&g_Mixer, g_Samples, 8192);
    }

    /* -- #232: THE OPL THROUGH THE STEREO MIXER. Its own chip and mixer, so nothing
     * above leaks in. Two claims: an OPL2 still mixes to EXACTLY the samples it did
     * when the mixer took it as a mono source (a golden taken from 42a9029, the
     * build before the OPL3 -- the same register sequence as opl_synth_test's), and
     * an OPL3 voice routed left arrives on the left and nowhere else.
     */
    {   static OPL_STATE opl3;
    static AUDIO_STATE opl3Mixer;
        static INT16 opl3Samples[2 * 8192];
        UINT32 hash = 2166136261u;
        INT channel;
        INT round;
        #define M3_EAT(frames) do { INT frameCount = (frames) * 44100 / 49716, sampleIndex;                        \
            VddAudioMixStereo(&opl3Mixer, opl3Samples, (UINT32)frameCount);                                 \
            for (sampleIndex = 0; sampleIndex < 2 * frameCount; ++sampleIndex) {                                         \
                hash ^= (BYTE)opl3Samples[sampleIndex]; hash *= 16777619u;                                \
                hash ^= (BYTE)((WORD)opl3Samples[sampleIndex] >> 8); hash *= 16777619u; } } while (0)
        #define M3_W(registerIndex, value) VddOplWriteRegister(&opl3, (BYTE)(registerIndex), (BYTE)(value))
        memset(&opl3, 0, sizeof opl3);
        VddOplReset(&opl3);
        VddAudioInitialize(&opl3Mixer, &opl3, NULL, 44100);
        M3_W(0x01, 0x20);
        M3_W(0xBD, 0xC0);

        for (channel = 0; channel < 9; ++channel)
        {
            INT modulatorIndex = VddOplOperatorIndex(channel, 0);
            INT carrierIndex = VddOplOperatorIndex(channel, 1);
            UINT modulatorOffset = (UINT)(modulatorIndex + 2 * (modulatorIndex / 6));
            UINT carrierOffset = (UINT)(carrierIndex + 2 * (carrierIndex / 6));
            M3_W(0x20 + modulatorOffset, 0x21 | ((channel & 1) << 7) | ((channel & 2) << 5) | (channel & 4 ? 0x10 : 0) | (channel % 5));
            M3_W(0x20 + carrierOffset, 0x21 | ((channel & 2) << 6));
            M3_W(0x40 + modulatorOffset, (UINT)(0x10 + channel * 3) | ((channel % 4) << 6));
            M3_W(0x40 + carrierOffset, (UINT)(channel * 2) | (((channel + 1) % 4) << 6));
            M3_W(0x60 + modulatorOffset, 0xF0 - (UINT)channel * 0x11 + 3);
            M3_W(0x60 + carrierOffset, 0xD2 + (UINT)channel);
            M3_W(0x80 + modulatorOffset, 0x35 + (UINT)channel * 0x10);
            M3_W(0x80 + carrierOffset, 0x24 + (UINT)channel);
            M3_W(0xE0 + modulatorOffset, (UINT)channel & 3);
            M3_W(0xE0 + carrierOffset, (UINT)(channel + 1) & 3);
            M3_W(0xC0 + channel, (UINT)((channel * 3) & 0x0E) | (UINT)(channel & 1));
            M3_W(0xA0 + channel, 0x40 + (UINT)channel * 23);
            M3_W(0xB0 + channel, 0x20 | (UINT)((2 + channel % 5) << 2) | (UINT)(channel & 3));
            M3_EAT(700);
        }

        M3_EAT(6000);

        for (channel = 0; channel < 9; channel += 2)
        {
            M3_W(0xB0 + channel, opl3.Registers[0xB0 + channel] & ~0x20);
            M3_EAT(900);
        }

        /* #139 made the hi-hat, cymbal and snare sound and OR'd each drum bit with
         * its channel's key bit, so from the rhythm write on the hash moved -- see
         * opl_synth_test's golden for the why. Everything BEFORE it must not.
         */
        {   UINT32 melodicHash = hash;
            printf("        OPL2 through the mixer, before rhythm mode: fnv=0x%08X (golden 0x%08X)\n",
                   melodicHash, 0x44CCD995u);
            CHECK(melodicHash == 0x44CCD995u, "mix: an OPL2 mixes bit-identically to the pre-OPL3 build up to rhythm mode"); }
        M3_W(0xBD, 0xE0 | 0x10 | 0x04);
        M3_EAT(4000);
        M3_W(0xBD, 0xE0 | 0x0B);
        M3_EAT(3000);
        M3_W(0xBD, 0x00);

        for (round = 0; round < 8; ++round)
            M3_EAT(4000);

        printf("        OPL2 through the mixer: fnv=0x%08X (golden 0x%08X; 0x6CA12225 before #139)\n", hash, 0x3B2BE715u);
        CHECK(hash == 0x3B2BE715u, "mix: an OPL2 mixes bit-identically to the #139 build");
        #undef M3_EAT
        #undef M3_W

        /* OPL3, NEW set, one voice on channel 0 routed LEFT only (C0 = 0x11) */
        memset(&opl3, 0, sizeof opl3);
        opl3.IsOpl3 = 1;
        VddOplReset(&opl3);
        VddAudioInitialize(&opl3Mixer, &opl3, NULL, 44100);
        VddOplWriteRegister(&opl3, 0x105, 0x01);
        VddOplWriteRegister(&opl3, 0x20, 0x21);
        VddOplWriteRegister(&opl3, 0x40, 0x3F);
        VddOplWriteRegister(&opl3, 0x23, 0x21);
        VddOplWriteRegister(&opl3, 0x43, 0x00);
        VddOplWriteRegister(&opl3, 0x63, 0xF0);
        VddOplWriteRegister(&opl3, 0x83, 0x0F);
        VddOplWriteRegister(&opl3, 0xC0, 0x11);
        VddOplWriteRegister(&opl3, 0xA0, 0x00);
        VddOplWriteRegister(&opl3, 0xB0, (BYTE)(0x20 | (4 << 2) | 0x02));
        VddAudioMixStereo(&opl3Mixer, opl3Samples, 4096);
        { long long leftEnergy = 0, rightEnergy = 0;
        INT frame;

          for (frame = 0; frame < 4096; ++frame)
          {
              leftEnergy += (long long)opl3Samples[2*frame] * opl3Samples[2*frame];
              rightEnergy += (long long)opl3Samples[2*frame+1] * opl3Samples[2*frame+1];
          }

          printf("        OPL3 left-only voice through the mixer: L energy %lld, R energy %lld\n", leftEnergy, rightEnergy);
          CHECK(leftEnergy > 0 && rightEnergy == 0, "mix: an OPL3 voice routed left (C0 bit 4) is heard on the left ONLY"); }
    }

    printf("-- %d checks, %d failures --\n", g_Total, g_Failures);
    return g_Failures ? 1 : 0;
}

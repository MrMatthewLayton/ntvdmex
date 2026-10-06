/* vdd_audio.c -- see vdd_audio.h.  Resample the OPL and the Sound Blaster onto a
 * common output clock and sum them.  Pure C, no <windows.h>. */
#include "vdd_audio.h"

/* The SB16 mixer registers the sources are scaled by (see AudioMixGain). */
#define AUDIO_SB_MIXER_VOICE        0x04
#define AUDIO_SB_MIXER_MASTER       0x22
#define AUDIO_SB_MIXER_FM           0x26
#define AUDIO_SB_LEVEL_SHIFT        4       /* the left level, bits 7-4                 */
#define AUDIO_SB_LEVEL_MASK         0x0F
#define AUDIO_SB_LEVEL_POWER_UP     12      /* 0xCC                                     */
#define AUDIO_SB_LEVEL_SQUARED      225u    /* 15 x 15                                  */
/* Gains are 0..256 (Q8). */
#define AUDIO_GAIN_UNITY            256
#define AUDIO_GAIN_SCALE            256u
#define AUDIO_GAIN_SHIFT            8
#define AUDIO_MASTER_MAX            100     /* percent                                  */
#define AUDIO_PERCENT               100u
/* The resampler's 16.16 position. */
#define AUDIO_FRACTION_BITS         16
#define AUDIO_FRACTION_ONE          0x10000u
#define AUDIO_INTERPOLATION_SHIFT   8       /* 8 bits of the fraction weight a step     */
#define AUDIO_PRIME_FRAMES          2u      /* priming loads the first pair             */
#define AUDIO_SPEAKER_HALF_CYCLE    0x8000u
#define AUDIO_SAMPLE_MAX            32767
#define AUDIO_SAMPLE_MIN            (-32768)
#define AUDIO_MONO_FOLD_DIVISOR     2       /* (L + R) / 2                              */

/* Source gain from the SB16 mixer registers, as a 0..256 fraction. A real card
   does not sum FM and sampled audio at unity -- the mixer chip attenuates both,
   which is exactly what stops a busy score from sitting on the clip rail. We
   honour the same registers (master 0x22, voice 0x04, FM 0x26), so a game's own
   volume control works, and their power-up value (0xCC) gives sane headroom. */
static INT32 AudioMixGain(PCSB_STATE soundBlaster, BYTE mixerRegister)
{
    UINT32 masterLevel, sourceLevel;
    if (!soundBlaster) return AUDIO_GAIN_UNITY;
    masterLevel = (soundBlaster->Mixer[AUDIO_SB_MIXER_MASTER] >> AUDIO_SB_LEVEL_SHIFT) & AUDIO_SB_LEVEL_MASK;
    sourceLevel    = (soundBlaster->Mixer[mixerRegister]   >> AUDIO_SB_LEVEL_SHIFT) & AUDIO_SB_LEVEL_MASK;
    if (!soundBlaster->Mixer[AUDIO_SB_MIXER_MASTER]) masterLevel = AUDIO_SB_LEVEL_POWER_UP;            /* never programmed: power-up 0xCC */
    if (!soundBlaster->Mixer[mixerRegister])  sourceLevel    = AUDIO_SB_LEVEL_POWER_UP;
    return (INT32)((masterLevel * sourceLevel * AUDIO_GAIN_SCALE) / AUDIO_SB_LEVEL_SQUARED);   /* (m/15)*(s/15) in 0..256   */
}

static VOID AudioResamplerSetup(PAUDIO_RESAMPLER resampler, UINT32 sourceHz, UINT32 outputHz)
{
    if (!sourceHz) sourceHz = outputHz;
    if (resampler->SourceHz != sourceHz) {      /* rate changed mid-stream        */
        resampler->SourceHz = sourceHz;
        resampler->Step   = (UINT32)(((UINT64)sourceHz << AUDIO_FRACTION_BITS) / (outputHz ? outputHz : 1));
        if (!resampler->Step) resampler->Step = 1;
    }
}

/* ── PULL EXACTLY WHAT WILL BE CONSUMED, AND NOT ONE SAMPLE MORE. ───────────────────
     This used to ask for two extra samples every chunk, "the pair being interpolated
     between". For a synthesised source that is merely wasteful; for the SOUND BLASTER
     it is data loss. VddSbRender() is the TRANSPORT -- every sample it is asked for
     is pulled out of the guest's DMA ring and thrown away if the resampler does not
     use it. Two per chunk, 86 chunks a second at Doom's 11025 Hz stereo, is 172
     dropped PCM samples a second: the read pointer walks away from the guest's write
     pointer at 1.6%, and what you hear is a soft click at chunk rate over everything.
     The count is exact and provable: Previous/Current persist across calls, so the only
     samples consumed are the ones a phase wrap loads, and there are exactly
     floor((frac + step*frames) / 0x10000) wraps. Priming loads the first pair, once. */
static UINT32 AudioResamplerNeed(PCAUDIO_RESAMPLER resampler, UINT32 frames)
{
    UINT64 span = (UINT64)resampler->Fraction + (UINT64)resampler->Step * frames;
    UINT32 count = (UINT32)(span >> AUDIO_FRACTION_BITS) + (resampler->IsPrimed ? 0u : AUDIO_PRIME_FRAMES);
    return count > AUDIO_SOURCE_MAX ? AUDIO_SOURCE_MAX : count;
}

/* (The mono walk, rs_step, went with #232: the OPL was its last caller, and every
   source is now stereo -- AudioResamplerStepStereo below, whose left channel is that walk.) */

VOID VddAudioInitialize(PAUDIO_STATE state, POPL_STATE opl, PSB_STATE soundBlaster, UINT32 outputHz)
{
    UINT byteIndex; BYTE *bytes = (BYTE *)state;
    for (byteIndex = 0; byteIndex < sizeof(*state); ++byteIndex) bytes[byteIndex] = 0;
    state->Opl = opl;
    state->Sb  = soundBlaster;
    state->OutputHz = outputHz ? outputHz : AUDIO_OUTPUT_HZ;
    state->Master = AUDIO_MASTER_MAX;              /* the struct is zeroed above: 0 would be silence */
}

VOID VddAudioSetGus(PAUDIO_STATE state, PGUS_STATE gus) { state->Gus = gus; }
VOID VddAudioSetEmu8k(PAUDIO_STATE state, PEMU8K_STATE emu8k) { state->Emu8k = emu8k; }

VOID VddAudioSetSpeaker(PAUDIO_STATE state, PCSPEAKER_STATE speaker, INT isEnabled)
{
    state->Speaker = speaker;
    state->SpeakerLevel = isEnabled ? AUDIO_SPEAKER_LEVEL : 0;
}

VOID VddAudioSetMaster(PAUDIO_STATE state, UINT32 percent, INT isMuted)
{
    state->Master = percent > AUDIO_MASTER_MAX ? AUDIO_MASTER_MAX : percent;
    state->IsMuted  = isMuted ? 1 : 0;
}

/* ── #189: STEREO. A source that has two channels now keeps them: the SB's stereo
     transfers (they were averaged) and the GUS's per-voice pan (voices were summed).
     The OPL (#232: an OPL3 with NEW set routes each channel left/right; otherwise one
     output, in the middle) and the PC speaker (middle). Output
     is interleaved L/R, 2*frames samples. For a mono source L = R and both equal what
     the mono mixer produced, which is why VddAudioMix below can simply fold this. */
static INT16 AudioClip(INT32 value) { return (INT16)(value > AUDIO_SAMPLE_MAX ? AUDIO_SAMPLE_MAX : (value < AUDIO_SAMPLE_MIN ? AUDIO_SAMPLE_MIN : value)); }

/* One output frame from an INTERLEAVED stereo source, consuming source frames as
   needed; the right channel's pair is carried beside the left (Previous/Current for L,
   PreviousRight/CurrentRight). `*index` walks the source buffer in frames and never runs past `count`,
   because AudioResamplerNeed() sized the buffer for exactly this walk. */
static VOID AudioResamplerStepStereo(PAUDIO_RESAMPLER resampler, const INT16 *source, UINT32 count, UINT32 *index,
                       INT32 *outputLeft, INT32 *outputRight)
{
    if (!resampler->IsPrimed) {
        if (*index < count) { resampler->Previous = source[AUDIO_STEREO_CHANNELS * *index]; resampler->PreviousRight = source[AUDIO_STEREO_CHANNELS * *index + 1]; ++*index; }
        else          { resampler->Previous = resampler->PreviousRight = 0; }
        if (*index < count) { resampler->Current = source[AUDIO_STEREO_CHANNELS * *index]; resampler->CurrentRight = source[AUDIO_STEREO_CHANNELS * *index + 1]; ++*index; }
        else          { resampler->Current = resampler->Previous; resampler->CurrentRight = resampler->PreviousRight; }
        resampler->IsPrimed = 1;
        resampler->Fraction = 0;
    }
    *outputLeft  = resampler->Previous   + (((resampler->Current   - resampler->Previous)   * (INT32)(resampler->Fraction >> AUDIO_INTERPOLATION_SHIFT)) >> AUDIO_INTERPOLATION_SHIFT);
    *outputRight = resampler->PreviousRight + (((resampler->CurrentRight - resampler->PreviousRight) * (INT32)(resampler->Fraction >> AUDIO_INTERPOLATION_SHIFT)) >> AUDIO_INTERPOLATION_SHIFT);
    resampler->Fraction += resampler->Step;
    while (resampler->Fraction >= AUDIO_FRACTION_ONE) {
        resampler->Fraction -= AUDIO_FRACTION_ONE;
        resampler->Previous = resampler->Current; resampler->PreviousRight = resampler->CurrentRight;
        if (*index < count) { resampler->Current = source[AUDIO_STEREO_CHANNELS * *index]; resampler->CurrentRight = source[AUDIO_STEREO_CHANNELS * *index + 1]; ++*index; }
    }
}

VOID VddAudioMixStereo(PAUDIO_STATE state, INT16 *output, UINT32 frames)
{
    UINT32 done = 0;

    while (done < frames) {
        UINT32 count = frames - done;
        UINT32 frameIndex, needed, index;
        INT16 *chunk;
        if (count > AUDIO_CHUNK) count = AUDIO_CHUNK;
        chunk = output + AUDIO_STEREO_CHANNELS * done;

        for (frameIndex = 0; frameIndex < AUDIO_STEREO_CHANNELS * count; ++frameIndex) chunk[frameIndex] = 0;

        /* --- FM: the OPL's own L/R (#232) ---------------------------------- */
        /* An OPL2, or an OPL3 before NEW, renders the same signal to both sides,
           and on L == R this walk is the old mono one exactly -- so an OPL2 mixes
           to the same samples it did as a mono source (audio_test's golden). */
        if (state->Opl) {
            INT32 gain = AudioMixGain(state->Sb, AUDIO_SB_MIXER_FM);
            AudioResamplerSetup(&state->OplResampler, OPL_NATIVE_HZ, state->OutputHz);
            needed = AudioResamplerNeed(&state->OplResampler, count);
            VddOplRenderStereo(state->Opl, state->Scratch, needed);
            index = 0;
            for (frameIndex = 0; frameIndex < count; ++frameIndex) {
                INT32 left, right;
                AudioResamplerStepStereo(&state->OplResampler, state->Scratch, needed, &index, &left, &right);
                chunk[AUDIO_STEREO_CHANNELS*frameIndex]   = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex]   + ((left * gain) >> AUDIO_GAIN_SHIFT));
                chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] + ((right * gain) >> AUDIO_GAIN_SHIFT));
            }
        }

        /* --- sampled audio: the SB's own L/R ------------------------------ */
        /* Called even while idle: this is what walks the DMA buffer and raises
           the block-completion IRQ the game is waiting for. */
        if (state->Sb) {
            INT32 gain = AudioMixGain(state->Sb, AUDIO_SB_MIXER_VOICE);
            AudioResamplerSetup(&state->SbResampler, VddSbFrameHz(state->Sb), state->OutputHz);
            needed = AudioResamplerNeed(&state->SbResampler, count);
            VddSbRenderStereo(state->Sb, state->Scratch, needed);
            index = 0;
            for (frameIndex = 0; frameIndex < count; ++frameIndex) {
                INT32 left, right;
                AudioResamplerStepStereo(&state->SbResampler, state->Scratch, needed, &index, &left, &right);
                chunk[AUDIO_STEREO_CHANNELS*frameIndex]   = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex]   + ((left * gain) >> AUDIO_GAIN_SHIFT));
                chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] + ((right * gain) >> AUDIO_GAIN_SHIFT));
            }
        }

        /* --- the GUS, panned per voice (see vdd_gus.c) -------------------- */
        if (state->Gus) {
            AudioResamplerSetup(&state->GusResampler, VddGusRateHz(state->Gus), state->OutputHz);
            needed = AudioResamplerNeed(&state->GusResampler, count);
            VddGusRenderStereo(state->Gus, state->Scratch, needed);
            index = 0;
            for (frameIndex = 0; frameIndex < count; ++frameIndex) {
                INT32 left, right;
                AudioResamplerStepStereo(&state->GusResampler, state->Scratch, needed, &index, &left, &right);
                chunk[AUDIO_STEREO_CHANNELS*frameIndex]   = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex]   + left);
                chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] + right);
            }
        }

        /* --- the AWE32's EMU8000, panned per channel (see vdd_emu8k.c) ----- */
        /* At its own fixed 44.1 kHz. Summed at unity, as the GUS is: the route through
           the CT1745 mixer on a real AWE32 is not modelled (docs/inventory/emu8k.md). */
        if (state->Emu8k) {
            AudioResamplerSetup(&state->Emu8kResampler, VddEmu8kRateHz(state->Emu8k), state->OutputHz);
            needed = AudioResamplerNeed(&state->Emu8kResampler, count);
            VddEmu8kRenderStereo(state->Emu8k, state->Scratch, needed);
            index = 0;
            for (frameIndex = 0; frameIndex < count; ++frameIndex) {
                INT32 left, right;
                AudioResamplerStepStereo(&state->Emu8kResampler, state->Scratch, needed, &index, &left, &right);
                chunk[AUDIO_STEREO_CHANNELS*frameIndex]   = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex]   + left);
                chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] + right);
            }
        }

        /* --- PC speaker (mono: both channels) ------------------------------ */
        /* Gated by port 0x61 bits 0+1 -- both, which is why a program that only
           sets the data bit to click the cone makes no tone here either. */
        if (state->Speaker && state->SpeakerLevel && VddSpeakerIsActive(state->Speaker)) {
            UINT32 speakerHz = VddSpeakerHz(state->Speaker);
            state->SpeakerGated += count;
            state->SpeakerHz = speakerHz;
            if (speakerHz >= AUDIO_SPEAKER_HZ_MIN && speakerHz <= AUDIO_SPEAKER_HZ_MAX) {
                UINT32 step = (UINT32)(((UINT64)speakerHz << AUDIO_FRACTION_BITS) / state->OutputHz);
                state->SpeakerFrames += count;
                for (frameIndex = 0; frameIndex < count; ++frameIndex) {
                    INT32 value = (state->SpeakerPhase & AUDIO_SPEAKER_HALF_CYCLE) ? state->SpeakerLevel : -state->SpeakerLevel;
                    chunk[AUDIO_STEREO_CHANNELS*frameIndex]   = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex]   + value);
                    chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] = AudioClip(chunk[AUDIO_STEREO_CHANNELS*frameIndex+1] + value);
                    state->SpeakerPhase = (WORD)(state->SpeakerPhase + step);
                }
            }
        }

        /* --- master attenuator -------------------------------------------- */
        if (state->IsMuted) {
            for (frameIndex = 0; frameIndex < AUDIO_STEREO_CHANNELS * count; ++frameIndex) chunk[frameIndex] = 0;
        } else if (state->Master < AUDIO_MASTER_MAX) {
            INT32 gain = (INT32)((state->Master * AUDIO_GAIN_SCALE) / AUDIO_PERCENT); /* 0..256 */
            for (frameIndex = 0; frameIndex < AUDIO_STEREO_CHANNELS * count; ++frameIndex) chunk[frameIndex] = (INT16)((chunk[frameIndex] * gain) >> AUDIO_GAIN_SHIFT);
        }

        state->FramesMixed += count;
        done += count;
    }
}

/* The mono mixer, as it always was to its callers: the stereo mix folded. For every
   mono source L = R, so (L + R) / 2 is exactly the old sample -- audio_test's checks
   are unchanged. (The SB's stereo was always averaged here; a GUS voice panned hard
   to one side now folds to half its old level -- nothing but the tests calls this.) */
VOID VddAudioMix(PAUDIO_STATE state, INT16 *output, UINT32 frames)
{
    INT16 stereo[AUDIO_STEREO_CHANNELS * AUDIO_CHUNK];
    UINT32 done = 0, frameIndex;
    while (done < frames) {
        UINT32 count = frames - done;
        if (count > AUDIO_CHUNK) count = AUDIO_CHUNK;
        VddAudioMixStereo(state, stereo, count);
        for (frameIndex = 0; frameIndex < count; ++frameIndex) output[done + frameIndex] = (INT16)(((INT32)stereo[AUDIO_STEREO_CHANNELS*frameIndex] + stereo[AUDIO_STEREO_CHANNELS*frameIndex+1]) / AUDIO_MONO_FOLD_DIVISOR);
        done += count;
    }
}

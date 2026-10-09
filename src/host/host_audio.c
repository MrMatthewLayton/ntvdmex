/* host_audio.c -- the host side of the sound devices: OPL tracing and pumping, the audio fill,
 *   MIDI sinks and the GUS report.
 *
 * Part of the host's single translation unit: #included by main.c after host_internal.h. */

#define OPLTRACE_PATH CFG_("opltrace.txt")
/* One line: did the guest find the card, fill it, play it, and take its interrupts?
   Printed from both exits, once. */
static VOID GusReport(VOID)
{
    static INT done = 0;
    CHAR buffer[400], *cursor = buffer;
    UINT voice, running = 0;
    if (done) return;
    done = 1;
    if (!g_GusOn) { cursor = LogPut(cursor, "STAGE2: GUS off (nogus.flag)\r\n"); LogAppend(LOG_PATH, buffer, cursor); return; }
    for (voice = 0; voice < GUS_VOICES; ++voice) if (!(g_Gus.Voices[voice].Control & GUS_VOICE_STOPPED_MASK)) ++running;
    cursor = LogPut(cursor, "STAGE2: GUS io_w=");  cursor = LogHex(cursor, g_Gus.IoWrites);
    cursor = LogPut(cursor, " io_r=");            cursor = LogHex(cursor, g_Gus.IoReads);
    cursor = LogPut(cursor, " reset=0x");         cursor = LogHexByte(cursor, g_Gus.ResetRegister);
    cursor = LogPut(cursor, " dram_pokes=");      cursor = LogHex(cursor, g_Gus.DramPokes);
    cursor = LogPut(cursor, " dram_peeks=");      cursor = LogHex(cursor, g_Gus.DramPeeks);
    cursor = LogPut(cursor, " dma_uploads=");     cursor = LogHex(cursor, g_Gus.DmaUploads);
    cursor = LogPut(cursor, " dma_bytes=");       cursor = LogHex(cursor, g_Gus.DmaBytes);
    cursor = LogPut(cursor, " active=");          cursor = LogHex(cursor, g_Gus.ActiveVoices);
    cursor = LogPut(cursor, " voice_starts=");    cursor = LogHex(cursor, g_Gus.VoiceStarts);
    cursor = LogPut(cursor, " running_now=");     cursor = LogHex(cursor, running);
    cursor = LogPut(cursor, " irqs=");            cursor = LogHex(cursor, g_Gus.IrqsRaised);
    cursor = LogPut(cursor, " fifo_reads=");      cursor = LogHex(cursor, g_Gus.FifoReads);
    cursor = LogPut(cursor, " latch_irq/dma=0x"); cursor = LogHexByte(cursor, g_Gus.IrqLatch);
    cursor = LogPut(cursor, "/0x");               cursor = LogHexByte(cursor, g_Gus.DmaLatch);
    cursor = LogPut(cursor, " locked_out=");      cursor = LogHex(cursor, g_Gus.LatchLockedOut);
    cursor = LogPut(cursor, " samples_out=");     cursor = LogHex(cursor, g_Gus.SamplesOut);
    cursor = LogPut(cursor, " nonzero=");         cursor = LogHex(cursor, g_Gus.OutputNonZero);
    cursor = LogPut(cursor, " peak=");            cursor = LogHex(cursor, g_Gus.OutputPeak);
    cursor = LogPut(cursor, "\r\n"); LogAppend(LOG_PATH, buffer, cursor);
}
/* OPL register trace (opltrace.flag). One entry per write; a busy run is ~6k
   writes a minute, so the cap is far above anything real and exists only so a
   runaway cannot eat memory. Dropped writes are reported, never silently lost. */
#define OPLTRACE_MAX 262144
static struct { DWORD Microseconds; BYTE Register, Value; } g_OplTrace[OPLTRACE_MAX];
static DWORD          g_OplTraceCount    = 0;
static DWORD          g_OplTraceDrop = 0;
static INT            g_OplTraceOn   = 0;
/* The trace hook handed to the OPL VDD. Timestamped from the same clock the CRT
   and PIT use, so a replay reproduces the guest's real WRITE TIMING -- which is
   most of what makes music sound like itself. */
static VOID OplTraceWrite(BYTE registerIndex, BYTE value)
{
    if (g_OplTraceCount >= OPLTRACE_MAX) { g_OplTraceDrop++; return; }
    g_OplTrace[g_OplTraceCount].Microseconds  = (DWORD)HostTimeMicroseconds();
    g_OplTrace[g_OplTraceCount].Register = registerIndex;
    g_OplTrace[g_OplTraceCount].Value = value;
    g_OplTraceCount++;
}
/* Write the trace out as text: one `us reg val` triple per line, hex. Text so it
   is diffable and survives the SMB round trip; a long run is well under a MB. */
static VOID OplTraceDump(VOID)
{
    HANDLE handle; DWORD index, bytesWritten;
    static CHAR buffer[64];
    if (!g_OplTraceOn || !g_OplTraceCount) return;
    handle = CreateFileA(OPLTRACE_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return;
    { PSTR cursor = buffer;
      cursor = LogPut(cursor, "# opl2 register trace: us reg val (hex). writes=");
      cursor = LogHex(cursor, g_OplTraceCount); cursor = LogPut(cursor, " dropped="); cursor = LogHex(cursor, g_OplTraceDrop);
      cursor = LogPut(cursor, "\r\n");
      WriteFile(handle, buffer, (DWORD)(cursor - buffer), &bytesWritten, NULL); }
    for (index = 0; index < g_OplTraceCount; ++index) {
        PSTR cursor = buffer;
        cursor = LogHex(cursor, g_OplTrace[index].Microseconds); cursor = LogPut(cursor, " ");
        cursor = LogHexByte(cursor, g_OplTrace[index].Register); cursor = LogPut(cursor, " ");
        cursor = LogHexByte(cursor, g_OplTrace[index].Value); cursor = LogPut(cursor, "\r\n");
        WriteFile(handle, buffer, (DWORD)(cursor - buffer), &bytesWritten, NULL);
    }
    CloseHandle(handle);
}
enum { OPL_PUMP_QUANTUM_US = 20 };   /* OplPumpTime: shorter is carried to the next pump */
static VOID OplPumpTime(VOID)
{
    static LARGE_INTEGER frequency, last;
    LARGE_INTEGER now;
    LONGLONG delta;
    DWORD microseconds;
    if (!frequency.QuadPart) {
        if (!QueryPerformanceFrequency(&frequency)) return;
        QueryPerformanceCounter(&last);
        return;
    }
    QueryPerformanceCounter(&now);
    delta = now.QuadPart - last.QuadPart;
    if (delta <= 0) return;
    if (delta > frequency.QuadPart) delta = frequency.QuadPart;       /* clamp a long stall to 1s */
    microseconds = (DWORD)((delta * MICROSECONDS_PER_SECOND) / frequency.QuadPart);
    if (microseconds < OPL_PUMP_QUANTUM_US) return;                                /* carry sub-quantum time   */
    last = now;
    HOST_LOCK();
    VddOplAddMicroseconds(&g_Opl, microseconds);
    HOST_UNLOCK();
}
/* The audio thread's fill callback. Mixing touches the DMA controller, guest
   memory and the IRQ path, so it takes the same lock the exec thread uses. */
/* ── WATCH DMX'S TASK TABLE FROM OUTSIDE. ────────────────────────────────────────
     The SB interrupt only ARMS the mixer (sets next_due = now, DOOM.EXE 0x571b4);
     the TIMER services it, and the scheduler's first act on a busy task is
     `jne 0x572ed` -- the loop EXIT, not the next task -- so ONE busy task abandons
     the whole pass and there are up to 12. Doom runs a MIDI task alongside the PCM
     mixer, so a task that overruns can starve the refill wholesale.
     Addresses are settled and self-checked: the IRQ table was FOUND at 0x03bc81ac and
     the code says it lives at virtual 0x281ac, so data guest = virtual + 0x03BA0000.
     That puts the task table at 0x03bc86a0 and the tick clock at 0x03bc8820 -- and it
     predicts the mixer task at index 4 (0x03bc8720), which is exactly where the
     earlier structure search found it. `mixer_ok` re-checks that in-run; if it is 0
     every number here is meaningless.
     Sampled from the audio fill, which runs ~86 times a second -- one sample per
     block, i.e. exactly the rate the refill is supposed to happen at. */
#define DMX_TASKS   0x03bc86a0u
#define DMX_CLOCK   0x03bc8820u
#define DMX_MIXER_I 4u
static VOID DmxSample(VOID)
{
    static INT isOk = 0;
    const volatile BYTE *tasks = (const volatile BYTE *)(ULONG_PTR)DMX_TASKS;
    const volatile DWORD *clock = (const volatile DWORD *)(ULONG_PTR)DMX_CLOCK;
    UINT task; INT anyBusy = 0;
    /* ⚠ RE-PROBE UNTIL IT APPEARS. Probing once latched a failure: this runs from the
         audio thread, which starts long before the guest has allocated the zone this
         table lives in, so the first call always sees unmapped memory and a one-shot
         probe would report `ok=0` for the whole run -- which is exactly what it did. */
    if (!isOk) {
        MEMORY_BASIC_INFORMATION memoryInfo;
        if (!(VirtualQuery((LPCVOID)tasks, &memoryInfo, sizeof memoryInfo) == sizeof memoryInfo
              && memoryInfo.State == MEM_COMMIT && !(memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD))))
            return;
        isOk = 1;
    }
    /* the mixer must be where the addressing predicts, or none of this means anything */
    if (*(const volatile DWORD *)(tasks + DMX_MIXER_I * 32u) == 0x56884u + 0x03AEDFECu)
        g_DmxMixerOk = 1;
    else return;
    ++g_DmxSamples;
    for (task = 0; task < 12; ++task) {
        if (tasks[task * 32u + 0x1c]) { g_DmxBusy[task]++; anyBusy = 1; }
    }
    if (anyBusy) ++g_DmxAnyBusy;
    { DWORD due = *(const volatile DWORD *)(tasks + DMX_MIXER_I * 32u + 0x14), now = *clock;
      if ((LONG)(now - due) >= 0) {            /* armed and still not serviced */
          DWORD late = now - due;
          ++g_DmxOverdue;
          if (late > g_DmxOverdueMaximum) g_DmxOverdueMaximum = late;
      } }
}
static VOID HostAudioFill(PVOID context, INT16 *out, UINT32 frames)
{
    (VOID)context;
    /* #219: paused -> silence, and the devices are NOT rendered, so a Sound Blaster
       block, the OPL envelopes and the GUS voices all stand still and carry on from
       the same sample on resume. */
    if (g_PauseWant) {
        UINT32 index;
        for (index = 0; index < frames * AUDIO_STEREO_CHANNELS; ++index) out[index] = 0;
        return;
    }
    DmxSample();
    HOST_LOCK();
    VddAudioMixStereo(&g_Audio, out, frames);   /* #189: interleaved L/R, as waveOut is opened */
    HOST_UNLOCK();
}

/* MPU-401 output -> the host's MIDI synth (XP ships a GS Wavetable device). */
static VOID HostMidiSink(PVOID context, UINT32 message)
{
    (VOID)context;
    AudioWaveMidi(&g_Wave, message);
}
/* #136: whole SysEx messages, wired ONLY when Settings > Audio > MIDI found an external
   synth by name (g_Wave.IsMidiExternal) -- see midi_route.h. Otherwise SysEx is swallowed in
   vdd_mpu exactly as it always was. */
static VOID HostMidiSysEx(PVOID context, const BYTE *message, UINT32 length)
{
    (VOID)context;
    AudioWaveMidiLong(&g_Wave, message, length);
}

/* #190: the GUS's 6850 MIDI UART sends raw bytes; a PRIVATE message assembler (never on
   the bus) turns them into MIDI messages for the same synth. Its own, not g_Mpu's: two
   byte streams through one assembler would corrupt each other's running status. */
static MPU_STATE g_GusMidi;
static VOID GusMidiToSynth(PVOID context, BYTE byteValue) { (VOID)context; VddMpuFeed(&g_GusMidi, byteValue); }

/* NTVDMEX -- An NTVDM replacement for Microsoft Windows
 *
 * The built-in General MIDI synth (GH #155): see audio_dmsynth.h for why and how.
 *
 * Interfaces only, from the DirectMusic headers; dmusic.dll and dmsynth.dll are reached
 * through COM, and ole32.dll is bound at run time like winmm and dsound (audio_wave.c).
 *
 *
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthew Layton
 */

/* [CAUTION]: THE DIRECTMUSIC HEADERS USE THE NAME PERCENT, which ntvdmex_units.h (through
 * audio_dmsynth.h, which the file order puts first) defines as a number: set aside around them.
 */
#define COBJMACROS
#include "audio_dmsynth.h"
#include <windows.h>
#include <objbase.h>
#pragma push_macro("PERCENT")
#undef PERCENT
#include <initguid.h>   /* the DEFINE_GUIDs below this line become definitions */
#include <dmusicc.h>
#include <dmusici.h>
#include <dmusics.h>
#include <dmksctrl.h>
#pragma pop_macro("PERCENT")
#include "audio_format.h"

#define AUDIO_DMSYNTH_MODULE_OLE32          "ole32.dll"
#define AUDIO_DMSYNTH_EXPORT_CO_INITIALIZE  "CoInitializeEx"
#define AUDIO_DMSYNTH_EXPORT_CO_CREATE      "CoCreateInstance"
#define AUDIO_DMSYNTH_VOICES                128         /* Spike: GM scores peaked at 26 of the default 32 */
#define AUDIO_DMSYNTH_CHANNEL_GROUP         1
#define AUDIO_DMSYNTH_BUFFER_BYTES          32768       /* One mixer tick of MIDI, with room to spare */
#define AUDIO_DMSYNTH_QUEUE_BYTES           32768       /* Messages that arrive while it starts */
#define AUDIO_DMSYNTH_QUEUE_SHORT           0
#define AUDIO_DMSYNTH_QUEUE_LONG            1
#define AUDIO_DMSYNTH_QUEUE_HEADER          3           /* kind byte + 16-bit length */
#define AUDIO_DMSYNTH_REFTIME_PER_SECOND    10000000LL  /* REFERENCE_TIME is in 100 ns */
#define AUDIO_DMSYNTH_MIDI_CHANNELS         16
#define AUDIO_DMSYNTH_STATUS_CONTROL        0xB0
#define AUDIO_DMSYNTH_DATA1_SHIFT           8
#define AUDIO_DMSYNTH_CC_SUSTAIN            64
#define AUDIO_DMSYNTH_CC_ALL_SOUND_OFF      120
#define AUDIO_DMSYNTH_CC_RESET_CONTROLLERS  121
#define AUDIO_DMSYNTH_CC_ALL_NOTES_OFF      123
#define AUDIO_DMSYNTH_ENUM_NAME_CHARS       64
#define AUDIO_DMSYNTH_MS_PER_SECOND         1000

typedef HRESULT (WINAPI *PFN_CO_INITIALIZE_EX)(LPVOID reserved, DWORD flags);

typedef HRESULT (WINAPI *PFN_CO_CREATE_INSTANCE)(REFCLSID classId, LPUNKNOWN outer, DWORD context, REFIID interfaceId, LPVOID *object);

/* The sink and its latency clock: one of each per process, so plain statics. */
typedef struct _AUDIO_DMSYNTH_CLOCK
{
    IReferenceClockVtbl *lpVtbl;
} AUDIO_DMSYNTH_CLOCK;

typedef struct _AUDIO_DMSYNTH_SINK
{
    IDirectMusicSynthSinkVtbl *lpVtbl;
} AUDIO_DMSYNTH_SINK;

static HRESULT STDMETHODCALLTYPE AudioDmSynthClockQueryInterface(
    IReferenceClock *self,
    REFIID interfaceId,
    void **object);
static ULONG STDMETHODCALLTYPE AudioDmSynthClockAddRef(IReferenceClock *self);
static ULONG STDMETHODCALLTYPE AudioDmSynthClockRelease(IReferenceClock *self);
static HRESULT STDMETHODCALLTYPE AudioDmSynthClockGetTime(
    IReferenceClock *self,
    REFERENCE_TIME *time);
static HRESULT STDMETHODCALLTYPE AudioDmSynthClockAdviseTime(
    IReferenceClock *self,
    REFERENCE_TIME baseTime,
    REFERENCE_TIME streamTime,
    HEVENT event,
    DWORD_PTR *cookie);
static HRESULT STDMETHODCALLTYPE AudioDmSynthClockAdvisePeriodic(
    IReferenceClock *self,
    REFERENCE_TIME startTime,
    REFERENCE_TIME periodTime,
    HSEMAPHORE semaphore,
    DWORD_PTR *cookie);
static HRESULT STDMETHODCALLTYPE AudioDmSynthClockUnadvise(IReferenceClock *self, DWORD_PTR cookie);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkQueryInterface(
    IDirectMusicSynthSink *self,
    REFIID interfaceId,
    void **object);
static ULONG STDMETHODCALLTYPE AudioDmSynthSinkAddRef(IDirectMusicSynthSink *self);
static ULONG STDMETHODCALLTYPE AudioDmSynthSinkRelease(IDirectMusicSynthSink *self);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkInit(
    IDirectMusicSynthSink *self,
    IDirectMusicSynth *synth);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkSetMasterClock(
    IDirectMusicSynthSink *self,
    IReferenceClock *clock);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkGetLatencyClock(
    IDirectMusicSynthSink *self,
    IReferenceClock **clock);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkActivate(
    IDirectMusicSynthSink *self,
    BOOL isEnabled);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkSampleToRefTime(
    IDirectMusicSynthSink *self,
    LONGLONG sampleTime,
    REFERENCE_TIME *refTime);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkRefTimeToSample(
    IDirectMusicSynthSink *self,
    REFERENCE_TIME refTime,
    LONGLONG *sampleTime);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkSetDirectSound(
    IDirectMusicSynthSink *self,
    LPDIRECTSOUND sound,
    LPDIRECTSOUNDBUFFER buffer);
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkGetDesiredBufferSize(
    IDirectMusicSynthSink *self,
    LPDWORD samples);

/* IUnknown and IReferenceClock: the two interface ids MIDL declares rather than DEFINE_GUIDs,
 * so <initguid.h> does not define them. The values are the public ones from the SDK headers.
 */
static const GUID g_AudioDmSynthIidUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID g_AudioDmSynthIidReferenceClock = { 0x56A86897, 0x0AD4, 0x11CE, { 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };

static AUDIO_DMSYNTH_STATS g_AudioDmSynthStats;
static CRITICAL_SECTION g_AudioDmSynthLock;     /* the queue, the buffer, the sample clock */
static volatile LONG g_AudioDmSynthState;
static DWORD g_AudioDmSynthHz;
static DWORD g_AudioDmSynthWakeMs;
static IDirectMusic8 *g_AudioDmSynthMusic;
static IDirectMusicPort *g_AudioDmSynthPort;
static IDirectMusicBuffer *g_AudioDmSynthBuffer;
static IDirectMusicSynth *g_AudioDmSynthSynth;  /* handed to the sink's Init by the port */
static IReferenceClock *g_AudioDmSynthMaster;
static REFERENCE_TIME g_AudioDmSynthStartTime;  /* master time of sample 0 */
static LONGLONG g_AudioDmSynthPosition;         /* the next sample Render will produce */
static BOOL g_AudioDmSynthIsPacked;             /* the buffer holds messages not yet played */
static BYTE g_AudioDmSynthQueue[AUDIO_DMSYNTH_QUEUE_BYTES];
static DWORD g_AudioDmSynthQueueLength;
static AUDIO_DMSYNTH_CLOCK g_AudioDmSynthClock;
static AUDIO_DMSYNTH_SINK g_AudioDmSynthSink;

static IReferenceClockVtbl g_AudioDmSynthClockVtbl =
{
    AudioDmSynthClockQueryInterface, AudioDmSynthClockAddRef, AudioDmSynthClockRelease, AudioDmSynthClockGetTime,
    AudioDmSynthClockAdviseTime, AudioDmSynthClockAdvisePeriodic, AudioDmSynthClockUnadvise
};

static IDirectMusicSynthSinkVtbl g_AudioDmSynthSinkVtbl =
{
    AudioDmSynthSinkQueryInterface, AudioDmSynthSinkAddRef, AudioDmSynthSinkRelease, AudioDmSynthSinkInit,
    AudioDmSynthSinkSetMasterClock, AudioDmSynthSinkGetLatencyClock, AudioDmSynthSinkActivate,
    AudioDmSynthSinkSampleToRefTime, AudioDmSynthSinkRefTimeToSample, AudioDmSynthSinkSetDirectSound,
    AudioDmSynthSinkGetDesiredBufferSize
};

static BOOL AudioDmSynthSameGuid(REFGUID left, REFGUID right)
{
    const BYTE *leftBytes = (const BYTE *)left;
    const BYTE *rightBytes = (const BYTE *)right;
    DWORD index;

    for (index = 0; index < sizeof(GUID); ++index)
        if (leftBytes[index] != rightBytes[index])
            return FALSE;

    return TRUE;
}

/* Sample position <-> master time: our audio IS the clock. */
static REFERENCE_TIME AudioDmSynthSampleTime(LONGLONG sample)
{
    return g_AudioDmSynthStartTime + sample * AUDIO_DMSYNTH_REFTIME_PER_SECOND / g_AudioDmSynthHz;
}

/* the clock: */

static HRESULT STDMETHODCALLTYPE AudioDmSynthClockQueryInterface(
    IReferenceClock *self,
    REFIID interfaceId,
    void **object)
{
    if (AudioDmSynthSameGuid(interfaceId, &g_AudioDmSynthIidUnknown)
        || AudioDmSynthSameGuid(interfaceId, &g_AudioDmSynthIidReferenceClock))
    {
        *object = self;
        return S_OK;
    }

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE AudioDmSynthClockAddRef(IReferenceClock *self)
{
    (VOID)self;
    return 2;               /* static: never freed */
}

static ULONG STDMETHODCALLTYPE AudioDmSynthClockRelease(IReferenceClock *self)
{
    (VOID)self;
    return 1;
}

/* The latency clock: the earliest time a message can still sound, the next sample to render. */
static HRESULT STDMETHODCALLTYPE AudioDmSynthClockGetTime(
    IReferenceClock *self,
    REFERENCE_TIME *time)
{
    (VOID)self;
    EnterCriticalSection(&g_AudioDmSynthLock);
    *time = AudioDmSynthSampleTime(g_AudioDmSynthPosition);
    LeaveCriticalSection(&g_AudioDmSynthLock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthClockAdviseTime(
    IReferenceClock *self,
    REFERENCE_TIME baseTime,
    REFERENCE_TIME streamTime,
    HEVENT event,
    DWORD_PTR *cookie)
{
    (VOID)self;
    (VOID)baseTime;
    (VOID)streamTime;
    (VOID)event;
    (VOID)cookie;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthClockAdvisePeriodic(
    IReferenceClock *self,
    REFERENCE_TIME startTime,
    REFERENCE_TIME periodTime,
    HSEMAPHORE semaphore,
    DWORD_PTR *cookie)
{
    (VOID)self;
    (VOID)startTime;
    (VOID)periodTime;
    (VOID)semaphore;
    (VOID)cookie;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthClockUnadvise(IReferenceClock *self, DWORD_PTR cookie)
{
    (VOID)self;
    (VOID)cookie;
    return E_NOTIMPL;
}

/* the sink: */

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkQueryInterface(
    IDirectMusicSynthSink *self,
    REFIID interfaceId,
    void **object)
{
    if (AudioDmSynthSameGuid(interfaceId, &g_AudioDmSynthIidUnknown)
        || AudioDmSynthSameGuid(interfaceId, &IID_IDirectMusicSynthSink))
    {
        *object = self;
        return S_OK;
    }

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE AudioDmSynthSinkAddRef(IDirectMusicSynthSink *self)
{
    (VOID)self;
    return 2;
}

static ULONG STDMETHODCALLTYPE AudioDmSynthSinkRelease(IDirectMusicSynthSink *self)
{
    (VOID)self;
    return 1;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkInit(
    IDirectMusicSynthSink *self,
    IDirectMusicSynth *synth)
{
    (VOID)self;
    g_AudioDmSynthSynth = synth;    /* not AddRef'd: the synth owns its sink */
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkSetMasterClock(
    IDirectMusicSynthSink *self,
    IReferenceClock *clock)
{
    (VOID)self;

    if (g_AudioDmSynthMaster)
        IReferenceClock_Release(g_AudioDmSynthMaster);

    g_AudioDmSynthMaster = clock;

    if (clock)
        IReferenceClock_AddRef(clock);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkGetLatencyClock(
    IDirectMusicSynthSink *self,
    IReferenceClock **clock)
{
    (VOID)self;
    *clock = (IReferenceClock *)&g_AudioDmSynthClock;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkActivate(
    IDirectMusicSynthSink *self,
    BOOL isEnabled)
{
    (VOID)self;

    if (isEnabled)
    {
        if (!g_AudioDmSynthMaster)
            return E_FAIL;

        EnterCriticalSection(&g_AudioDmSynthLock);
        IReferenceClock_GetTime(g_AudioDmSynthMaster, &g_AudioDmSynthStartTime);
        g_AudioDmSynthPosition = 0;
        LeaveCriticalSection(&g_AudioDmSynthLock);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkSampleToRefTime(
    IDirectMusicSynthSink *self,
    LONGLONG sampleTime,
    REFERENCE_TIME *refTime)
{
    (VOID)self;
    *refTime = AudioDmSynthSampleTime(sampleTime);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkRefTimeToSample(
    IDirectMusicSynthSink *self,
    REFERENCE_TIME refTime,
    LONGLONG *sampleTime)
{
    (VOID)self;
    *sampleTime = (refTime - g_AudioDmSynthStartTime) * g_AudioDmSynthHz / AUDIO_DMSYNTH_REFTIME_PER_SECOND;
    return S_OK;
}

/* The port offers its DirectSound; this sink plays nothing itself. */
static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkSetDirectSound(
    IDirectMusicSynthSink *self,
    LPDIRECTSOUND sound,
    LPDIRECTSOUNDBUFFER buffer)
{
    (VOID)self;
    (VOID)sound;
    (VOID)buffer;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE AudioDmSynthSinkGetDesiredBufferSize(
    IDirectMusicSynthSink *self,
    LPDWORD samples)
{
    (VOID)self;
    *samples = g_AudioDmSynthHz;
    return S_OK;
}

/* packing (any thread): */

/* Caller holds the lock and the synth is READY. */
static VOID AudioDmSynthPackShort(DWORD message)
{
    REFERENCE_TIME now = AudioDmSynthSampleTime(g_AudioDmSynthPosition);

    if (FAILED(IDirectMusicBuffer_PackStructured(g_AudioDmSynthBuffer, now, AUDIO_DMSYNTH_CHANNEL_GROUP, message)))
    {
        IDirectMusicPort_PlayBuffer(g_AudioDmSynthPort, g_AudioDmSynthBuffer);   /* full: send it now */
        IDirectMusicBuffer_Flush(g_AudioDmSynthBuffer);
        IDirectMusicBuffer_PackStructured(g_AudioDmSynthBuffer, now, AUDIO_DMSYNTH_CHANNEL_GROUP, message);
    }

    g_AudioDmSynthIsPacked = TRUE;
    g_AudioDmSynthStats.Messages++;
}

static VOID AudioDmSynthPackLong(const BYTE *message, DWORD length)
{
    REFERENCE_TIME now = AudioDmSynthSampleTime(g_AudioDmSynthPosition);

    if (FAILED(IDirectMusicBuffer_PackUnstructured(g_AudioDmSynthBuffer, now, AUDIO_DMSYNTH_CHANNEL_GROUP, length, (LPBYTE)message)))
    {
        IDirectMusicPort_PlayBuffer(g_AudioDmSynthPort, g_AudioDmSynthBuffer);
        IDirectMusicBuffer_Flush(g_AudioDmSynthBuffer);

        if (FAILED(IDirectMusicBuffer_PackUnstructured(g_AudioDmSynthBuffer, now, AUDIO_DMSYNTH_CHANNEL_GROUP, length, (LPBYTE)message)))
        {
            g_AudioDmSynthStats.Dropped++;      /* longer than the whole buffer */
            return;
        }
    }

    g_AudioDmSynthIsPacked = TRUE;
    g_AudioDmSynthStats.SysEx++;
}

/* Caller holds the lock. A message that does not fit is dropped and counted, never cut. */
static VOID AudioDmSynthQueue(BYTE kind, const BYTE *payload, DWORD length)
{
    if (length > 0xFFFF || g_AudioDmSynthQueueLength + AUDIO_DMSYNTH_QUEUE_HEADER + length > AUDIO_DMSYNTH_QUEUE_BYTES)
    {
        g_AudioDmSynthStats.Dropped++;
        return;
    }

    g_AudioDmSynthQueue[g_AudioDmSynthQueueLength] = kind;
    g_AudioDmSynthQueue[g_AudioDmSynthQueueLength + 1] = (BYTE)length;
    g_AudioDmSynthQueue[g_AudioDmSynthQueueLength + 2] = (BYTE)(length >> 8);
    CopyMemory(g_AudioDmSynthQueue + g_AudioDmSynthQueueLength + AUDIO_DMSYNTH_QUEUE_HEADER, payload, length);
    g_AudioDmSynthQueueLength += AUDIO_DMSYNTH_QUEUE_HEADER + length;
    g_AudioDmSynthStats.Queued++;
}

/* Caller holds the lock and the synth is READY: send what arrived while it started. */
static VOID AudioDmSynthDrainQueue(VOID)
{
    DWORD offset = 0;

    while (offset < g_AudioDmSynthQueueLength)
    {
        BYTE kind = g_AudioDmSynthQueue[offset];
        DWORD length = g_AudioDmSynthQueue[offset + 1] | (g_AudioDmSynthQueue[offset + 2] << 8);
        const BYTE *payload = g_AudioDmSynthQueue + offset + AUDIO_DMSYNTH_QUEUE_HEADER;

        if (kind == AUDIO_DMSYNTH_QUEUE_SHORT)
        {
            DWORD message;
            CopyMemory(&message, payload, sizeof(message));
            AudioDmSynthPackShort(message);
        }
        else
            AudioDmSynthPackLong(payload, length);

        offset += AUDIO_DMSYNTH_QUEUE_HEADER + length;
    }

    g_AudioDmSynthQueueLength = 0;
}

/* start (background): */

static BOOL AudioDmSynthStep(PCSTR step, HRESULT result)
{
    if (SUCCEEDED(result))
        return TRUE;

    g_AudioDmSynthStats.FailedStep = step;
    g_AudioDmSynthStats.FailedResult = (LONG)result;
    return FALSE;
}

/* Every step of the s96 spike, in its order. TRUE = the port is active with our sink. */
static BOOL AudioDmSynthOpen(VOID)
{
    HMODULE ole32 = LoadLibraryA(AUDIO_DMSYNTH_MODULE_OLE32);
    PFN_CO_INITIALIZE_EX coInitializeEx;
    PFN_CO_CREATE_INSTANCE coCreateInstance;
    DMUS_PORTPARAMS8 parameters;
    DMUS_BUFFERDESC bufferDescription;
    DMUS_OBJECTDESC objectDescription;
    KSPROPERTY property;

    IKsControl *control = NULL;
    IDirectMusicLoader8 *loader = NULL;
    IDirectMusicCollection *collection = NULL;
    IDirectMusicSynthSink *sinkPointer = (IDirectMusicSynthSink *)&g_AudioDmSynthSink;
    ULONG returned = 0;
    DWORD instrumentIndex;

    if (!ole32)
        return AudioDmSynthStep("LoadLibrary(ole32)", E_FAIL);

    coInitializeEx = (PFN_CO_INITIALIZE_EX)GetProcAddress(ole32, AUDIO_DMSYNTH_EXPORT_CO_INITIALIZE);
    coCreateInstance = (PFN_CO_CREATE_INSTANCE)GetProcAddress(ole32, AUDIO_DMSYNTH_EXPORT_CO_CREATE);

    if (!coInitializeEx || !coCreateInstance)
        return AudioDmSynthStep("GetProcAddress(ole32)", E_FAIL);

    /* This thread only; it ends when the port is up. The objects are called directly from
     * the exec and audio threads afterwards: the port is built for that (its own sink
     * thread renders while the application plays buffers).
     */
    coInitializeEx(NULL, COINIT_MULTITHREADED);

    if (!AudioDmSynthStep("CoCreateInstance(DirectMusic)",
            coCreateInstance(&CLSID_DirectMusic, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectMusic8, (void **)&g_AudioDmSynthMusic)))
        return FALSE;

    if (!AudioDmSynthStep("SetDirectSound", IDirectMusic8_SetDirectSound(g_AudioDmSynthMusic, NULL, NULL)))
        return FALSE;

    ZeroMemory(&parameters, sizeof(parameters));
    parameters.dwSize = sizeof(parameters);
    parameters.dwValidParams = DMUS_PORTPARAMS_VOICES
        | DMUS_PORTPARAMS_CHANNELGROUPS
        | DMUS_PORTPARAMS_AUDIOCHANNELS
        | DMUS_PORTPARAMS_SAMPLERATE
        | DMUS_PORTPARAMS_EFFECTS;
    parameters.dwVoices = AUDIO_DMSYNTH_VOICES;
    parameters.dwChannelGroups = AUDIO_DMSYNTH_CHANNEL_GROUP;
    parameters.dwAudioChannels = AUDIO_STEREO_CHANNELS;
    parameters.dwSampleRate = g_AudioDmSynthHz;
    parameters.dwEffectFlags = DMUS_EFFECT_NONE;

    if (!AudioDmSynthStep("CreatePort(DirectMusicSynth)",
            IDirectMusic8_CreatePort(g_AudioDmSynthMusic, &CLSID_DirectMusicSynth, (LPDMUS_PORTPARAMS)&parameters, &g_AudioDmSynthPort, NULL)))
        return FALSE;

    g_AudioDmSynthStats.Voices = parameters.dwVoices;

    if (parameters.dwSampleRate != g_AudioDmSynthHz)
        return AudioDmSynthStep("CreatePort: sample rate refused", E_FAIL);

    if (!AudioDmSynthStep("QueryInterface(IKsControl)",
            IDirectMusicPort_QueryInterface(g_AudioDmSynthPort, &IID_IKsControl, (void **)&control)))
        return FALSE;

    property.Set = GUID_DMUS_PROP_SetSynthSink;
    property.Id = 0;
    property.Flags = KSPROPERTY_TYPE_SET;

    if (!AudioDmSynthStep("KsProperty(SetSynthSink)",
            control->lpVtbl->KsProperty(control, &property, sizeof(property), &sinkPointer, sizeof(sinkPointer), &returned)))
        return FALSE;

    control->lpVtbl->Release(control);

    if (!g_AudioDmSynthSynth)
        return AudioDmSynthStep("sink Init never called", E_FAIL);

    /* Windows' own GS sound set, every instrument in it. */
    if (!AudioDmSynthStep("CoCreateInstance(DirectMusicLoader)",
            coCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectMusicLoader8, (void **)&loader)))
        return FALSE;

    ZeroMemory(&objectDescription, sizeof(objectDescription));
    objectDescription.dwSize = sizeof(objectDescription);
    objectDescription.dwValidData = DMUS_OBJ_OBJECT | DMUS_OBJ_CLASS;
    objectDescription.guidObject = GUID_DefaultGMCollection;
    objectDescription.guidClass = CLSID_DirectMusicCollection;

    if (!AudioDmSynthStep("GetObject(DefaultGMCollection)",
            IDirectMusicLoader8_GetObject(loader, &objectDescription, &IID_IDirectMusicCollection, (void **)&collection)))
        return FALSE;

    for (instrumentIndex = 0; ; ++instrumentIndex)
    {
        DWORD patch;
        WCHAR name[AUDIO_DMSYNTH_ENUM_NAME_CHARS];
        IDirectMusicInstrument *instrument = NULL;
        IDirectMusicDownloadedInstrument *downloaded = NULL;

        if (IDirectMusicCollection_EnumInstrument(collection, instrumentIndex, &patch, name, AUDIO_DMSYNTH_ENUM_NAME_CHARS) != S_OK)
            break;

        if (FAILED(IDirectMusicCollection_GetInstrument(collection, patch, &instrument)))
            continue;

        if (SUCCEEDED(IDirectMusicPort_DownloadInstrument(g_AudioDmSynthPort, instrument, &downloaded, NULL, 0)))
            g_AudioDmSynthStats.Instruments++;

        IDirectMusicInstrument_Release(instrument);     /* the download keeps its own reference */
    }

    if (!g_AudioDmSynthStats.Instruments)
        return AudioDmSynthStep("DownloadInstrument: none", E_FAIL);

    ZeroMemory(&bufferDescription, sizeof(bufferDescription));
    bufferDescription.dwSize = sizeof(bufferDescription);
    bufferDescription.guidBufferFormat = KSDATAFORMAT_SUBTYPE_DIRECTMUSIC;
    bufferDescription.cbBuffer = AUDIO_DMSYNTH_BUFFER_BYTES;

    if (!AudioDmSynthStep("CreateMusicBuffer",
            IDirectMusic8_CreateMusicBuffer(g_AudioDmSynthMusic, &bufferDescription, &g_AudioDmSynthBuffer, NULL)))
        return FALSE;

    return AudioDmSynthStep("port Activate", IDirectMusicPort_Activate(g_AudioDmSynthPort, TRUE));
}

static DWORD WINAPI AudioDmSynthStartThread(LPVOID parameter)
{
    BOOL isOpen;

    (VOID)parameter;
    isOpen = AudioDmSynthOpen();
    EnterCriticalSection(&g_AudioDmSynthLock);
    g_AudioDmSynthStats.StartMs = GetTickCount() - g_AudioDmSynthWakeMs;

    if (isOpen)
        AudioDmSynthDrainQueue();
    else
    {
        g_AudioDmSynthStats.Dropped += g_AudioDmSynthStats.Queued;     /* nowhere to send them */
        g_AudioDmSynthQueueLength = 0;
    }

    InterlockedExchange(&g_AudioDmSynthState, isOpen ? AUDIO_DMSYNTH_READY : AUDIO_DMSYNTH_FAILED);
    g_AudioDmSynthStats.State = g_AudioDmSynthState;
    LeaveCriticalSection(&g_AudioDmSynthLock);
    return 0;
}

/* the API: */

VOID AudioDmSynthPrepare(DWORD sampleHz)
{
    InitializeCriticalSection(&g_AudioDmSynthLock);
    g_AudioDmSynthHz = sampleHz;
    g_AudioDmSynthClock.lpVtbl = &g_AudioDmSynthClockVtbl;
    g_AudioDmSynthSink.lpVtbl = &g_AudioDmSynthSinkVtbl;
}

VOID AudioDmSynthWake(VOID)
{
    HANDLE thread;

    if (!g_AudioDmSynthHz || InterlockedCompareExchange(&g_AudioDmSynthState, AUDIO_DMSYNTH_STARTING, AUDIO_DMSYNTH_IDLE) != AUDIO_DMSYNTH_IDLE)
        return;

    g_AudioDmSynthStats.State = AUDIO_DMSYNTH_STARTING;
    g_AudioDmSynthWakeMs = GetTickCount();
    thread = CreateThread(NULL, 0, AudioDmSynthStartThread, NULL, 0, NULL);

    if (!thread)
    {
        AudioDmSynthStep("CreateThread", E_FAIL);
        InterlockedExchange(&g_AudioDmSynthState, AUDIO_DMSYNTH_FAILED);
        g_AudioDmSynthStats.State = AUDIO_DMSYNTH_FAILED;
        return;
    }

    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);   /* never ahead of the guest */
    CloseHandle(thread);
}

BOOL AudioDmSynthShort(DWORD message)
{
    if (g_AudioDmSynthState == AUDIO_DMSYNTH_IDLE)
        AudioDmSynthWake();

    if (g_AudioDmSynthState == AUDIO_DMSYNTH_FAILED)
        return FALSE;

    EnterCriticalSection(&g_AudioDmSynthLock);

    if (g_AudioDmSynthState == AUDIO_DMSYNTH_READY)
        AudioDmSynthPackShort(message);
    else if (g_AudioDmSynthState == AUDIO_DMSYNTH_STARTING)
        AudioDmSynthQueue(AUDIO_DMSYNTH_QUEUE_SHORT, (const BYTE *)&message, sizeof(message));

    LeaveCriticalSection(&g_AudioDmSynthLock);
    return g_AudioDmSynthState != AUDIO_DMSYNTH_FAILED;
}

BOOL AudioDmSynthLong(const BYTE *message, DWORD length)
{
    if (g_AudioDmSynthState == AUDIO_DMSYNTH_IDLE)
        AudioDmSynthWake();

    if (g_AudioDmSynthState == AUDIO_DMSYNTH_FAILED)
        return FALSE;

    EnterCriticalSection(&g_AudioDmSynthLock);

    if (g_AudioDmSynthState == AUDIO_DMSYNTH_READY)
        AudioDmSynthPackLong(message, length);
    else if (g_AudioDmSynthState == AUDIO_DMSYNTH_STARTING)
        AudioDmSynthQueue(AUDIO_DMSYNTH_QUEUE_LONG, message, length);

    LeaveCriticalSection(&g_AudioDmSynthLock);
    return g_AudioDmSynthState != AUDIO_DMSYNTH_FAILED;
}

VOID AudioDmSynthSilence(VOID)
{
    DWORD channel;

    if (g_AudioDmSynthState != AUDIO_DMSYNTH_READY && g_AudioDmSynthState != AUDIO_DMSYNTH_STARTING)
        return;

    for (channel = 0; channel < AUDIO_DMSYNTH_MIDI_CHANNELS; ++channel)
    {
        DWORD control = AUDIO_DMSYNTH_STATUS_CONTROL | channel;
        AudioDmSynthShort(control | (AUDIO_DMSYNTH_CC_SUSTAIN << AUDIO_DMSYNTH_DATA1_SHIFT));
        AudioDmSynthShort(control | (AUDIO_DMSYNTH_CC_ALL_SOUND_OFF << AUDIO_DMSYNTH_DATA1_SHIFT));
        AudioDmSynthShort(control | (AUDIO_DMSYNTH_CC_ALL_NOTES_OFF << AUDIO_DMSYNTH_DATA1_SHIFT));
        AudioDmSynthShort(control | (AUDIO_DMSYNTH_CC_RESET_CONTROLLERS << AUDIO_DMSYNTH_DATA1_SHIFT));
    }
}

BOOL AudioDmSynthRender(INT16 *output, DWORD frames)
{
    LARGE_INTEGER before;
    LARGE_INTEGER after;
    LONGLONG position;
    DWORD sample;

    if (g_AudioDmSynthState != AUDIO_DMSYNTH_READY)
        return FALSE;

    EnterCriticalSection(&g_AudioDmSynthLock);

    if (g_AudioDmSynthIsPacked)
    {
        IDirectMusicPort_PlayBuffer(g_AudioDmSynthPort, g_AudioDmSynthBuffer);
        IDirectMusicBuffer_Flush(g_AudioDmSynthBuffer);
        g_AudioDmSynthIsPacked = FALSE;
    }

    position = g_AudioDmSynthPosition;
    LeaveCriticalSection(&g_AudioDmSynthLock);

    for (sample = 0; sample < frames * AUDIO_STEREO_CHANNELS; ++sample)
        output[sample] = 0;

    /* Outside the lock: the exec thread can keep packing while the synth renders. */
    QueryPerformanceCounter(&before);
    IDirectMusicSynth_Render(g_AudioDmSynthSynth, output, frames, position);
    QueryPerformanceCounter(&after);

    EnterCriticalSection(&g_AudioDmSynthLock);
    g_AudioDmSynthPosition += frames;
    g_AudioDmSynthStats.RenderCalls++;
    g_AudioDmSynthStats.RenderTicks += after.QuadPart - before.QuadPart;

    if (after.QuadPart - before.QuadPart > g_AudioDmSynthStats.RenderTicksWorst)
        g_AudioDmSynthStats.RenderTicksWorst = after.QuadPart - before.QuadPart;

    LeaveCriticalSection(&g_AudioDmSynthLock);
    return TRUE;
}

VOID AudioDmSynthStop(VOID)
{
    if (g_AudioDmSynthState == AUDIO_DMSYNTH_READY && g_AudioDmSynthPort)
        IDirectMusicPort_Activate(g_AudioDmSynthPort, FALSE);
}

PCAUDIO_DMSYNTH_STATS AudioDmSynthStats(VOID)
{
    return &g_AudioDmSynthStats;
}

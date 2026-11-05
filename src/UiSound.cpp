/*
FILE : UiSound.cpp
PROJECT : Omni360
DESCRIPTION : The interface's sounds. See UiSound.h.
*/

#include "UiSound.h"
#include "OutputConsole.h" // dprintf

#include <xtl.h>
#include <xaudio2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SOUND_RATE 48000

// A sound is one or more taps, mixed: a sine at a pitch - which can glide to
// another over the first few milliseconds - under an envelope that rises,
// decays and fades out at the end, with noise under the same envelope if
// noise is set. The same table is in tools/preview_ui_sounds.py, which
// renders them to WAVs to hear on a PC; change both.
struct Tap
{
    float fromHz;
    float toHz;
    float glideMs;
    float ms;
    float gain;
    float decayMs;
    float noise;
    float startMs;
    float riseMs;
    float harmonic;     // the second harmonic's share: 0 for a pure sine
    float noiseDecayMs; // 0 for the old 2 ms puff
    float noiseCut;     // the noise's one-pole smoothing; 0 for 0.25
};

struct SoundDef
{
    const Tap *taps;
    int tapCount;
    int loopSamples; // 0 for a one-shot
};

struct Clip
{
    short *pcm;
    UINT32 bytes;
    UINT32 loopSamples;
    IXAudio2SourceVoice *voice;
};

static IXAudio2 *g_audio = NULL;
static IXAudio2MasteringVoice *g_master = NULL;
static Clip g_clips[UI_SOUND_COUNT];
static volatile LONG g_clipReady[UI_SOUND_COUNT];
static bool g_started = false;
static bool g_on = true;

static unsigned long g_seed = 1;

// White noise, -1 to 1.
static float Noise()
{
    g_seed = g_seed * 1103515245UL + 12345UL;
    return (float)((g_seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

static int Samples(float ms)
{
    return (int)(ms * SOUND_RATE / 1000.0f);
}

static void MixTap(const Tap &tap, float *mix, int total)
{
    const float twoPi = 6.2831853f;
    const int samples = Samples(tap.ms), first = Samples(tap.startMs);
    const int rise = Samples(tap.riseMs), fall = Samples(8.0f);
    const float noiseDecay = tap.noiseDecayMs > 0.0f ? tap.noiseDecayMs : 2.0f;
    const float noiseCut = tap.noiseCut > 0.0f ? tap.noiseCut : 0.25f;
    float phase = 0.0f, smooth = 0.0f;
    for (int i = 0; i < samples && first + i < total; ++i)
    {
        const float tMs = i * 1000.0f / SOUND_RATE;
        const float hz = tap.toHz + (tap.fromHz - tap.toHz) * expf(-tMs / tap.glideMs);
        phase += twoPi * hz / SOUND_RATE;
        if (phase > twoPi)
            phase -= twoPi;

        // One shape for the whole tap, so tone and noise swell and die
        // together - a whoosh is noise with a long rise, nothing more.
        float shape = 1.0f;
        if (i < rise && rise > 0)
            shape = (float)i / rise;
        if (i > samples - fall)
            shape *= (float)(samples - i) / fall;

        const float envelope = tap.gain * expf(-tMs / tap.decayMs) * shape;
        smooth += (Noise() - smooth) * noiseCut;
        const float puff = tap.noise * expf(-tMs / noiseDecay) * shape * smooth;
        mix[first + i] += envelope * (sinf(phase) + tap.harmonic * sinf(phase * 2.0f)) + puff;
    }
}

// The sound into 16-bit mono PCM - native byte order, which is what XAudio2
// takes on the 360.
static short *Synthesise(const SoundDef &def, UINT32 *outBytes)
{
    float endMs = 0.0f;
    for (int n = 0; n < def.tapCount; ++n)
    {
        if (def.taps[n].startMs + def.taps[n].ms > endMs)
            endMs = def.taps[n].startMs + def.taps[n].ms;
    }
    const int total = Samples(endMs);
    float *mix = (float *)calloc(total, sizeof(float));
    short *pcm = (short *)malloc(total * sizeof(short));
    if (mix == NULL || pcm == NULL || total <= 0)
    {
        free(mix);
        free(pcm);
        return NULL;
    }

    for (int n = 0; n < def.tapCount; ++n)
        MixTap(def.taps[n], mix, total);

    for (int i = 0; i < total; ++i)
    {
        float v = mix[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (short)(v * 32767.0f);
    }
    free(mix);
    *outBytes = (UINT32)(total * sizeof(short));
    return pcm;
}

static void FillFormat(WAVEFORMATEX *format)
{
    memset(format, 0, sizeof(*format));
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = 1;
    format->nSamplesPerSec = SOUND_RATE;
    format->wBitsPerSample = 16;
    format->nBlockAlign = 2;
    format->nAvgBytesPerSec = SOUND_RATE * 2;
}

// A sound rendered, with a voice to play it on, published so the UI thread
// sees the whole clip before it sees the flag.
static bool BuildClip(const SoundDef &def, UiSound slot)
{
    WAVEFORMATEX format;
    FillFormat(&format);
    Clip clip;
    memset(&clip, 0, sizeof(clip));
    clip.loopSamples = (UINT32)def.loopSamples;
    clip.pcm = Synthesise(def, &clip.bytes);
    if (clip.pcm == NULL || FAILED(g_audio->CreateSourceVoice(&clip.voice, &format)))
    {
        dprintf("[sound] couldn't set up sound %d\n", (int)slot);
        return false;
    }
    g_clips[slot] = clip;
#ifdef _XBOX
    __lwsync();
#endif
    InterlockedExchange(&g_clipReady[slot], 1);
    return true;
}

// ---------------------------------------------------------------------------
// The tables
// ---------------------------------------------------------------------------
//
// from Hz, to Hz, glide ms, length ms, gain, decay ms, noise gain,
// start ms, rise ms, harmonic, noise decay ms, noise smoothing.
//
// Navigation, fitted to sounds the user picked: a soft, high tick to move, a
// short tick for A, and a low round bump for B - its fast fade and long
// quiet tail as two taps at one pitch. Pure sines, no glide.
static const Tap kMove[] = { { 523.0f, 523.0f, 1.0f, 40.0f, 0.28f, 21.0f, 0.0f, 0.0f, 4.4f, 0.01f } };
static const Tap kSelect[] = { { 481.0f, 481.0f, 1.0f, 45.0f, 0.28f, 12.5f, 0.0f, 0.0f, 4.4f, 0.01f } };
static const Tap kBack[] = { { 187.0f, 187.0f, 1.0f, 215.0f, 0.78f, 18.0f, 0.0f, 0.0f, 4.4f, 0.01f },
                             { 187.0f, 187.0f, 1.0f, 215.0f, 0.34f, 110.0f, 0.0f, 0.0f, 4.4f, 0.01f } };

// HEAD - the disc arriving, against the animation: it fades in at 0, travels
// at 340, meets the slot at 820, and the motor comes up from 740. That last
// tap runs past the head's end on purpose, so the loop starting underneath
// it is not heard joining.
static const Tap kHead[] = {
    { 392.00f, 523.25f, 170.0f, 430.0f, 0.105f, 300.0f, 0.00f, 0.0f, 180.0f, 0.10f, 0.0f, 0.0f },
    { 196.00f, 261.63f, 170.0f, 430.0f, 0.080f, 320.0f, 0.00f, 0.0f, 200.0f, 0.08f, 0.0f, 0.0f },
    { 150.00f, 92.00f, 190.0f, 450.0f, 0.070f, 420.0f, 0.26f, 340.0f, 150.0f, 0.20f, 300.0f, 0.05f },
    { 150.00f, 96.00f, 16.0f, 180.0f, 0.200f, 55.0f, 0.05f, 820.0f, 2.0f, 0.25f, 0.0f, 0.0f },
    { 31.00f, 98.00f, 46.0f, 230.0f, 0.150f, 9e8f, 0.00f, 740.0f, 22.0f, 0.34f, 0.0f, 0.0f },
};

// SPIN - one revolution, looped. The motor is G2 at exactly 40 cycles across
// the loop, its partials at 80, 120 and 160, so nothing beats against
// anything and the loop joins itself. Over it, once a turn, the printed
// label passing the head: the tick measured off the Steam Deck recording,
// four partials with no attack at all, which is what keeps it soft however
// many times it comes round. The taps run 8 ms past the loop so the
// renderer's own fade-out never reaches into it - the buffer plays and loops
// UI_SPIN_LOOP_SAMPLES and those last milliseconds are never heard.
static const Tap kSpin[] = {
    { 98.00f, 98.00f, 1.0f, UI_SPIN_LOOP_MS + 8.0f, 0.150f, 9e8f, 0.0f, 0.0f, 0.0f, 0.34f, 0.0f, 0.0f },
    { 196.00f, 196.00f, 1.0f, UI_SPIN_LOOP_MS + 8.0f, 0.048f, 9e8f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f, 0.0f },
    { 294.00f, 294.00f, 1.0f, UI_SPIN_LOOP_MS + 8.0f, 0.024f, 9e8f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f, 0.0f },
    { 392.00f, 392.00f, 1.0f, UI_SPIN_LOOP_MS + 8.0f, 0.012f, 9e8f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f, 0.0f },

    { 196.00f, 196.00f, 1.0f, 52.0f, 0.300f, 90.0f, 0.0f, 0.0f, 24.0f, 0.0f, 0.0f, 0.0f },
    { 392.00f, 392.00f, 1.0f, 52.0f, 0.135f, 90.0f, 0.0f, 0.0f, 24.0f, 0.0f, 0.0f, 0.0f },
    { 588.00f, 588.00f, 1.0f, 52.0f, 0.084f, 90.0f, 0.0f, 0.0f, 24.0f, 0.0f, 0.0f, 0.0f },
    { 784.00f, 784.00f, 1.0f, 52.0f, 0.060f, 90.0f, 0.0f, 0.0f, 24.0f, 0.0f, 0.0f, 0.0f },
};

// TAIL - the motor falling away over the deceleration, one last tick as the
// label goes by, the drive taking the disc at 680, and the chord that
// resolves it at 720, under the wordmark washing in. The whole loading
// section sits on G; this is the C it was waiting for, so a load of any
// length lands on a cadence. Its first tap rises over 30 ms, which is the
// crossfade the spin is faded out under.
static const Tap kTail[] = {
    { 98.00f, 33.00f, 300.0f, 700.0f, 0.165f, 900.0f, 0.00f, 0.0f, 30.0f, 0.34f, 0.0f, 0.0f },
    { 196.00f, 160.00f, 320.0f, 300.0f, 0.120f, 150.0f, 0.00f, 120.0f, 24.0f, 0.00f, 0.0f, 0.0f },

    { 228.00f, 70.00f, 11.0f, 280.0f, 0.520f, 46.0f, 0.16f, 680.0f, 1.5f, 0.30f, 34.0f, 0.18f },
    { 1760.00f, 1180.00f, 8.0f, 70.0f, 0.070f, 13.0f, 0.05f, 680.0f, 1.0f, 0.00f, 9.0f, 0.60f },

    { 130.81f, 130.81f, 1.0f, 1340.0f, 0.260f, 620.0f, 0.0f, 720.0f, 12.0f, 0.22f, 0.0f, 0.0f },
    { 196.00f, 196.00f, 1.0f, 1340.0f, 0.140f, 640.0f, 0.0f, 720.0f, 16.0f, 0.14f, 0.0f, 0.0f },
    { 329.63f, 329.63f, 1.0f, 1300.0f, 0.115f, 660.0f, 0.0f, 736.0f, 20.0f, 0.12f, 0.0f, 0.0f },
    { 523.25f, 523.25f, 1.0f, 1260.0f, 0.085f, 600.0f, 0.0f, 752.0f, 24.0f, 0.10f, 0.0f, 0.0f },
    { 783.99f, 783.99f, 1.0f, 760.0f, 0.048f, 300.0f, 0.0f, 900.0f, 10.0f, 0.06f, 0.0f, 0.0f },
};

#define TAPS(a) (sizeof(a) / sizeof((a)[0]))

// The startup parts, in the order they are needed. Nearly four seconds of
// audio is a tenth of a second to synthesise, so it is made while the black
// screen before the first frame is already going rather than in front of it;
// each part is published as it finishes, and the head - the one wanted
// soonest and the quickest to make - comes first.
static DWORD WINAPI SynthesiseStartup(LPVOID)
{
    const SoundDef head = { kHead, (int)TAPS(kHead), 0 };
    const SoundDef spin = { kSpin, (int)TAPS(kSpin), UI_SPIN_LOOP_SAMPLES };
    const SoundDef tail = { kTail, (int)TAPS(kTail), 0 };
    BuildClip(head, UI_SOUND_STARTUP_HEAD);
    BuildClip(spin, UI_SOUND_STARTUP_SPIN);
    BuildClip(tail, UI_SOUND_STARTUP_TAIL);
    return 0;
}

bool StartUiSounds()
{
    if (g_started)
        return true;

    HRESULT hr = XAudio2Create(&g_audio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        dprintf("[sound] XAudio2 didn't start (0x%08lX) - no interface sounds\n", (unsigned long)hr);
        return false;
    }
    hr = g_audio->CreateMasteringVoice(&g_master);
    if (FAILED(hr))
    {
        dprintf("[sound] no mastering voice (0x%08lX) - no interface sounds\n", (unsigned long)hr);
        g_audio->Release();
        g_audio = NULL;
        return false;
    }

    memset(g_clips, 0, sizeof(g_clips));
    memset((void *)g_clipReady, 0, sizeof(g_clipReady));

    // The navigation sounds are half a second of audio between them, so they
    // are made here; the startup score is twenty times that and goes on the
    // thread below.
    const SoundDef move = { kMove, (int)TAPS(kMove), 0 };
    const SoundDef select = { kSelect, (int)TAPS(kSelect), 0 };
    const SoundDef back = { kBack, (int)TAPS(kBack), 0 };
    BuildClip(move, UI_SOUND_MOVE);
    BuildClip(select, UI_SOUND_SELECT);
    BuildClip(back, UI_SOUND_BACK);

    HANDLE worker = CreateThread(NULL, 64 * 1024, SynthesiseStartup, NULL, CREATE_SUSPENDED, NULL);
    if (worker != NULL)
    {
#ifdef _XBOX
        // Hardware thread 4, core 2's first: a thread made without one goes
        // on 0, which is the one about to draw the startup screen.
        XSetThreadProcessor(worker, 4);
#endif
        ResumeThread(worker);
        CloseHandle(worker);
    }
    else
        dprintf("[sound] no thread for the startup score (%lu) - it starts quietly\n", GetLastError());

    g_started = true;
    dprintf("[sound] interface sounds ready\n");
    return true;
}

// The clip, or NULL if it is not built yet - the barrier pairs with
// BuildClip's, so what is read here is the whole thing.
static Clip *ReadyClip(UiSound sound)
{
    if (!g_started || sound < 0 || sound >= UI_SOUND_COUNT)
        return NULL;
    if (InterlockedCompareExchange(&g_clipReady[sound], 1, 1) == 0)
        return NULL;
#ifdef _XBOX
    __lwsync();
#endif
    Clip *clip = &g_clips[sound];
    return (clip->voice != NULL && clip->pcm != NULL) ? clip : NULL;
}

void PlayUiSound(UiSound sound)
{
    Clip *clip = ReadyClip(sound);
    if (!g_on || clip == NULL)
        return;

    clip->voice->Stop(0);
    clip->voice->FlushSourceBuffers();
    clip->voice->SetVolume(1.0f);

    XAUDIO2_BUFFER buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.AudioBytes = clip->bytes;
    buffer.pAudioData = (const BYTE *)clip->pcm;
    if (clip->loopSamples > 0)
    {
        // Play and loop exactly one revolution; the few milliseconds the
        // taps run past it are only there to keep the fade-out off the seam.
        buffer.PlayBegin = 0;
        buffer.PlayLength = clip->loopSamples;
        buffer.LoopBegin = 0;
        buffer.LoopLength = clip->loopSamples;
        buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    }
    else
        buffer.Flags = XAUDIO2_END_OF_STREAM;

    if (SUCCEEDED(clip->voice->SubmitSourceBuffer(&buffer)))
        clip->voice->Start(0);
}

void FadeUiSound(UiSound sound, float volume)
{
    Clip *clip = ReadyClip(sound);
    if (clip == NULL)
        return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    clip->voice->SetVolume(volume);
}

void StopUiSound(UiSound sound)
{
    Clip *clip = ReadyClip(sound);
    if (clip == NULL)
        return;
    clip->voice->Stop(0);
    clip->voice->FlushSourceBuffers();
}

void SetUiSoundsOn(bool on)
{
    g_on = on;
}

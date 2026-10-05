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

// A sound is one or more taps, mixed: a sine at a pitch - which can drop
// to another over the first few milliseconds, a "tok" - rising over riseMs
// and fading from gain, with a puff of soft noise at its start if noise is
// set. The same table is in tools/preview_ui_sounds.py, which renders it to
// WAVs to hear on a PC.
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
    float harmonic; // the second harmonic's share: 0 for a pure sine
};

struct SoundDef
{
    const Tap *taps;
    int tapCount;
};

struct Clip
{
    short *pcm;
    UINT32 bytes;
    IXAudio2SourceVoice *voice;
};

static IXAudio2 *g_audio = NULL;
static IXAudio2MasteringVoice *g_master = NULL;
static Clip g_clips[UI_SOUND_COUNT];
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
    float phase = 0.0f, smooth = 0.0f;
    for (int i = 0; i < samples && first + i < total; ++i)
    {
        const float tMs = i * 1000.0f / SOUND_RATE;
        const float hz = tap.toHz + (tap.fromHz - tap.toHz) * expf(-tMs / tap.glideMs);
        phase += twoPi * hz / SOUND_RATE;
        if (phase > twoPi)
            phase -= twoPi;

        float envelope = tap.gain * expf(-tMs / tap.decayMs);
        if (i < rise && rise > 0)
            envelope *= (float)i / rise;
        if (i > samples - fall)
            envelope *= (float)(samples - i) / fall;

        smooth += (Noise() - smooth) * 0.25f; // dull, not hissy
        const float puff = tap.noise * expf(-tMs / 2.0f) * smooth;
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

    // Fitted to sounds the user picked: a soft, high tick to move, a short
    // tick for A, and a low round bump for B - its fast fade and long quiet
    // tail as two taps at one pitch. Pure sines, no glide.
    //
    // from Hz, to Hz, glide ms, length ms, gain, decay ms, noise gain, start ms, rise ms, harmonic
    static const Tap kMove[] = { { 523.0f, 523.0f, 1.0f, 40.0f, 0.28f, 21.0f, 0.0f, 0.0f, 4.4f, 0.01f } };
    static const Tap kSelect[] = { { 481.0f, 481.0f, 1.0f, 45.0f, 0.28f, 12.5f, 0.0f, 0.0f, 4.4f, 0.01f } };
    static const Tap kBack[] = { { 187.0f, 187.0f, 1.0f, 215.0f, 0.78f, 18.0f, 0.0f, 0.0f, 4.4f, 0.01f },
                                 { 187.0f, 187.0f, 1.0f, 215.0f, 0.34f, 110.0f, 0.0f, 0.0f, 4.4f, 0.01f } };

    const SoundDef defs[UI_SOUND_COUNT] = {
        { kMove, 1 },
        { kSelect, 1 },
        { kBack, 2 },
    };

    WAVEFORMATEX format;
    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = SOUND_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = SOUND_RATE * 2;

    memset(g_clips, 0, sizeof(g_clips));
    for (int s = 0; s < UI_SOUND_COUNT; ++s)
    {
        g_clips[s].pcm = Synthesise(defs[s], &g_clips[s].bytes);
        if (g_clips[s].pcm == NULL || FAILED(g_audio->CreateSourceVoice(&g_clips[s].voice, &format)))
        {
            dprintf("[sound] couldn't set up sound %d\n", s);
            g_clips[s].voice = NULL;
        }
    }
    g_started = true;
    dprintf("[sound] interface sounds ready\n");
    return true;
}

void PlayUiSound(UiSound sound)
{
    if (!g_started || !g_on || sound < 0 || sound >= UI_SOUND_COUNT)
        return;
    Clip &clip = g_clips[sound];
    if (clip.voice == NULL || clip.pcm == NULL)
        return;

    clip.voice->Stop(0);
    clip.voice->FlushSourceBuffers();

    XAUDIO2_BUFFER buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.AudioBytes = clip.bytes;
    buffer.pAudioData = (const BYTE *)clip.pcm;
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (SUCCEEDED(clip.voice->SubmitSourceBuffer(&buffer)))
        clip.voice->Start(0);
}

void SetUiSoundsOn(bool on)
{
    g_on = on;
}

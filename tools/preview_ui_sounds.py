"""Renders the interface sounds to WAV files, to hear them on a PC.

The same synthesis as src/UiSound.cpp, number for number - change the tables
in both. Writes docs/sound-preview/{move,select,back}.wav, and prints how
loud each is.

A sound is one or more taps, mixed: a sine at a pitch - which can drop to
another over the first few milliseconds - rising over a few milliseconds and
fading away. The numbers were fitted to sounds the user picked: a soft high
tick to move, a short tick for A, a low round bump for B.
"""
import math
import os
import random
import struct
import wave

RATE = 48000

# from Hz, to Hz, glide ms, length ms, gain, decay ms, noise gain, start ms, rise ms, harmonic
SOUNDS = {
    'move': [(523, 523, 1, 40, 0.28, 21, 0.0, 0, 4.4, 0.01)],
    'select': [(481, 481, 1, 45, 0.28, 12.5, 0.0, 0, 4.4, 0.01)],
    'back': [(187, 187, 1, 215, 0.78, 18, 0.0, 0, 4.4, 0.01),   # the bump
             (187, 187, 1, 215, 0.34, 110, 0.0, 0, 4.4, 0.01)],  # its long, quiet tail
}


def render(taps):
    end = max(t[7] + t[3] for t in taps)
    out = [0.0] * int(end * RATE / 1000)
    rng = random.Random(1)
    for from_hz, to_hz, glide, ms, gain, decay, noise, start, rise_ms, harmonic in taps:
        n, first = int(ms * RATE / 1000), int(start * RATE / 1000)
        rise, fall = int(RATE * rise_ms / 1000), RATE * 8 // 1000
        phase = smooth = 0.0
        for i in range(n):
            t_ms = i * 1000.0 / RATE
            hz = to_hz + (from_hz - to_hz) * math.exp(-t_ms / glide)
            phase += 2 * math.pi * hz / RATE
            env = gain * math.exp(-t_ms / decay)
            if i < rise:
                env *= i / rise
            if i > n - fall:
                env *= (n - i) / fall
            smooth += (rng.uniform(-1, 1) - smooth) * 0.25
            puff = noise * math.exp(-t_ms / 2.0) * smooth
            out[first + i] += env * (math.sin(phase) + harmonic * math.sin(2 * phase)) + puff
    return out


def write(path, samples):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767)))) for s in samples))


if __name__ == '__main__':
    folder = os.path.join(os.path.dirname(__file__), '..', 'docs', 'sound-preview')
    os.makedirs(folder, exist_ok=True)
    for name, taps in SOUNDS.items():
        samples = render(taps)
        write(os.path.join(folder, name + '.wav'), samples)
        peak = max(abs(s) for s in samples)
        rms = math.sqrt(sum(s * s for s in samples) / len(samples))
        print('%-7s %4d ms  peak %.2f  rms %.3f' % (name, len(samples) * 1000 // RATE, peak, rms))
    print('wrote', os.path.abspath(folder))

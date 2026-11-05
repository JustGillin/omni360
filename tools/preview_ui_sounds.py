"""Renders the interface sounds to WAV files, to hear them on a PC.

The same synthesis as src/UiSound.cpp, number for number - change the tables
in both. Writes docs/sound-preview/*.wav and prints how loud each is.

A sound is one or more taps, mixed: a sine at a pitch - which can glide to
another - under a shared envelope that rises, decays and fades out, plus
optional noise under the same envelope. The navigation sounds were fitted to
ones the user picked: a soft high tick to move, a short tick for A, a low
round bump for B.


The startup screen
------------------

Scored against the animation, in three parts, because the load is of unknown
length and only the middle may stretch:

    HEAD   0 ms        the disc fades in
           340         it travels to the slot - air moving
           860         it docks, and the motor comes up
    LOOP   408.163 ms, repeating for as long as the library takes
    TAIL   0           the motor spins down
           680         the drive takes the disc: the clunk
           720         the chord, under the wordmark washing in

The loop is one revolution of the disc, and everything in it is pitched so
that it is: the motor is G2 at exactly 40 cycles per loop, its partials at
80, 120 and 160, and the tick is the printed label passing the head once a
turn - G3, an octave up. Nothing beats against anything.

Because the loop is a whole revolution, the tail always begins at the same
angle, which is what lets the print land behind the slot however long the
load ran. The spin is free; the head and tail are fixed.

G is the dominant of C, so the whole loading section sits unresolved and the
chord at the end is the resolution of it - a load of any length lands on a
cadence.
"""
import math
import os
import random
import struct
import sys
import wave

RATE = 48000

# One revolution, and the motor's fundamental: 98 Hz x 0.4081632 s = exactly
# 40 cycles, so the loop joins itself without a click.
LOOP_SAMPLES = 19592
LOOP_MS = LOOP_SAMPLES * 1000.0 / RATE   # 408.163 ms
HEAD_MS = 860.0                          # the disc fades in, travels, docks
TAIL_SPINDOWN_MS = 680.0
TAIL_SEAT_MS = 680.0
TAIL_CHORD_MS = 720.0

# from Hz, to Hz, glide ms, length ms, gain, decay ms, noise gain, start ms,
# rise ms, harmonic, and optionally noise decay ms and noise smoothing
# (0 = the old defaults, 2.0 ms and 0.25).
SOUNDS = {
    'move': [(523, 523, 1, 40, 0.28, 21, 0.0, 0, 4.4, 0.01)],
    'select': [(481, 481, 1, 45, 0.28, 12.5, 0.0, 0, 4.4, 0.01)],
    'back': [(187, 187, 1, 215, 0.78, 18, 0.0, 0, 4.4, 0.01),   # the bump
             (187, 187, 1, 215, 0.34, 110, 0.0, 0, 4.4, 0.01)],  # its long, quiet tail

    # The tick, measured off the Steam Deck recording: four partials on one
    # note at 1, 0.45, 0.28, 0.2, and no attack at all - it swells to its
    # peak over 24 ms and is gone by 52, which is what keeps it soft however
    # often it repeats. Now the label passing the read head, once a turn.
    'tick': [(196.00, 196.00, 1, 52, 0.300, 90, 0.0, 0, 24.0, 0.0),
             (392.00, 392.00, 1, 52, 0.135, 90, 0.0, 0, 24.0, 0.0),
             (588.00, 588.00, 1, 52, 0.084, 90, 0.0, 0, 24.0, 0.0),
             (784.00, 784.00, 1, 52, 0.060, 90, 0.0, 0, 24.0, 0.0)],

    # HEAD. The disc arriving: a soft swell as it fades in, air as it
    # travels, a low contact as it meets the slot, then the motor coming up
    # to speed underneath - which runs past the head's end so the loop
    # starting beneath it is not heard joining.
    'head': [(392.00, 523.25, 170, 430, 0.105, 300, 0.0, 0, 180.0, 0.10),
             (196.00, 261.63, 170, 430, 0.080, 320, 0.0, 0, 200.0, 0.08),
             (150.00, 92.00, 190, 450, 0.070, 420, 0.26, 340, 150.0, 0.20, 300.0, 0.05),
             (150.00, 96.00, 16, 180, 0.200, 55, 0.05, 820, 2.0, 0.25),
             (31.00, 98.00, 46, 230, 0.150, 9e8, 0.0, 740, 22.0, 0.34)],

    # LOOP, one revolution. The motor is four partials of G2 that neither
    # rise nor decay, so the only thing that changes across the loop is the
    # tick - see spin_loop(), which renders 8 ms long and cuts back, since
    # the renderer fades every tap out at its end.
    'motor': [(98.00, 98.00, 1, LOOP_MS + 8, 0.150, 9e8, 0.0, 0, 0.0, 0.34),
              (196.00, 196.00, 1, LOOP_MS + 8, 0.048, 9e8, 0.0, 0, 0.0, 0.0),
              (294.00, 294.00, 1, LOOP_MS + 8, 0.024, 9e8, 0.0, 0, 0.0, 0.0),
              (392.00, 392.00, 1, LOOP_MS + 8, 0.012, 9e8, 0.0, 0, 0.0, 0.0)],

    # TAIL. The motor falling away, one last tick as the label goes by, the
    # drive taking the disc, and the chord that resolves the G.
    # The first tap rises over 30 ms: that is the crossfade the spin loop is
    # faded out under on the console, since stopping it dead would click.
    'tail': [(98.00, 33.00, 300, 700, 0.165, 900, 0.0, 0, 30.0, 0.34),
             (196.00, 160.00, 320, 300, 0.120, 150, 0.0, 120, 24.0, 0.0),

             (228.00, 70.00, 11, 280, 0.520, 46, 0.16, 680, 1.5, 0.30, 34.0, 0.18),
             (1760.00, 1180.00, 8, 70, 0.070, 13, 0.05, 680, 1.0, 0.0, 9.0, 0.6),

             (130.81, 130.81, 1, 1340, 0.260, 620, 0.0, 720, 12.0, 0.22),
             (196.00, 196.00, 1, 1340, 0.140, 640, 0.0, 720, 16.0, 0.14),
             (329.63, 329.63, 1, 1300, 0.115, 660, 0.0, 736, 20.0, 0.12),
             (523.25, 523.25, 1, 1260, 0.085, 600, 0.0, 752, 24.0, 0.10),
             (783.99, 783.99, 1, 760, 0.048, 300, 0.0, 900, 10.0, 0.06)],
}


def render(taps):
    taps = [tuple(t) + (0.0,) * (12 - len(t)) for t in taps]
    end = max(t[7] + t[3] for t in taps)
    out = [0.0] * int(end * RATE / 1000)
    rng = random.Random(1)
    for (from_hz, to_hz, glide, ms, gain, decay, noise, start, rise_ms,
         harmonic, noise_decay, noise_cut) in taps:
        noise_decay = noise_decay or 2.0
        noise_cut = noise_cut or 0.25
        n, first = int(ms * RATE / 1000), int(start * RATE / 1000)
        rise, fall = int(RATE * rise_ms / 1000), RATE * 8 // 1000
        phase = smooth = 0.0
        for i in range(n):
            t_ms = i * 1000.0 / RATE
            hz = to_hz + (from_hz - to_hz) * math.exp(-t_ms / glide)
            phase += 2 * math.pi * hz / RATE
            shape = 1.0
            if i < rise:
                shape = i / rise
            if i > n - fall:
                shape *= (n - i) / fall
            env = gain * math.exp(-t_ms / decay) * shape
            smooth += (rng.uniform(-1, 1) - smooth) * noise_cut
            puff = noise * math.exp(-t_ms / noise_decay) * shape * smooth
            out[first + i] += env * (math.sin(phase) + harmonic * math.sin(2 * phase)) + puff
    return out


def spin_loop():
    """One revolution: the motor, cut back to the exact loop length so it
    joins itself, with the tick laid over it."""
    motor = render(SOUNDS['motor'])[:LOOP_SAMPLES]
    for i, v in enumerate(render(SOUNDS['tick'])):
        if i < LOOP_SAMPLES:
            motor[i] += v
    return motor


def mix_at(out, samples, start_ms):
    first = int(start_ms * RATE / 1000)
    for i, s in enumerate(samples):
        if first + i < len(out):
            out[first + i] += s


def startup_screen(turns):
    """The whole screen: head, `turns` revolutions of the loop, then tail."""
    head, loop, tail = render(SOUNDS['head']), spin_loop(), render(SOUNDS['tail'])
    spin_ms = turns * LOOP_MS
    out = [0.0] * (int((HEAD_MS + spin_ms) * RATE / 1000) + len(tail))
    mix_at(out, head, 0)
    for n in range(turns):
        mix_at(out, loop, HEAD_MS + n * LOOP_MS)
    mix_at(out, tail, HEAD_MS + spin_ms)
    return out


def write(path, samples):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', max(-32767, min(32767, int(s * 32767)))) for s in samples))


def report(name, samples):
    peak = max(abs(s) for s in samples)
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    print('%-16s %5d ms  peak %.2f  rms %.3f%s' %
          (name, len(samples) * 1000 // RATE, peak, rms, '  CLIPS' if peak > 1.0 else ''))


if __name__ == '__main__':
    turns = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    folder = os.path.join(os.path.dirname(__file__), '..', 'docs', 'sound-preview')
    os.makedirs(folder, exist_ok=True)
    for name in sorted(SOUNDS):
        samples = render(SOUNDS[name])
        write(os.path.join(folder, name + '.wav'), samples)
        report(name, samples)
    loop = spin_loop()
    write(os.path.join(folder, 'loop.wav'), loop)
    report('loop (1 turn)', loop)
    for label, n in (('startup-warm', 2), ('startup-long', 7)):
        whole = startup_screen(n)
        write(os.path.join(folder, label + '.wav'), whole)
        report(label, whole)
    print('wrote', os.path.abspath(folder))

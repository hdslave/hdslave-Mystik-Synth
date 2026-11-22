# Mystik: design

A west-coast hand drum for the MPC Sample: one drum (a membrane and a complex oscillator) struck through a modelled
vactrol low-pass gate, played by its own sequencer, through a few very cheap effects, with its own page on the screen.
CLAP id `com.hdslave.mystik`, listed as "hdslave Mystik", state tag `MYST`, binary `build/Mystik.clap`, C++ namespace
`mystik`.

## Voice (one drum, mono, run at twice the host's rate)

```
strike ──► hand exciter (raised-cosine click + noise burst) ──► membrane: 8 modes ──┐
   │                         └──── Snap: the burst straight into the gate ─────────┐│
   │      complex oscillator: principal sine, phase-modulated by a modulator sine  ││
   │      (FM Index, FM Ratio), plus Feedback from the gate's output ─────────────┐││
   │                                                                Source mix ◄──┘│┘
   │                                                        wavefolder (Timbre, Symmetry)
   └──► vactrol LED ──► light x(t) ──► LDR Rf ──► 292 gate (VCA ... Combo ... Lowpass) ◄─┘
                                                     │
                     half-band decimator (47 taps) ◄─┘──► Drive ─► Crush ─► Echo ─► Space ─► Volume ─► L/R
```

- Complex oscillator: principal sine, its phase moved by `Index * (1 + Bloom * light) * sin(modulator)` (phase
  modulation, through-zero FM) and by `Feedback * gate output` (the last sample). Modulator at FM Ratio times the
  principal: 1/2 1 3/2 2 5/2 3 4 5 7 and the inharmonic 1.414 and the membrane's 1.594 2.136 2.296 2.653 2.918 3.156.
- Membrane: 8 modes, each a coupled-form oscillator (a rotation by the mode's angle times its decay a sample: the
  amplitude stays put when the pitch moves, so bends and slides are clean). Bodies (numbers fixed forever, append
  only): Tabla (harmonic, a loaded membrane), Conga (ideal membrane, Bessel ratios), Frame (Bessel, bright and long),
  Udu (clay pot: a strong low mode whose pitch rises after the strike), Slit (wooden log: free bar ratios
  1, 2.756, 5.404, 8.933), Handpan (steel: 1, 2, 3 plus shell modes), Bowl (singing bowl doublets that beat), Bell
  (agogo-like), Goblet (0.2: a doumbek or djembe, the goblet's Helmholtz air at 0.42 under the head; the conga has
  its open shell's air at 0.47 too). Position moves the strike from the centre (round modes) to the edge (the rest).
  Ring sets the fundamental's decay; higher modes decay faster by the body's table.
- Hand exciter: a raised-cosine pulse of area `force`, its width the stroke's times `1.4 - 0.6 force` (a harder hand
  is shorter, so it reaches higher modes), and a decaying noise burst (force^1.5): broadband into the skin, and through
  a band-pass at the stroke's crack frequency (450 Hz for a bass, 1.6 kHz open, 2.8 kHz slap; Q 1.3) straight into the
  gate by Snap. A hand landing on a head that still rings damps it first by `contact * (0.4 + 0.6 force)` (a mute
  most, a ghost least): flams and rolls keep their main hits. Human (0.2) varies each hit a little (force, place,
  contact, bend, every mode's tuning), by a hash of the hit count.
- Strokes (per step, and from keys in Strokes mode): Open, Slap (edge, bright, short, its fundamental damped by the
  fingers left on), Bass (centre, soft, long, a deeper bend, its upper modes damped by the palm), Mute (damped modes
  and gate), Ghost (soft), Flam (a grace hit, the main one 22 ms after), Roll (four hits that speed up and fade),
  Press (the head pressed after the hit: the pitch rises, a tabla's bayan).
- Pitch: Tune + the step + MIDI transposition + Glide (slides) + Bend (tension: the pitch starts higher and falls as
  the swing does, its time the fundamental's T60 / 13.8 (amplitude squared); deeper for harder hits; negative rises,
  as an udu) + Drift (a smoothed random walk: the "woggle").
- Wavefolder: `sin(g * x + b) - sin(b)`, g = 1 + 11 * clamp(Timbre + Bloom * light)^2, b = Symmetry * pi/2. It folds
  the oscillator and the membrane alike.

## The vactrol low-pass gate

From Parker and D'Angelo, "A Digital Model of the Buchla Lowpass-Gate" (DAFx-13), the canonical 292:

- LED current to LDR resistance: `Rf = A / If^1.4 + B`, A = 3.464 ohm A^1.4, B = 1136.212 ohm. If = x * 40 mA for
  the light x (no 10 uA floor: in the dark Rf grows without bound, so a closed gate is silent, not a leak).
- Light: a one-pole on the LED drive whose time constant switches with the direction (paper: about 12 ms up,
  250 ms down, faster when bright). Decay scales the fall (25 ms to 2.5 s at the knob's ends, 250 ms at 0.5 of
  a log taper); Memory adds a light-history state h so a gate hit often falls slower:
  `tau_fall * min(6, 1 + 12 * Memory * h)` with h following the light over 0.5 s.
- LED drive: a strike is a pulse of Strike's length (0.5 to 30 ms) at 0.4 times the hit's force (a ping lights the
  vactrol only partly: a default one to about 0.08 of full, a gate near 6 kHz, so it closes on the note in a few
  hundred ms); Hold keeps `force * Hold * 0.06` on while the step's gate is held (Gate; ties and slides hold it
  through); Open adds a constant (0 to 0.1); an accent multiplies the drive by `1 + Accent * (0.6 + 0.8 * charge)`,
  the accent capacitor gaining half of what it lacks at each accent and draining with a 300 ms time constant
  (consecutive accents grow brighter, as a 303's do).
- Velocity: the vactrol alone barely quiets a low tone in Combo, so the hit's force also sets the level into the gate
  (`0.2 + 0.8 * force`, after the fold, which would even a softer hand out), eased over a control tick.
- Audio path: `H(s) = 1 / (a1 + a2 s + a3 s^2)`, `a1 = 1 + 2 Rf/Ra`,
  `a2 = Rf (2 C1 + C2 - C3 (a - 1) + (C2 + C3) Rf / Ra)`, `a3 = Rf^2 C1 (C2 + C3)`; C1 = 1 nF, C2 = 220 pF.
  Gate Mode morphs: 0 VCA (Ra = 5 kohm, +4.5 dB), 0.5 Combo (Ra = 5 Mohm, C3 = 0), 1 Lowpass (C3 = 4.7 nF).
  Resonance brings in the Lowpass mode's feedback capacitor as it rises (C3 = 4.7 nF * max(lowpass share,
  Resonance)) and its feedback amplifier `a = 1 + Resonance * (0.985 * amax - 1)`,
  `amax = (2 C1 Ra + (C2 + C3)(Ra + Rf)) / (C3 Ra)`: at 0 the gate is the 292 as built.
- Realised as a trapezoidal state-variable filter, its coefficients worked out every 8 inner samples: real poles
  (Q <= 0.5) are prewarped one by one, each held under 0.49 of the inner rate, then g = sqrt(g1 g2),
  k = (g1 + g2) / g; complex poles take g = tan(w0 T / 2) and k = 1/Q. The two meet at Q = 0.5. The DC gain
  `Ra / (Ra + 2 Rf)` scales the output. The band-pass state is soft-limited, so a resonant gate rings like a
  pinged filter but stays bounded.

## Sequencer

On or off (Sequencer). With it on and the host playing, a key transposes the pattern (from C3, latched); stopped,
keys play the drum. With it off keys play the drum: Notes = Pitch (key sets the pitch, velocity >= 100 accents,
overlapping keys slide), Strokes (keys 36-43 play the eight strokes at Tune, 44-51 an octave up), Steps (keys
36-51 play steps 1-16).

The clock is Anvil's, unchanged in behaviour: a step on each multiple of Rate from the song's top, MPC swing,
clockSlack 1e-3 frames, relocate() for jumps and for a loop the host tells a block late, regrid on a Rate change,
tempo held to 1-1000 BPM, events at frame 0 before the transport.

Modes:

- Pattern: the 16 steps over Length, by Direction (Forward, Backward, Ping-Pong, Brownian, Random), Rotate shifts
  where it starts. Density 50 % plays it as written; lower drops hits, higher fills rests with ghost notes.
  Mutate moves some steps by a scale degree on each pass (a fresh choice each pass, the same for the same place).
- Euclid: Density * Length pulses spread evenly (Bjorklund by Bresenham), Rotate turns them, Accents spreads accents
  over the pulses; each pulse plays the programmed step at its place.
- Turing: a shift register of Length bits; the bit going round is flipped with probability Mutate / 2 (0 locks the
  loop). Its bits give the pitch (Range), the gate (Density), accents (Accents) and slides.
- Automaton: an elementary cellular automaton (Rule 0-255) on a ring of Length cells, a generation a pass; the first
  from Seed at Density; live cells play their steps; cells born this pass may accent. A dead ring is reseeded.
- Bounce: a ball dropped each Length steps: Density sets the bounces, Curve how fast they shrink; each bounce plays
  the next step, softer.
- Rhythm: a traditional timeline with its strokes (tresillo, cinquillo, son and rumba clave, bossa, standard bell,
  tumbao, maqsum, baladi, saidi, malfuf, teental, keherwa, dadra, rupak); pitches from the steps.

Per step (parameters, so K1-K3 can bind them): Pitch (scale degree -21..21), Trig (Rest, Hit, Accent, Slide,
Acc+Slide), Stroke, Chance. Scales: Chromatic, Major, Minor, Dorian, Phrygian, Hijaz, Bhairav, Pent Minor,
Pent Major, Hirajoshi, Pelog, Slendro (Slendro in five equal steps of 240 cents). Randomness is a hash of the clock
number, Seed and a salt: the same place in the song plays the same way. Regenerate picks a new Seed.

## Effects (after the decimator, at the host's rate; each skipped while it is off and silent)

- Drive: `tanh(x * (1 + 7 d^2))` made up.
- Crush: sample-and-hold rate (to about 1.5 kHz) and bits (to 3) from one knob.
- Echo: ping-pong, Echo Time synced to the tempo, Repeats up to 1.05 with Absorb (a low-pass) and a soft clip in
  the loop (exactly linear below 0.75, so repeats are not squeezed).
- Space: a spring-ish tank: per side a feedback delay through a chain of stretched allpasses (dispersion: the
  chirp), Space Decay sets the feedback.

## Parameter ids (fixed forever; append only)

| id | name | | id | name |
|---|---|---|---|---|
| 0 | Tune | | 26 | Echo Time |
| 1 | Source | | 27 | Repeats |
| 2 | Body | | 28 | Absorb |
| 3 | Ring | | 29 | Space |
| 4 | Position | | 30 | Space Decay |
| 5 | Snap | | 31 | Volume |
| 6 | FM Index | | 32 | Sequencer |
| 7 | FM Ratio | | 33 | Seq Mode |
| 8 | Feedback | | 34 | Rate |
| 9 | Timbre | | 35 | Length |
| 10 | Symmetry | | 36 | Direction |
| 11 | Bloom | | 37 | Swing |
| 12 | Bend | | 38 | Gate |
| 13 | Drift | | 39 | Scale |
| 14 | Gate Mode | | 40 | Density |
| 15 | Resonance | | 41 | Rotate |
| 16 | Strike | | 42 | Mutate |
| 17 | Decay | | 43 | Range |
| 18 | Open | | 44 | Accents |
| 19 | Hold | | 45 | Rule |
| 20 | Memory | | 46 | Rhythm |
| 21 | Accent | | 47 | Curve |
| 22 | Glide | | 48 | Seed |
| 23 | Drive | | 49 | Regenerate |
| 24 | Crush | | 50 | Notes |
| 25 | Echo | | 51-66 | Pitch 1-16 |
| | | | 67-82 | Trig 1-16 |
| | | | 83-98 | Stroke 1-16 |
| | | | 99-114 | Chance 1-16 |
| | | | 115 | Human (0.2) |

## Screen page

The plug-in draws at 320 × 218 beneath Melodee’s soft keys. A chrome Mystik wordmark and a seven-position page
indicator sit above every page. Acid green marks the active row or playing step. The cyan outline on SEQ follows
the editing cursor independently of playback.

Minus and Plus move through seven pages. PLAY surrounds a chrome smiley with the active step ring. Its enamel
brightens with the gate’s light, and a strike sends a ripple out to the steps. The scope shows the live voice.
The wheel selects a trio for K1–K3, Push toggles the sequencer, and Shift + Push regenerates the pattern.
SEQ groups the 16 lanes in fours. The wheel picks a step, Push cycles its trigger, and K1–K3 set pitch, stroke,
and chance. SKIN shows the membrane modes, OSC the folded waveform, GATE the current response and light history,
FX the signal chain, and MODE the active sequencer algorithm. On these pages the wheel moves the cursor, Push
edits, and K1–K3 hold the selected row and the next two. Shift shows the control legend.

The chrome art is embedded at native resolution. PLAY copies the static plate and relights the enamel spans on
each animation frame. The renderer compares regions before sending changed pixels to the host. The artwork can
be regenerated with `tools/rave_art.py` using Pillow and aggdraw.

## Cost targets (callgrind, 128-frame blocks at 48 kHz)

Worst (everything on, notes dense) well under 150k instructions a block; a still, silent voice near nothing.

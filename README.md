# Fork Module for Schwung

MIDI channel splitter and router module for Ableton Move, built for Schwung.

## Features

Fork is a chainable MIDI FX module (`midi_fx`) for Schwung. It acts as a 1-to-2 or 1-to-3 MIDI channel splitter. 

Input MIDI notes are split by pitch at two configurable split octaves:
- Notes below both split points are kept on the track (emitted further), adjusted by the main octave transpose.
- Notes falling into the Split 1 or Split 2 ranges have their octave transposed and MIDI channel adjusted, and are exported via named Unix pipes (FIFOs) under `/data/UserData/schwung/`.
- Global stream controllers (Control Change, Pitch Bend, Program Change, Channel Pressure) are automatically forwarded to all active splits (adjusting the channel to match) so that target synthesizers receive all expressive controller data without loss.
- Active note tracking prevents stuck notes on split destinations if parameters are adjusted while keys are held.
- In **Receiver** mode, Fork reads from a selected Unix pipe and replays the MIDI stream as-is on the track.

## Installation

### Manual Installation

```bash
./scripts/build.sh
./scripts/install.sh
```

## Usage

1. Insert **Fork** on the main MIDI track in Splitter mode.
2. Configure **Split 1 Oct** and/or **Split 2 Oct** boundaries (e.g. C3, C4).
3. Set target MIDI channels (relative offsets like `+4` or absolute channels like `10`) and target named pipes (e.g. `midifork1`).
4. Insert another **Fork** instance on a second MIDI track, set its **Mode** to `Receiver`, and select the matching pipe (e.g. `midifork1`).
5. Play notes on the main track. The split-off notes will play the second track's synth in real time!

## Gotchas

* Schwung and Move tracks share some settings, such as volume or mute flags - keep that in mind
if something doesn't work.


## Parameters

In Shadow UI, parameters are organized into categories:

### Global

| Parameter | What it does |
|---------|--------|
| `mode` | Selects operating mode: `splitter` or `receiver`. |
| `main_oct_trans` | Octave transposition for the main (lowest) notes (`-4` to `+4`). |
| `recv_pipe_select` | Named pipe to read from in receiver mode (`midifork1-8` or `custom`). |

### Split 1

| Parameter | What it does |
|---------|--------|
| `split_oct_1` | Boundary octave for Split 1 (`off` or `C-1` to `C8`). |
| `split_1_oct_trans` | Octave transposition for Split 1 notes (`-4` to `+4`). |
| `split_1_chan` | MIDI channel for Split 1 (`+0` to `+15` or absolute `1` to `16`). |
| `pipe_1_select` | Named pipe to send Split 1 data (`midifork1-8` or `custom`). |

### Split 2

| Parameter | What it does |
|---------|--------|
| `split_oct_2` | Boundary octave for Split 2 (`off` or `C-1` to `C8`). |
| `split_2_oct_trans` | Octave transposition for Split 2 notes (`-4` to `+4`). |
| `split_2_chan` | MIDI channel for Split 2 (`+0` to `+15` or absolute `1` to `16`). |
| `pipe_2_select` | Named pipe to send Split 2 data (`midifork1-8` or `custom`). |

## Building from Source

```bash
./scripts/build.sh
```

The build script outputs:
- `dist/fork/`
- `dist/fork-module.tar.gz`

## Credits

- Schwung framework and host APIs: Charles Vestal and contributors
- Fork implementation: move-anything-superarp project contributors

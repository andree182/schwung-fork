# Fork Module for Schwung

MIDI channel splitter and router module for Ableton Move, built for Schwung.

## Features

Fork is a chainable MIDI FX module (`midi_fx`) for Schwung. It acts as a 1-to-2 or 1-to-3 MIDI channel splitter. 

Input MIDI notes are split by pitch at two configurable split octaves:
- Notes below both split points are kept on the track (emitted further), adjusted by the global `transpose` parameter.
- Notes falling into the Split 1 or Split 2 ranges have their MIDI channel adjusted, and are either exported via named Unix pipes (FIFOs) under `/data/UserData/schwung/` or forwarded directly on the current track (if pipe is set to `off`) for routing to external MIDI hardware.
- Global stream controllers (Control Change, Pitch Bend, Program Change, Channel Pressure) are automatically forwarded to all active splits (adjusting the channel to match) via their respective pipe or track output.
- Active note tracking prevents stuck notes on split destinations if parameters are adjusted while keys are held.
- In **Receiver** mode, Fork reads from a selected Unix pipe and replays the MIDI stream, adjusted by the global `transpose` parameter, on the track.

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
if something doesn't seem to work.
* On that note, Schwung has also aux independent controls of volume/mute/solo, use that
to adjust relative volumes.
* More than 5 parallel tracks seem to easily cause stutter (even though the CPU use
may not be 100%)

## Parameters

In Shadow UI, parameters are organized into categories:

### Global

| Parameter | What it does |
|---------|--------|
| `mode` | Selects operating mode: `splitter` or `receiver`. |
| `transpose` | Transposition for main/received notes (`-10` to `10` octaves). |
| `recv_pipe_select` | Named pipe to read from in receiver mode (`1` to `8`). |

### Split 1

| Parameter | What it does |
|---------|--------|
| `split_oct_1` | Boundary octave for Split 1 (`off` or `C-1` to `C8`). |
| `split_1_chan` | MIDI channel for Split 1 (absolute `1` to `16`). |
| `pipe_1_select` | Named pipe to send Split 1 data (`1` to `8`, or `off` to route to current track). |

### Split 2

| Parameter | What it does |
|---------|--------|
| `split_oct_2` | Boundary octave for Split 2 (`off` or `C-1` to `C8`). |
| `split_2_chan` | MIDI channel for Split 2 (absolute `1` to `16`). |
| `pipe_2_select` | Named pipe to send Split 2 data (`1` to `8`, or `off` to route to current track). |

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

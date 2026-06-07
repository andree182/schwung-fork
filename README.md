# Fork Module for Schwung

MIDI channel splitter and router module for Ableton Move, built for Schwung.

Do you want to transform Move into 7 synth tracks (with limitations) or up to
16 MIDI tracks, or some combination of the above? You are at the right place!

## Features

Fork is a chainable MIDI FX module (`midi_fx`) for Schwung. It acts as a 1-to-2, 1-to-3, or 1-to-4 MIDI channel splitter. 

Input MIDI notes are split by pitch at three configurable split octaves:
- Notes below all split points are kept on the track (emitted further), no other changes are applied.
- Notes falling into the Split 1, Split 2, or Split 3 ranges have their MIDI channel adjusted, and are either exported via named Unix pipes (FIFOs) under `/data/UserData/schwung/` or forwarded directly on the current track (if pipe is set to `off`) for routing to external MIDI hardware.
- Global stream controllers (Control Change, Pitch Bend, Program Change, Channel Pressure) are automatically forwarded to all active splits (adjusting the channel to match) via their respective pipe or track output.
- Active note tracking prevents stuck notes on split destinations if parameters are adjusted while keys are held.
- In **Receiver** mode, Fork reads from a selected Unix pipe and replays the MIDI stream, adjusted by the global `transpose` parameter, on the track.

## Conceptual Routing Layout

Here is how Fork can be set up to split and redirect a single MIDI input track (e.g. Track 1) into three separate target Schwung tracks (Tracks 2, 3, and 4), plus:

```
                            +-----------------------------------+
                            |    Track 1: Normal Move Track     |
                            |    (MIDI Input -> Fork (Split))   |
                            +-----------------------------------+
                             /     /             |             \
                            /   (Split 1)      (Split 2)     (Split 3)
   [Track 1 Chain] --------/       |              |              |
                                   v              v              v
                            [Re-channel]   [Re-channel]   [Re-channel]
                                   |              |              |
                                   v              v              v
                                 [ 2 ]          [ 3 ]          [ 4 ]  <-- Local Unix Pipes
                                   |              |              |
                                   v              v              v
                            +-------------+ +-------------+ +-------------+
                            |  Track 2:   | |  Track 3:   | |  Track 4:   |
                            | Fork (Recv) | | Fork (Recv) | | Fork (Recv) | <-- Schwung Tracks
                            +-------------+ +-------------+ +-------------+
                                   |              |              |
                                   v              v              v
                            +-------------+ +-------------+ +-------------+
                            | Local Synth | | Local Synth | | Local Synth |
                            +-------------+ +-------------+ +-------------+
                                   |              |              |
                                   v              v              v
                            ==============================================
                                          Stereo Audio Output Mix
                            ==============================================
```

## Installation

### Manual Installation

```bash
./scripts/build.sh
./scripts/install.sh
```

## Usage

1. Insert **Fork** on the main MIDI track in Splitter mode.
2. Configure **Split 1 Oct** or further boundaries (e.g. C4).
3. Set target MIDI channels and target pipe (there are 4 available, most likely you'll want to choose the same number as Schwung track number).
4. Insert another **Fork** instance on a second MIDI track, set its **Mode** to `Receiver`, and select the matching pipe number.
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
| `main_oct_trans` | Octave transposition for the main (lowest) notes (`-10` to `+10`). |
| `recv_pipe_select` | Pipe index to read from in receiver mode (`1` to `4`). |
| `recv_chan` | Target MIDI channel mapping in receiver mode (`track` uses active slot track channel, `as_is`   leaves channel unchanged, or absolute channel `1` to `16`). |
| `fallthrough` | Receiver mode fallthrough (`off` or `on`, default is `off`). When enabled, merges incoming MIDI track events (from the pads or sequencer) with the stream received from the pipe. |

### Split 1/2/3

| Parameter | What it does |
|---------|--------|
| `split_oct_1` | Boundary octave for Split 1 (`off` or `C1` to `C10`). |
| `split_1_oct_trans` | Octave transposition for Split 1 notes (`-10` to `+10`). |
| `split_1_chan` | MIDI channel for Split 1 (`+0` to `+15` relative offset, or absolute `1` to `16`). |
| `pipe_1_select` | Named pipe index to send Split 1 data (`off` or `1` to `4`). |

Split 2/3 work the same, boundary octave must be always higher than the split before.

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

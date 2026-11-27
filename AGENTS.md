# Notes for coding agents

**hdslave Mystik** is a west-coast hand drum built on a low-pass gate model, with a six-mode sequencer and its own page: a CLAP plug-in for the Akai MPC Sample. It is made to run in hdslaveOS, the hdslave fork of Kerem Kiliç's Melodee. Its CLAP id is `com.hdslave.mystik`, and it builds to `build/Mystik.clap`.

## Read the workspace guide first

On the author's machine, this repository sits in one folder beside the other hdslave projects. That folder's `../AGENTS.md` and `../Workflow/` hold the shared workflow:
- building in the Linux VM;
- what hdslaveOS does to plug-ins;
- screen pages;
- reviews;
- publishing, the hub repository and staging for the SD card.

Read `../AGENTS.md`, then `../Workflow/plugins.md` and `../Workflow/host-facts.md`, before you change anything. The rules below hold either way.

## Build and test

- `./build.sh` builds the plug-in in a Linux arm64 container (image `melodee-builder`, Colima VM `melodee`). It runs `make all test`: the host tests in `tests/host.cpp` and the sound's fingerprint. Pass make targets to run others, for example `./build.sh sanitize` (ASan and UBSan over the host tests).
- `tools/bench.sh` counts the instructions of a 128-frame block with callgrind. A block is about 4.8 M cycles on one of the MPC's Cortex-A55 cores.
- Check the build's own exit code: `./build.sh | grep …` hides a failure.

## Rules

- **Fixed for saved projects.** Parameter ids, preset keys and choice orders never change once released, because saved projects and presets refer to them.
  - Add new ones at the end.
  - Ranges stay as they are too: the host stores bindings normalized to the range.
- **The sound is held to the bit** by `tests/fingerprint.txt`.
  - A refactor or speed-up must leave every hash unchanged. With `-ffp-contract=fast`, even reshaping a loop can change the last bit, so check after every edit.
  - Only a change meant to change the sound runs `make fingerprint-update`, and its commit says which hashes moved and why.
- **Real-time and determinism.** The output is the same at every block size. No allocation, locks or file I/O on the audio thread.
- **Notes** must survive how hdslaveOS sends them:
  - every MIDI event of a block arrives at frame 0;
  - a re-pressed key arrives as on, on, off;
  - CC 120 and 123 arrive at 0 on every channel when the host silences.
- **Commits** are made as `hdslave <111106843+hdslave@users.noreply.github.com>`, with Conventional Commits. Never push, publish or stage for the card without the owner's word.
- **Other agents' work.** Others may be working here. Check `git status` and `git worktree list`, and never edit, commit or reset files you did not change.
- **Names and art.** No trademarked names in the plug-in's names, presets, UI text or art. Art is original; fonts are OFL, with their licences in `licenses/`.
- **Read the README** before you change the sound. It lists the sources, the inferred values and the deliberate deviations. Keep it current: version, costs, and "not yet run on the MPC" until it has been.

## This plug-in

- The gate follows a published vactrol gate model, realised as a TPT state-variable filter. The host tests hold it to the analog response within 0.35 dB.
- Per-tick state is relative to the strike. The idle fast path counts skipped control ticks exactly; keep it that way, or block sizes start to differ.
- The factory presets keep the 0.1 values on purpose, Volumes included. Don't rebalance them unless asked.
- **Pads.** With Sequencer On (playing or stopped), keys 36-51 play steps 1-16 as Notes: Steps does, without moving the sequence or the transposition. Other keys keep their old roles, and Sequencer Off gives the Notes keyboard back. A pad waits for its frame's audio (`Engine::playPad` in `render`): the louder of two on one frame plays once, and a step the sequence plays on that frame wins. The `pads` tests in `tests/host.cpp` hold it.
- **Hold** follows the key or step pad that struck the drum last (`Engine::owner`): an older key still held ends nothing. A step that plays drops every older gate end still to come, so none cuts its Hold. A full queue of hits drops a hit, never a gate end. A step's clock may be negative (a host's pre-roll), so what is no step is marked by `noClock`, `padClock` and `auditionClock`, never by a sign. The step a late-told wrap decides again plays in `transport()`, before that frame's notes and presses: it began on the wrap's frame, in the block before. A step drops a pad's and the audition's flam and roll hits still to come. The `holdplay`, `holdlast`, `fullqueue`, `gateends` and `clockedges` tests hold it, and the desktop copy has the same rules in `mac-clap/tests/hold_test.cpp` (run by `mac-clap/build-macos.sh`).

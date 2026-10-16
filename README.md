# Super Mario Land 2 — native Linux C port

A readable C reconstruction of Super Mario Land 2: 6 Golden Coins for Linux.
Gameplay, menus, world navigation, bosses, saves, rendering and sound run as
native code. The executable does not execute Game Boy instructions or include
a CPU emulator.

This repository contains the native source, standalone game assets and build
files. **No ROM is required to build or play.** Tests, saves, reference tooling
and generated build files are excluded.

## Linux download

Download the Linux x86-64 archive from [Releases](https://github.com/blasdecrespo/sml2-native-linux/releases),
extract it, and run `./sml2-native`. The executable includes the game assets.
It requires glibc 2.34 or newer, the X11 and ALSA shared libraries, and an X11
or XWayland display. Use `./sml2-native --mute` to disable audio.

## Build and run

Requirements: a C11 compiler, Make, Python 3, and the X11 and ALSA development
libraries. The window uses X11 or XWayland.

```sh
make
./build/sml2-native
```

The build verifies and embeds `assets/game-data.bin`, which contains the
graphics, maps, music and tables used by the C game. `assets/index.json` maps
those data segments to the internal coordinates used by the translated
readers. Unused data space is zero; the original ROM is not reconstructed.
The resulting executable needs no separate asset files at runtime. Generated
files stay in `build/`, which is ignored by Git.

If your environment overrides the compiler or linker flags, use:

```sh
make CC=cc LDFLAGS=
```

## Controls

| Key | Action |
| --- | --- |
| Arrows | Move / navigate |
| Z | Jump / A |
| X | Run / fireball / B |
| Enter | Start / select / pause |
| Backspace | Select / easy-mode toggle on the file menu |
| Escape | Quit |

Hold Z, X, Backspace and Enter together to reset to the title. Saved progress
survives. While paused in a previously cleared level, Backspace returns to the
world map without spending a life.

The title initially ignores Start, as in the original. On file selection,
wait for Mario to emerge before entering a pipe.

## Options and saves

```sh
./build/sml2-native --scale 4
./build/sml2-native --mute
./build/sml2-native --save /path/to/progress.sav
```

Display scales range from 1 to 8. Windowed runs save to `sml2.sav` by default.
Saves use the original 8192-byte format with three file slots. Writes are
atomic, and the first existing save is retained as a `.bak` file.

## Validation and differences

The standalone asset build passed 73 local checks against the original ROM
and native executable. A fresh source checkout also built without a ROM. One uninterrupted new-file run earned all six golden coins,
defeated all three Wario forms and finished the ending, comparing 119,709 native
calls with the original game using recorded clock inputs. Those tests and
reference captures are not included here.

This is not a cycle-exact hardware recreation. The normal executable uses a
Linux monotonic timer for divider-dependent randomness, and the sound
synthesizer approximates the Game Boy's analogue output. Replaying the full
fixed controller sequence with the live Linux clock did not finish; separate
live-clock routes through stages 10–13, stages 20–23, and the castle/ending
passed. Exact behavior for every input sequence and physical Game Boy hardware
equivalence have not been established.

This is an unofficial reconstruction. Super Mario Land 2 and its original
game assets belong to Nintendo.

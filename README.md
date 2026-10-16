# Super Mario Land 2 — native Linux C port

A readable C reconstruction of Super Mario Land 2: 6 Golden Coins for Linux.
Gameplay, menus, world navigation, bosses, saves, rendering and sound run as
native code. The executable does not execute Game Boy instructions or include
a CPU emulator.

This repository contains the source and build files. It does not include the
ROM, generated assets, binaries, saves, tests or reverse-engineering tools.

## Build and run

Requirements: a C11 compiler, Make, Python 3, and the X11 and ALSA development
libraries. The window uses X11 or XWayland.

Supply your own copy of **Super Mario Land 2 - 6 Golden Coins (USA, Europe)**.
The supported ROM is 524,288 bytes with SHA-256:

```text
5450dce1bd0c073964c374b5b5b5729dce8d00f2e807892c34af32b8bce1392e
```

Place it at `baserom.gb`, or give Make its path:

```sh
make ROM=/path/to/original.gb
./build/sml2-native
```

The build verifies the ROM and embeds its assets and tables locally. The
resulting executable needs no separate ROM file at runtime. Generated files
stay in `build/`, which is ignored by Git.

If your environment overrides the compiler or linker flags, use:

```sh
make CC=cc LDFLAGS= ROM=/path/to/original.gb
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

The development version passed 73 local checks against the original ROM and
native executable. One uninterrupted new-file run earned all six golden coins,
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

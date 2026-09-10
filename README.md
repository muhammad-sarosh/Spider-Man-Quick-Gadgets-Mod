# Quick Gadgets for Marvel's Spider-Man Remastered

Quick Gadgets is an experimental Spider-Man 2-style direct-gadget mod for
Marvel's Spider-Man Remastered 4.0630.0.0. It finds the live
HeroWeaponManager, resolves the weapon ID in the requested wheel slot, calls
the game's own selection routine, and can pulse the native UseGadget action.
The native controller route does not synthesize keyboard input.

The old keyboard-wheel implementation is retained only as an opt-in diagnostic
fallback. It sends synthetic `[`, `]`, and `E` keystrokes, is focus-sensitive,
and is disabled by default because it is not suitable for controller play.

## Current controls

| Shortcut | Gadget |
| --- | --- |
| `Alt + 1` | Web Shooter |
| `Alt + 2` | Impact Web |
| `Alt + 3` | Spider Drone |
| `Alt + 4` | Electric Web |
| `Alt + 5` | Web Bomb |
| `Alt + 6` | Trip Mine |
| `Alt + 7` | Concussive Blast |
| `Alt + 8` | Suspension Matrix |

Alt + 1 through Alt + 8 use the native selector and native fire pulse when
NativeDirectSelect and NativeDirectFire are enabled. These are physical test
triggers only; the mod does not send those keys to the game.

On an XInput-compatible controller, leave the game's Gadget Select Button on
**Hold R1**, then hold LB/L1 and press a face button. The default slot mapping
is configurable in the Controller section:

- A/Cross: slot 5 (Web Bomb)
- B/Circle: slot 4 (Electric Web)
- X/Square: slot 2 (Impact Web)
- Y/Triangle: slot 3 (Spider Drone)

F10 enables or disables the mod.

## Install

1. Install the current community **Spider-Man PC Script Hook** for the exact
   version of your game.
2. Build the package CMake target. Copy QuickGadgets.script and
   QuickGadgets.ini into the game's scripts folder.
3. Launch the game through the Script Hook/community loader.

During development, fully close the game before replacing QuickGadgets.script.
Script Hook documents Insert as an unload/reload command, but does not expose a
documented script shutdown callback; this mod owns a polling worker, so process
restart is the safe way to load a newly compiled DLL.

Only the optional KeyboardWheelFallback needs the game's Previous Gadget,
Next Gadget, and Use Gadget keyboard bindings. Leave that fallback disabled
for native/controller use.

The development build can run a read-only native probe after attach. Once a
save is loaded, findings are written to QuickGadgets.log beside the DLL. The
probe is opt-in:
NativeProbe=1 is required, because some Script Hook builds can crash while
enumerating components. Leave it at 0 for normal play. When diagnosing, set
NativeProbeLevel from 1 through 5 to add one operation at a time: hero pointer,
component count, component names, vtable reads, then named component lookups.

## Configure

Edit `QuickGadgets.ini` beside the script before launching the game. Values are
Windows virtual-key codes; common examples are `18` for Alt, `16` for Shift,
`17` for Ctrl, and `49` through `56` for `1` through `8`.

KeyboardWheelFallback=0 is the default and is the correct setting for the
native/controller route. Set it to 1 only for the legacy keyboard test.
NativeDirectFire=0 keeps native selection but disables the experimental fire
pulse, which is useful for isolating a problem.

The wheel order is the default Remastered order. If a mod changes that order,
reassign the `Slot1`...`Slot8` key values to match its actual order.

When enabled, the legacy fallback anchors the game to the first wheel slot by
sending several Previous Gadget pulses, then advances to the requested gadget
and fires it. This path is intentionally stateless but remains keyboard-only.

RestoreWebShooter currently applies only to the legacy wheel fallback. Native
auto-restore is intentionally deferred until direct selection and firing have
been validated in-game.

## Offline layout verification

Before installing a build, verify that the installed executable still matches
the exact 4.0630.0.0 native layout this mod targets:

```powershell
python tools/verify_game_layout.py `
  "S:\Steam\steamapps\common\Marvel's Spider-Man Remastered\Spider-Man.exe"
```

The verifier checks the executable hash, all called function signatures,
HeroWeaponManager vtable entries, and the embedded input-action names/hashes.
Technical evidence and current limitations are recorded in
`docs/REVERSE_ENGINEERING.md`.

The native controller path clears Attack, Dodge, Jump, and Web Strike through
the engine action system during the gadget pulse. This is an experimental
first pass at preventing face-button leakage; it does not remap the controller
through Steam Input.

## Build

Requires Visual Studio 2022 Build Tools with the Desktop C++ workload and CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target package
```

The package is written to `build/package/QuickGadgets.script`.

## Validation plan

Start with an unlocked slot and verify selection plus firing. If the game
crashes or only selection works, set NativeDirectFire=0 and repeat. Then test
the four controller combinations and note whether the normal face-button
action also occurs. Finally, manually change the wheel selection and repeat;
the native route addresses slots directly and should not depend on current
wheel state.

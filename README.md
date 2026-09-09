# Quick Gadgets for Marvel's Spider-Man Remastered

Quick Gadgets is a keyboard-first Spider-Man 2-style gadget mod. A modifier tap
fires Web Shooter; holding the modifier and pressing a shortcut selects and
fires a specific gadget immediately, without asking you to use the wheel.

This initial release deliberately drives the game's own **Previous Gadget**,
**Next Gadget**, and **Use Gadget** actions. That gives the game responsibility
for the actual ammo, availability, animations, and HUD updates, and avoids
hard-coded executable addresses.

## Default controls

| Shortcut | Gadget |
| --- | --- |
| `Alt` tap | Web Shooter |
| `Alt + 1` | Web Shooter |
| `Alt + 2` | Impact Web |
| `Alt + 3` | Spider Drone |
| `Alt + 4` | Electric Web |
| `Alt + 5` | Web Bomb |
| `Alt + 6` | Trip Mine |
| `Alt + 7` | Concussive Blast |
| `Alt + 8` | Suspension Matrix |

`F10` enables or disables the mod.

## Install

1. Install the current community **Spider-Man PC Script Hook** for the exact
   version of your game.
2. In the game, assign unique keyboard bindings for **Previous Gadget**, **Next
   Gadget**, and **Use Gadget**. The defaults in this project expect `[`, `]`,
   and `E`.
3. Build the `package` CMake target. Copy `QuickGadgets.script` and
   `QuickGadgets.ini` into the game’s `scripts` folder.
4. Launch the game through the Script Hook/community loader.

The current development build also runs a read-only native probe after the
script loads. Once a save is loaded, its findings are printed in the Script
Hook console with a `[QuickGadgets]` prefix. This confirms which live hero
gadget components are available before we call any internal direct-fire
routine; it does not change gameplay by itself.

## Configure

Edit `QuickGadgets.ini` beside the script before launching the game. Values are
Windows virtual-key codes; common examples are `18` for Alt, `16` for Shift,
`17` for Ctrl, and `49` through `56` for `1` through `8`.

The wheel order is the default Remastered order. If a mod changes that order,
reassign the `Slot1`...`Slot8` key values to match its actual order.

Every direct shortcut first anchors the game to the first wheel slot by sending
several Previous Gadget pulses, then advances to the requested gadget and fires
it. This is intentionally stateless: manually opening the wheel or a mission
changing the active gadget cannot desynchronise Quick Gadgets.

`RestoreWebShooter=1` is enabled by default and makes the mod return to Web
Shooter after every non-web gadget use, matching Spider-Man 2’s non-persistent
quick-fire behaviour. Set it to `0` if you want the selected gadget to remain
active after firing.

## Using a controller through Steam Input

The DLL cannot safely consume an XInput face-button event by itself: polling it
would also let Square/Cross/etc. reach the game as attack or dodge. Use a Steam
Input layout that emits keyboard keys instead:

1. Map the controller’s R1 to the mod’s modifier key (`Left Alt`, virtual-key
   `18`).
2. Map Square, Triangle, Circle, and Cross to four unused keyboard keys, for
   example `1`, `2`, `3`, and `4`.
3. Keep those face buttons as keyboard outputs only; do not also bind them as
   gamepad buttons in the same layout.
4. The resulting controls are `R1 + Square = Alt + 1`, `R1 + Triangle = Alt + 2`,
   `R1 + Circle = Alt + 3`, and `R1 + Cross = Alt + 4`.

This is a keyboard translation layer, not native XInput interception. It keeps
the original controller actions from leaking into combat while using the same
tested DLL path.

Native controller interception is still being investigated. The game has no
user-facing Previous/Next Gadget buttons on a controller, so the eventual
Spider-Man 2-style implementation must call the hero gadget component directly
instead of relying on those keyboard actions.

## Build

Requires Visual Studio 2022 Build Tools with the Desktop C++ workload and CMake.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target package
```

The package is written to `build/package/QuickGadgets.script`.

## Validation plan

Test each gadget after it is unlocked, then test after manually changing the
wheel selection. If a game configuration has more than eight entries, increase
`AnchorPreviousPulses` so it reliably reaches Web Shooter before advancing.

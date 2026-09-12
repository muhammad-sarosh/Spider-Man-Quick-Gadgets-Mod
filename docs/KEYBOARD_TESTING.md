# Experimental keyboard shortcuts

This development build adds native keyboard triggers. Public controller support
is unchanged. Keyboard/mouse firing needs in-game validation before release.

1. Close the game. Back up the installed QuickGadgets.dll and QuickGadgets.ini.
2. Replace the DLL with this development build and add the new [Keyboard]
   section from the repository INI to your installed INI. Set Enabled=1 in that
   section. Keep your existing controller/general settings.
3. Launch normally through Steam and enter gameplay. Use keyboard/mouse input
   with the controller disconnected to verify keyboard-only operation.
4. Press F1 once for Impact Web. Confirm the correct projectile, ammo use, and
   automatic return to usable Web Shooter. Repeat F1 several times, releasing
   between presses; hold F1 once to check that it does not auto-repeat.
5. Test F2 Web Bomb, F3 Electric Web, F4 Spider Drone, F5 Concussive Blast,
   F6 Trip Mine, and F7 Suspension Matrix as they become unlocked.
6. Alt-tab to another application and press the shortcut: it must do nothing.
   Holding a shortcut while returning to the game must not fire it.
7. Reconnect the controller and verify the existing D-pad shortcuts still work.

Keys are configurable by readable names in [Keyboard]. Use None to disable one.
Duplicate keys, the mod toggle key (normally F10), and Script Hook's Insert key
are rejected and logged. Do not assign shortcut keys to other in-game actions:
this implementation does not suppress their normal behavior.

The native firing/restore path is shared with controller shortcuts; it does not
send simulated keys or cycle the gadget wheel. General AutoRestoreWebShooter and
RepeatWindowMs apply to both. No XInput device is needed to detect keyboard keys.

Pause/photo/map menu detection is not yet verified. For this first test build,
toggle the mod OFF with F10 before entering a menu, then ON back in gameplay.
Player lookup rejects requests when no hero exists, but a hero may remain loaded
in menus. Do not publish this as fully supported keyboard functionality yet.

If it selects but does not fire, provide scripts/QuickGadgets.log. The key press
is logged separately from the native equipment transition and fire operation.

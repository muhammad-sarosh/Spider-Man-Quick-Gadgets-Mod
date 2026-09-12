# Keyboard and mouse shortcuts

Quick Gadgets supports native keyboard and mouse shortcuts alongside controller
shortcuts. No XInput controller is required when using keyboard and mouse.

Edit `[Keyboard]` and `[KeyboardBindings]` in `scripts/QuickGadgets.ini` while
the game is closed. Restart the game after saving changes.

Keys are configurable by readable names in `[KeyboardBindings]`. Use `None` to disable one.
Duplicate keys, the mod toggle key (normally F10), and Script Hook's Insert key
are rejected and logged. Do not assign shortcut keys to other in-game actions:
this implementation does not suppress their normal behavior.

The native firing/restore path is shared with controller shortcuts; it does not
send simulated keys or cycle the gadget wheel. General AutoRestoreWebShooter and
RepeatWindowMs apply to both. No XInput device is needed to detect keyboard keys.

The game rejects gadget shortcuts in the pause menu, map, Photo Mode, and main
menu. The mod also ignores shortcuts while another application has focus.

If it selects but does not fire, provide scripts/QuickGadgets.log. The key press
is logged separately from the native equipment transition and fire operation.

## Select/double-fire mode

Set `SingleTapSelectDoubleTapFire=1`. One tap selects the gadget without firing
or scheduling automatic restoration. A second tap of the same key inside
`DoubleTapWindowMs` fires it and starts the normal repeat/restore timer. A late
second tap or a tap on another gadget only makes that selection.

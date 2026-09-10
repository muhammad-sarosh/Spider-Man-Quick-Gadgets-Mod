# Native gadget path (Spider-Man.exe 4.0630.0.0)

This file records the static-analysis evidence behind the native implementation.
It is deliberately version-specific. Runtime byte guards must pass before any
of these routines are called.

## Verified executable

- File version: 4.0630.0.0
- SHA-256: E297D4D94F1FFE4FEBF289745E79E7B6FA233A788E7A00F480FC77C55DB81AD1
- Preferred image base: 0x140000000

## HeroWeaponManager

- Base vtable RVA: 0x38B55C8
- Local vtable RVA: 0x38B59B8
- Remote vtable RVA: 0x38B5B98
- Weapon slot table: manager + 0x6C, stride 0x28, eight slots
- Get weapon ID by slot: RVA 0x21634F0
- Resolve weapon record by ID: RVA 0x21633C0
- Low-level active weapon setter: RVA 0x09A5FF0
- Selection wrapper with normal notification/activation work: RVA 0x09A4110

The slot getter computes manager + 0x6C + slot * 0x28 and returns the 32-bit
weapon ID. The low-level setter resolves that ID and deactivates the previous
weapon before activating the new record. QuickGadgets uses the wrapper at
0x09A4110 because it performs the setter plus the normal follow-up work.

## Input action system

HeroWeaponManagerLocal resolves an input-action context from its handle at
manager + 0x78C. The handle resolver is RVA 0x16798F0.

The action value setter is RVA 0x09098C0 with the inferred x64 signature:

    void SetActionValue(void* context, uint32_t actionHash, float value)

This inference is supported by normal game callers that pass a mapped action
hash in EDX and an input value in XMM2. QuickGadgets uses a 1.0 then 0.0 pulse
for the native UseGadget action.

Action hashes use Insomniac's CRC32 implementation with seed 0xEDB88320. The
important values are:

- kUseGadget: 0x9D43C80F
- kAttack: 0x2B24146B
- kDodge: 0x7CA907FC
- kJump: 0xD69724B0
- kWebStrike: 0x775E96E1
- kEquipGadgetLeft: 0x39BC2B52
- kEquipGadgetRight: 0xA08798E7
- kEquipGadgetUp: 0xBE2CCCA6

The function at RVA 0x09A6EB0 was initially suspected to be the use path, but
the immediate 0xBE2CCCA6 proves it handles kEquipGadgetUp instead. The actual
kUseGadget hash appears throughout the game's input registration and gameplay
code and is the action pulsed by the current build.

## Remaining validation

Static analysis cannot establish the exact point in the frame at which Script
Hook drains GameMainThreadCallFunc relative to gameplay input consumers. The
native pulse duration and face-action suppression therefore require an in-game
test. NativeDirectFire can be disabled independently so selection can be tested
without the action pulse.

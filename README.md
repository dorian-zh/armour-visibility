# Armour Visibility (RE_Kenshi plugin)

Hide worn armour on any squad member without unequipping it. Hidden armour keeps all of its stats; only the model is hidden.

- Toggle per slot: Head, Shirt, Body armour, Legs, Boots, Belt, Backpack.
- Hiding is per character and per slot, so a new helmet equipped into a hidden head slot stays hidden.
- Saved with each save game (an `ArmourVisibility.txt` file in the save folder).

## Using it

Open a squad member's inventory. Each equipment slot has a small tick box in its top-right corner.

- **Ticked** (the default): the armour in that slot is shown.
- **Unticked**: it's hidden, but still equipped.

Ticks only appear for your own squad members.

## Requirements

- Kenshi 1.0.65 or 1.0.68 (Steam or GOG)
- [RE_Kenshi](https://www.nexusmods.com/kenshi/mods/847) 0.3.1 or later

## Building

KenshiLib plugins must be built with the **Visual C++ 2010 x64 compiler** (toolset `v100`), driven from Visual Studio 2019 or newer.

1. Install Visual Studio 2019+ with C++ support, plus the VS2010 x64 compilers. Copies of Visual Studio 2010 are on the [Wayback Machine](https://archive.org/search?query=visual+studio+2010).
2. Clone the dependency bundle with Git LFS, not as a .zip:
   `git clone https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps` then `git lfs pull`
3. Run `Setup.bat` in that folder. It extracts Boost 1.60 and sets the `KENSHILIB_DIR`, `KENSHILIB_DEPS_DIR` and `BOOST_INCLUDE_PATH` environment variables this project uses.
4. Open `ArmourVisibility.sln` and build **Release | x64**. Debug builds are broken in KenshiLib.

If setup gives you trouble, there is a more detailed guide at <https://github.com/weisspure/re_kenshi-working-solution>.

## Installing

1. Copy the `ArmourVisibility/` folder (the one with `ArmourVisibility.mod` and `RE_Kenshi.json`) to `[Kenshi install]/mods/ArmourVisibility/`.
2. Copy the built `x64/Release/ArmourVisibility.dll` into that same folder.
3. Launch Kenshi with RE_Kenshi and enable **ArmourVisibility** in the Mods tab.

## How it works

- Hooks `AppearanceBase::attachItem` (both overloads). If a slot is hidden for that character, the attach is skipped, so the mesh never appears.
- Every attach call's arguments are recorded, so a hidden item can be re-attached exactly as the game did it.
- Hooks `CharacterInventoryLayout::setupSections`, which builds a character's inventory window, and adds a `Kenshi_TickBoxSkin` tick (the skin RE_Kenshi's own settings menu uses) to each equipment slot. Each tick is set from the saved state when the window opens, and stores its own owner and slot, so the plugin keeps no pointers into inventory windows (a stale pointer to a closed window was the cause of an earlier crash).
- Toggles from the ticks are queued and applied in `GameWorld::mainLoop_GPUSensitiveStuff`, the same place the official Character Highlight example changes rendering state.
- Hooks `SaveManager::saveGame` / `loadGame` to write and read the hidden slots, and `newGame` / `importGame` to reset them.

## Untested assumptions

This was written against the KenshiLib headers but has not been run in-game yet. These are the things to check first, and where to adjust if one is wrong:

| Assumption | If it's wrong |
|---|---|
| A skipped `attachItem` should return `false` (the same thing the game sees when an item has no mesh for a race). | Set `BLOCKED_ATTACH_RETURN` to `true`. |
| `detachItem` removes the mesh immediately. | Try calling `appearance->reload()` at the end of `ApplyToggle`. |
| Hair reappears when a helmet is hidden. Refreshed via `notifyDirty()` and `setHiddenPartsEnabled()`. | Look at `AppearanceHuman::updateHiddenParts` / `attachItem_Hair`. Set `AV_REFRESH_HIDDEN_PARTS` to 0 if the refresh misbehaves. |
| `item->slotType` matches the equipment slot. | Match on the recorded slot string instead (check names in RE_Kenshi's log). |
| Character handles stay the same across save and load. | Hidden slots won't come back after loading; key them by character name instead. |
| The save folder is `<location>/<save name>/`. | The plugin falls back to `<location>/<save name>_ArmourVisibility.txt`. |
| Each equipment slot is its own section in the inventory window, marked as an equipped-item section with its slot type. | No ticks appear; log the section names in `SetupSections_hook` to see how the layout is split. |
| A tick in the slot's top-right corner sits on top of the slot and receives clicks. | Adjust `PlaceTick` (e.g. put it just outside the slot), or change `TICK_SIZE` / `TICK_MARGIN`. |
| Some clothing might also paint onto the body texture. | A hidden shirt could leave a painted-on look; would need a hook on `updateCharaterTexture`. |

**Suggested first test:** load a save, open a character's inventory and check a tick appears on each equipment slot. Untick the helmet, check that hair shows, then save, reload and check the helmet is still hidden. Next, unequip and re-equip the helmet, and finally walk far away and come back so the character's model is rebuilt. Check RE_Kenshi's log for `ArmourVisibility:` messages.

## License

GPLv3, because KenshiLib is GPLv3.

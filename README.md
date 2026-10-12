# Armour Visibility

An [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) plugin for Kenshi that hides worn armour without unequipping it or changing its stats. Visibility is controlled separately for each squad member and equipment slot.

This is a source-only release. The plugin has been validated in-game, but this repository does not include a compiled DLL or the installable mod package (`ArmourVisibility.mod` and `RE_Kenshi.json`).

## Using it

Open a squad member's inventory. Each equipment slot has a tick box: ticked shows the equipped item; unticked hides its model while the item remains equipped. The choice is saved per character and slot in an `ArmourVisibility.txt` file alongside the save game. Importing a game resets hidden slots.

## Building

Build on Windows with Visual Studio 2019 or newer, the C++ workload, and the **Visual C++ 2010 x64 compiler** (`v100`). Install Git LFS before fetching the dependencies.

1. Clone [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps) with Git, run `git lfs pull` inside that clone, then run its `Setup.bat`. Reopen Visual Studio afterward so it picks up the environment variables.
2. Open `ArmourVisibility.sln` and build **Release | x64**.

The expected output is `x64/Release/ArmourVisibility.dll`. The upstream [KenshiLib examples](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples#compiling) provide more setup detail. Debug builds are not supported by the dependency setup.

## Installing a build

The DLL alone is not an installable mod. Package it with `ArmourVisibility.mod` and `RE_Kenshi.json` in `[Kenshi install]/mods/ArmourVisibility/`, then enable the mod in Kenshi's Mods tab with RE_Kenshi installed. Those package files are not supplied here.

## License

GPL-3.0. See [LICENSE](LICENSE).

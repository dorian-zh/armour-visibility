# Armour Visibility

An [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) plugin for Kenshi that offers an in-game toggle for armour visibility without changing any stats. Adds a button next to every equipment piece which toggles whether they are shown or not. 

## Using it

Open a squad member's inventory. Each equipment slot has a tick box: ticked shows the equipped item; unticked hides its model while the item remains equipped. The choice is saved per character and slot in an `ArmourVisibility.txt` file alongside the save game. Importing a game resets hidden slots.

Compatibility has not been tested with other mods which change player meshes.

## Releases

See the releases section on the sidebar for the built dll. 

## Building

Build on Windows with Visual Studio 2019 or newer, the C++ workload, and the **Visual C++ 2010 x64 compiler** (`v100`). Install Git LFS before fetching the dependencies.

1. Clone [KenshiLib_Examples_deps](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples_deps) with Git, run `git lfs pull` inside that clone, then run its `Setup.bat`. Reopen Visual Studio afterward so it picks up the environment variables.
2. Open `ArmourVisibility.sln` and build **Release | x64**.

The expected output is `x64/Release/ArmourVisibility.dll`. The upstream [KenshiLib examples](https://github.com/BFrizzleFoShizzle/KenshiLib_Examples#compiling) provide more setup detail. Debug builds are not supported by the dependency setup.

## License

GPL-3.0. See [LICENSE](LICENSE).

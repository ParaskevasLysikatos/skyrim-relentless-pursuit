# Relentless Pursuit

SKSE plugin for Skyrim SE/AE (built and tested against 1.6.1170).

- **Doors:** enemies in combat follow you through load doors. At startup the plugin switches on *Disable Combat Boundary* for every encounter zone, in memory only, so no plugin records are overridden (Open World Loot and other zone mods keep their level ranges).
- **Water:** every race that can swim loses *No Combat In Water*, so bandits, wolves, bears and so on swim after you. Races with no swim animation stop at the shore.
- **Dragon guard:** dragons never follow you through a load door. When you use one, dragons fighting you stop combat. If one still comes through within 90 seconds, it is moved back outside that door.

No ESP: it doesn't use a plugin slot and is safe to add or remove mid-playthrough.

## Install (Mod Organizer 2)
Install `RelentlessPursuit.zip` as a mod and enable it. Requires SKSE and Address Library for SKSE Plugins.

Settings: `SKSE/Plugins/RelentlessPursuit.ini`. Log: `Documents/My Games/Skyrim Special Edition/SKSE/RelentlessPursuit.log`.

## Build
GitHub Actions builds it on every push (`.github/workflows/build.yml`) with xmake and [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR). Pushing a `v*` tag also publishes the zip as a release.

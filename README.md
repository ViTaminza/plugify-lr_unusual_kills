# lr_unusual_kills (Plugify)

[LR] Unusual Kills: counts unusual kills (first kill of the round, penetration, no scope, running, jump, blinded, through smoke, last clip, distance) in the `<lr_table>_unusualkills` table and awards the experienced configured per kill type in unusual_kills.ini (separate messages, added to the kill's experience, or records only - Exp_Mode). Ported from the Pisex Levels Ranks module LR_UnusualKills, same phrases file (lr_unusualkills.phrases.txt). Owns its plgdb connection with the levels_ranks database settings. Requires the levels_ranks core; no exported methods.

Plugify port (fork) of **LR_UnusualKills ([LR] Unusual Kills)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-lvl_ranks_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, levels_ranks, hudmenu (optional)

## Installation
Download `lr_unusual_kills-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/lr_unusual_kills`.

`plugins/levels_ranks` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.


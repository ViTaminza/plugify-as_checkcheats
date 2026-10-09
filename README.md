# as_checkcheats (Plugify)

Admin cheat-check module for admin_system (port of the Pisex Metamod:Source module 'CheckCheats'): !check/!cheats opens the check menu (or the 'checkcheats' item of the players admin-menu category), the admin picks a suspect, the suspect submits a social-network contact with !contact, the admin starts the check and closes it with a verdict from configs/admin_system/checkcheats.ini - optionally banning the suspect. Matches the original commands, menu items, phrases and config keys; writes the same checkcheats_stats table through its own plgdb connection. No exported methods.

Plugify port (fork) of **CheckCheats** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_checkcheats-1.0.6.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_checkcheats`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.


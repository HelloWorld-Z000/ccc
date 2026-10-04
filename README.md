# Cinematic Conversation Camera

Skyrim dialogue camera mod. Cut between the player and NPC, choose camera
angles, and adjust framing, movement and dialogue presentation.

## Setup

Close Skyrim and install with MO2 or Vortex, then start the game through SKSE.
Open **Cinematic Conversation Camera** in SKSE Menu Framework to change settings,
or edit `Data/SKSE/Plugins/SD.ini`. Menu settings save to `SD_user.ini`.
Keep that file when updating.

Requires Skyrim SE/AE, matching SKSE and Address Library. SKSE Menu Framework
is optional. Dragonborn ReVoiced 1.4.4 or newer is recommended for player voice
timing and lip sync. DBVO 1 and 2 are also supported.

Keep SmoothCam enabled. Improved Camera users playing in first person need
`bScripted=0` in its profile. Disable other dialogue-camera mods.
See [compatibility and settings](USAGE.md).

No ESP or Papyrus scripts. Nothing is written to your save.

## Docs

- [Controls and features](USAGE.md)
- [Build](BUILDING.md)
- [Changes](CHANGELOG.md)
- [Publish source](LICENSING.md)

Version 1.5.0 can film other NPCs' conversations (by key, or automatically when
you stand still), put subtitles inside the black bar, and hold a close-up on the
answer to a persuasion check. Close-ups follow the face, NPCs put down their
work to talk, and the extreme close-ups are tighter. Presets can be exported
and installed as files, so they can be shared as their own mods. The build and
24 tests pass.

## License

GPL-3.0-or-later with the permissions in [EXCEPTIONS.md](EXCEPTIONS.md).
See [LICENSING.md](LICENSING.md) and [third-party credits](THIRD-PARTY-NOTICES.md).
Publish the matching complete source ZIP with each mod download.

Created by hashhbbrown.

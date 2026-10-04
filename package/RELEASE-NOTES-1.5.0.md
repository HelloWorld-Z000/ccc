# Cinematic Conversation Camera 1.5.0

## New

- Film other people's conversations: press Film Their Conversation (Keys page),
  or turn on Film When You Stand Still (Camera > Other People's Conversations),
  and two NPCs talking are filmed like your own conversation. Game scenes are
  filmed as a whole, and the camera lets go when the scene hands you your turn.
- Subtitles In The Black Bar (Screen > Black Bars): the spoken line sits inside
  the bottom bar and follows it. Off by default.
- Hold On Their Answer (Camera > Persuasion): persuade, intimidate or bribe, and
  their answer gets a close-up with a slow push in.
- Close-Ups Follow The Face (Camera > Framing): close-ups frame where the face
  actually is, for example someone leaning over a workbench.
- They Stop Working To Talk (Faces > Body): NPCs at a workbench, forge or
  similar step away from it a few seconds into the conversation.
- Your Gestures Start With Your Line (Faces > Body): DBVO player-gesture add-ons
  now gesture while you speak under ReVoiced and DBVO 2.
- Preset files: export your settings from the Presets page as a JSON file and
  share it as a mod. Install as many preset mods as you like; each preset in
  SKSE/Plugins/SceneDirector/Presets/ is listed on the same page.

## Fixes

- The extreme close-ups now crop below the chin.
- NPCs no longer snap into place when the camera cuts to them.
- Stay On You After You Speak no longer holds on you once their reply starts.
- The spent topic list is gone by the time your voiced line ends.
- The topic list fade no longer flashes rows while the menu rebuilds them.
- The camera takes conversations it used to miss (a distant hostile, or
  re-entering a conversation while the NPC is still talking).
- Bars are drawn correctly under the interface with the PureDark upscaler.
- Quieter log.

## Install

Close Skyrim and replace the old version through MO2 or Vortex.
Keep SD_user.ini and any separately installed regional face asset.

The mod ZIP contains the DLL, default INI and documentation.
The Source.zip is for building and modifying the mod.

## Checks

The Release build and all 24 tests pass. The DLL version is 1.5.0.0.

## Publish

Upload the mod ZIP and matching complete Source.zip together.
Apply the prepared Nexus text and GPL permissions in [LICENSING.md](../LICENSING.md).

Archive hashes are in the adjacent .sha256 files. SOURCE-MANIFEST.json lists
source file hashes.

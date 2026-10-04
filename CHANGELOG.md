# Changelog

## 1.5.0 — 2026-10-03

New:

- **Film other people's conversations.** Two NPCs talking to each other are
  filmed like a conversation of your own, with the second NPC in your place in
  every shot. Use the Film Their Conversation key (Keys page,
  `iKeyFilmScene`), which works outside conversations and also films one NPC
  talking to you, or turn on **Film When You Stand Still** (Camera > Other
  People's Conversations, `bFilmScenesAuto`, default off; `iSceneRange`,
  `iSceneWait`) to start filming when you stand still near a conversation in
  front of you.
  - A scene the game is playing is filmed as a whole: anyone in it can speak,
    and the camera pairs the speaker with whoever they're talking to.
    Filming follows the sequence into its next scene and continues if
    someone in it dies.
  - Automatic mode only starts on a real conversation: two NPCs in the same
    scene, or a quick back-and-forth between them. A line said to you outside
    a scene (a greeting in passing, a follower's aside, a merchant) never
    starts it; a scene that addresses you with several voices does.
  - When a scene hands you your turn ("Up through the tower, let's go!"
    followed by the same lines on repeat), the camera lets go a second after
    the last real line. This is read from the scene's own data (lines the
    game repeats while it waits), not from the words.
  - A one- or two-word shout from outside the frame ("Halt!") doesn't cause a
    cut. Shots from behind someone's head are avoided.
  - Moving, combat, a menu, your own conversation or the scene going quiet
    hands the camera back.
- **Subtitles In The Black Bar** (Screen > Black Bars, `bSubtitlesInBar`,
  default off). The spoken line sits centred in the bottom bar and follows it
  as it eases in and out. With no bar, or a bar too thin for the text, it goes
  back over the picture. Works for your conversations and for filmed ones.
  Edge UI's Exit prompt, which sits in the bottom bar, is faded while this is
  on; it still works.
- **Hold On Their Answer** (Camera > Persuasion, `bPersuasionBeat`, default
  on). When you persuade, intimidate or bribe, their answer cuts to a
  close-up of them, held for the whole reply with a slow push in. Speech
  checks are recognised from the reply's conditions, so modded checks that
  use the standard conditions work too.
- **Close-Ups Follow The Face** (Camera > Framing, `bFollowFace`, default
  on). Close-ups frame where the face actually is, so someone leaning over a
  workbench is framed on their face rather than on where their head was.
  Humanoids only. In filmed scenes the face is found at each cut and then
  held, since people in scenes walk and kneel.
- **They Stop Working To Talk** (Faces > Body, `bStopWorkToTalk`, default
  on). Someone at a workbench, forge, smelter, tanning rack or similar steps
  away from it 3 to 5 seconds after they start talking. Seats, beds and quest
  scenes are left alone.
- **Your Gestures Start With Your Line** (Faces > Body, `bPlayerGestureCue`,
  default on). When your voiced line starts, the mod sends DBVO's
  PlayDBVOTopic event, so DBVO player-gesture add-ons gesture while you speak
  under ReVoiced and DBVO 2. Not sent under DBVO 1, which sends its own.
- **Preset files.** Presets can be exported and installed as JSON files, so
  they can be shared as their own mods. Every file in
  `SKSE/Plugins/SceneDirector/Presets/` is listed on the Presets page under
  Installed Presets, as many as you install, and one file can hold several
  presets. The Export section saves your current settings there with a name,
  author and description. Mistakes in a hand-edited file are shown under the
  preset instead of failing silently. The format is in USAGE.md, and
  `[Presets] sApply` can name an installed preset.

Changed and fixed:

- The extreme close-ups are now actually extreme: they frame forehead to just
  under the chin. They were sized like a close-up.
- NPCs no longer snap into place when the camera cuts to them. The game only
  fully animates actors that were on screen last frame; the people the camera
  may cut to are now kept fully animated while it's directing.
- **Stay On You After You Speak** only covers the silence before their reply;
  the camera goes to them as soon as they start talking.
- **Fade After PC Line**: the topic list you picked from is gone by the time
  your voiced line ends, instead of fading during their reply.
- The topic list fade no longer flashes rows while the menu rebuilds them,
  never brightens what the menu itself hid, and leaves the list alone when
  the menu's state can't be read.
- The camera now takes conversations it used to miss: a distant hostile
  keeping your combat flag up no longer releases it while the dialogue menu
  is open, and re-entering a conversation while the NPC finishes a line is
  staged right away.
- Under the PureDark upscaler, the bars are drawn correctly beneath the
  interface (the UI and the swap chain are different sizes there).
- Quieter log: lines that used to repeat on every check are written when they
  change, and the frame heartbeat is once a minute.

The Release build and all 24 tests pass. New options that change behaviour
on their own (filming by standing still, subtitles in the bar) ship off.

## 1.4.8 — 2026-09-23

- Add **True 180 Rule** (Camera > Framing, `[Direction] bTrue180`, default off).
  You are filmed over one shoulder and the other person over the opposite one,
  so the camera stays on one side of the conversation. Before this, both
  over-the-shoulders used the same shoulder and every reverse crossed the line,
  even with Never Cross The Eyeline on. Enabling it also enables Never Cross The
  Eyeline. The key light follows the camera to its side of the line.
- Add **Stay On You After You Speak** (Camera > Holds, `[Direction]
  iPlayerVoiceHold`, 0–3 s, default 0). With a voiced player line, the camera
  stays on you this long after your line ends before it goes to them. 0 is the
  current behaviour: cut the moment you finish.
- Add **Reaction Shots** (Camera > Reaction Shots, `bReactionShots`,
  `iReactionEvery`, `iReactionChance`, default off). After every N of their
  lines, a percent chance that their next line plays on you listening, then the
  camera goes back. Short lines and full-intensity lines are never reactions.
  Automatic framing only.
- The line count for **Per Line Angle Change** and the Reaction Shots count and
  roll now start again with every reply, including one to a topic picked while
  the NPC was still talking. Before, the cadence counted across the whole
  conversation, so a reply could cut early on progress made in the last one.
- Restore Defaults now also resets Never Cross The Eyeline.
- TrueHUD compatibility: the Recent Loot list stays visible during
  conversations, so items received from an NPC show up. TrueHUD's bars and
  other widgets are still hidden while the camera is directing.

The Release build and all 17 tests pass. All new options ship off or at 0, so
existing setups behave as before until they are turned on.

## 1.4.6 — prepared 2026-09-13

- Cache settings while INI files are unchanged. File edits apply at the next
  conversation; menu changes clear the cache. Settings precedence is unchanged.
- Load expression profiles once when a conversation starts.
- Log conversation setup time and add cache/Windows INI tests.
- Include GPL notices, build instructions and the matching complete source ZIP.
- Shorten the README, Nexus text and release notes. Add USAGE.md for settings.

The Release build and 16 tests pass. In-game performance testing is still needed.
The reported dialogue exit/re-entry control lock remains unresolved.
## 1.4.5 — 2026-09-11

Packages the current playtested development tree. No additional
camera or expression behavior changes were made for this version bump.

- Clause-based player facial acting separates conversational action from tone:
  distinct inquiry, confirmation, skepticism, greeting, reassurance, explanation
  and statement gestures, with mouth-safe brow/lid/cheek movement.
- Questions and emotional changes receive their own estimated clause timing;
  common negations and quoted speech are guarded. Neutral NPC records no longer
  receive keyword-derived emotion overrides, and neutral listening releases old
  affect. Existing silent-listener full-face expressions retain speech priority.
- Includes the player-voice-end NPC camera handoff and current UI/re-entry
  safeguards from the development tree. The reported dialogue exit/re-entry
  control lock remains unresolved; this release does not claim to fix it.
- Retains the pinned CommonLibSSE-NG compatibility work for Skyrim 1.7.99/1.7.104.
  Matching SKSE and Address Library are required; future 1.7.x is not guaranteed.
- The release ZIP contains the DLL and default SD.ini only. No user INI or
  third-party-derived HPH asset is bundled. Existing compatible local HPH female
  assets continue to work; without one, native brow/squint acting remains available.
- Release build and all 12 automated test suites pass. Clause timing remains an
  estimate, not word-level audio alignment. Automatic player reaction shots are
  not included.

## 1.4.2

### Emotion overhaul — clause acting v2 (development test)

Replace the player upper-face controller with a clause planner, separate action
and tone, and distinct inquiry/confirmation/skepticism/explanation/reassurance/
greeting/statement recipes. Questions use brow knit and focused lids rather than
the surprise region. Timed beats follow word-weighted clause spans inside the
voice window, using the existing read-only voice clock; no word-level alignment
or voice-tone inference is claimed. Explicit social/first-person cues supply
tone; common negations and quoted speech are guarded. Neutral NPC records no
longer get keyword-derived full-face emotions. Neutral listener lines release
previous affect instead of carrying it indefinitely. Preserve mouth ownership,
regional topology guards, bounded smoothing, voice handoff and camera/input logic.
New plan/beat diagnostics and semantic/temporal regression coverage accompany
the redesign. Legacy classifier helpers remain for compatibility tests, but the
active player controller no longer uses the previous DialogueAffect rule set.

### Speaking upper-face phrase test

The missing movement was on the player while speaking, not while
listening. Strengthen neutral spoken delivery and give it opening/phrase accents;
increase the mouth-excluded regional eye/cheek contribution, including ordinary
questions and statements. Keep routine questions emotionally neutral and preserve
native modifier caps, smooth interruption/cleanup, blink masking and mouth
ownership. No changes to lipsync, NPC acting, listener-v2 tuning or camera logic.

### Stronger cinematic listening test (v2)

The first full-face test was still too subtle in the HPH female playtest, including
at the maximum 2.0 expression strength. Raise the listener coefficient from
0.38 to 0.72 and the shared native-pose ceiling from 0.55 to 0.85. Broaden emotional
accents and retain more smile/concern after the peak; surprise still subsides.
Speech gates, ownership cleanup, overlapping-layer attenuation and camera logic
are unchanged. This is tuning for another visual test, not a validated release.

### Cinematic listening expression test

The HPH female playtest verified that the regional layer binds and applies, but
the normal player controller still excluded lower-face expression. Silent
listening now adds bounded native full-face poses, reducing overlapping upper
layers as the pose develops. Player voice onset, disable and close release that
contribution; restoration is tied to the exact head/animation buffer and preserves
other writers' changed values. Distinct reaction envelopes settle differently;
NPC disgust no longer always becomes puzzled, and anger no longer mandates fear.
`[Performance] bCinematicListening=0` disables the full-face contribution for A/B
testing. No automatic player reaction shots, gaze changes or re-entry lock fix
are included. This is a prototype requiring visible in-game validation.

Skyrim 1.7 runs. Two camera bugs reported against 1.4.1 are fixed, a shot you
switch off is now off everywhere, and the player's face reacts to the line being
spoken instead of sitting still.

### Voiced-player camera handoff

Automatic coverage now requests the NPC when the player's observed voice ends,
including the quiet post-line delay before the reply. That voiced exchange no
longer adds the unvoiced player-beat delay at the start of the NPC's reply.
Manual framing, shot eligibility, safety checks and unvoiced timing are unchanged.
This is camera selection only: dialogue readiness and input are not modified.
Timing follows the existing voice tracker (measured audio when available, its
fallback timing otherwise). In-game timing and the earlier re-entry fix still
need playtesting; passing policy tests is not an in-game validation.

### Skyrim 1.7.x

Bethesda's August updates take the game to 1.7.99 and then 1.7.104. This mod did
not run on either, and the reason is a single line in the library it is compiled
against, which decides which Skyrim it is looking at by reading the minor version
and nothing else: 4 is VR, 6 is Anniversary Edition, and anything that is neither
is assumed to be the 2016 Special Edition.

1.7 is neither, so it was read as Special Edition — and everything downstream of
that goes wrong at once. The address library, the table every version-independent
plugin uses to find anything inside the exe, is looked for under
`version-1-7-104-0.bin`. That file does not exist: Anniversary Edition ships the
same table as `versionlib-1-7-104-0.bin`, and the two names are the whole
difference between finding it and not. Behind that, every address the mod does
resolve would have taken its Special Edition value, and the structure layouts
picked would have been the ones from before 1.6.629 on an executable long past
it.

So the library moved. The packaged copy this project had built against since it
started was last published in May 2023 and is now marked unmaintained by its own
authors, which is exactly why the game moved and the mod did not. In its place is
the fork still being maintained: it reads any minor version of 6 or above as
Anniversary Edition, and it has already tracked the fields 1.7.99 moved. It is
pinned inside the repository rather than fetched while building, so which version
this mod was compiled against is a fact about the checkout and not about whatever
machine happened to build it.

One DLL still covers 1.5.97 through 1.7.104. Nothing about how the mod behaves
changed, no setting was added or removed, and on the versions that already worked
this is the same build it was before.

You still need SKSE and Address Library for the version you are actually running
— on 1.7.104 that is SKSE 2.3.1, and the Anniversary Edition Address Library.

### Talking to somebody too quickly no longer switches the camera off

Reported against 1.4.1: talk to an NPC too fast and the cinematic camera turns
itself off. It does, and it stays off for the rest of that conversation.

Here is the whole of it, out of a log from a shopkeeper spoken to twice in quick
succession:

```
09:38:55.962  Dialogue Menu CLOSE | manager: speaker, talking
09:38:55.989  Player left the conversation; releasing
09:38:56.615  Dialogue Menu OPEN  | manager: lastSpeaker only, silent
09:39:04.993  ...speaker handle returns; the camera comes back
```

Nine seconds of dialogue menu on screen with the bars gone, the HUD back and the
game's own camera in charge — and the player standing perfectly still through
all of it, which the face probe confirms frame by frame.

The mod decides the player has walked out by watching one thing: the engine's
record of who is being spoken to. When it goes empty, the camera is handed back
at once, and that is deliberate — waiting for the NPC to stop talking used to
leave the camera locked on somebody the player had already walked away from.

But an empty speaker is not the same as an empty room. The engine also clears it
in the gap between one conversation and the next with the same person: commit a
topic while a line is still running, or press activate again a moment after the
last exchange ended, and it drops the record, closes the menu, opens it again,
and takes as long as it likes to fill the record back in. Above, that was nine
seconds. Everything the mod could see said the player had left. They were
standing in a dialogue menu the whole time.

The dialogue menu is what tells the two apart, and it is not a subtle signal:
Skyrim disables the movement controls for as long as it is up. A player looking
at one has not walked away from anybody — they cannot. So the release now needs
both: no live speaker **and** no dialogue menu. Closing the menu still hands the
camera straight back, which is the case that rule was written for.

### Re-entering a conversation waits for the live speaker

A camera that was released may recover while the player is still engaged in the
same conversation. Recovery respects combat, pausing menus and suspension, is
limited to one attempt a second, and does not re-read the settings.

A reopened dialogue menu alone is not enough. When the player returns while the
NPC is finishing an earlier line, the menu can remain in its greeting phase with
only `lastSpeaker` populated. The camera now waits for the live speaker handle
before staging. When that handle returns, the existing conversation serial
logic starts the new scene once.

A captured Duraz re-entry stayed in that gap for about six seconds. Previously SD
staged the old conversation during the gap, then closed and opened again when the
engine recognized the new one. The new guard avoids that premature staging; it
does not force the engine to finish or skip the NPC's line.

### Topic-list restoration belongs to its original menu

Fading now remembers the exact display objects it changed and their original
alpha and visibility. It restores only those objects, and leaves values changed
by another writer alone. Replacing the dialogue movie resets the cached paths
and cannot transfer a pending restore to its new topic rows.

A full-opacity request releases SD's fade rather than forcing a previously
untouched clip visible. This keeps the new menu's unfinished placeholder rows
under the movie's own control. Failed restoration still retries for the same
object, including through barter, but never for an unrelated replacement.

Regression tests cover replacement during a failed restore, same-menu handback,
already-hidden clips, non-default opacity and another writer changing a value.
Skip/exit input remains untouched; the reported input lockout still requires an
in-game retest.

### Shots you switch off stay off

The Shots page has always looked like a set of switches, and two of them were
not wired to everything they needed to be.

A shot you disabled, or set to zero frequency, was still reachable four other
ways: through the fallback that runs when the chosen angle cannot be placed,
through a saved pose being reused, through a resume after a menu, and at the
last step where the camera is actually applied. Turning a shot off took it out
of the ordinary rotation and left those four alone. It is now excluded from all
of them.

Behind that sat an emergency camera that quietly substituted a medium single
whenever the selected shot failed. It ignored your settings by construction — it
was not one of the compositions, so there was nothing to switch it off with. It
is gone, and recovery now picks from the angles you actually left enabled.

With every shot off there is no implicit shoulder shot any more either. Where
subject protection needs a view it uses first person if that is allowed;
otherwise Scene Director leaves the ordinary game camera alone rather than
inventing an angle you did not ask for.

Obstruction adjustment is capped at ±12°, in 3° steps. The old search ran to
±80°, which is wide enough to walk a Close Up around into a profile: you chose
one shot and got another, and the shot list looked wrong when the search was at
fault. The cap applies in normal and subject-protection modes, to held poses and
to continuity clamping. Placement lines in the log now name the adjustment that
was chosen, so this is checkable in game.

Saved poses keep their shot identity, composition and lens, and disabling a shot
that is currently held invalidates its reuse immediately rather than at the next
cut.

### The player's face reacts to the line being spoken

Responsive expressions were not off by default — they were off in the runtime.
The setting saved in your ini was read and then overruled, and strength was
pinned at 100%. Both controls are live under Faces now, and apply as you drag
them.

What drives the expression, in order: an NPC's authored emotion wins whenever
the line carries one. Neutral records and player topics — which is most of what
you say — are read for explicit English text cues, now including concern, doubt,
resolve and curiosity. A line with no cue stays neutral, and punctuation alone no
longer turns an unmarked line into anger. There is no voice-tone analysis
anywhere in this.

Listening has its own reactions rather than a blank face, with a quick onset and
a gradual return to rest, and they carry across neutral continuation lines with a
bounded decay. A new topic clears that memory, so a run of neutral lines cannot
stretch one reaction indefinitely or restart the opening gesture.

The player's upper face is one coordinated brow and squint performance rather
than a full-face override — warm, concerned, doubtful, firm and surprised, on
slow critically damped gestures that keep their velocity through an interruption
instead of snapping back. The random syllable accents and the per-line brow
asymmetry are gone; they read as fidgeting rather than as acting. NPCs keep their
full-face dialogue expressions.

The two face layers no longer fight each other. Responsive expressions own the
brow and squint channels while active, including their release tail, and the
standalone brow layer waits until they let go. Mouth lip sync stays independent
of both, and blinking and eye direction are left to the engine throughout.
Modifiers this mod owns are released on handback, on a head being replaced and on
load, without clearing values another writer has changed.

Fixed alongside: the mouth toggle was stopping brow and expression timing as well
as the mouth.

### Eye and cheek detail

Brows and squints are modifier channels — a fixed set of controls, shared with
the engine. Around the eyes that is a coarse instrument, so there is an optional
geometry layer on top of it: additional eye and upper-cheek movement written
directly to the head mesh.

It is player-only, and it currently supports the High Poly Head female head. The
installed .tri is fingerprinted and its vertex count checked before anything is
written; any other head, or a mismatch, keeps the brow and squint motion and
nothing else changes. A feathered mask keeps the overlay off the lower face,
holds down its overlap with the brow controls, and yields at the eyelids so
native blinking still reads. Mouth and phoneme data is preserved, and the layer
restores its previous offsets before each face pass — it changes no shared head
asset, no RaceMenu sculpt and no NPC geometry.

The toggle is Eye & Cheek Detail, under Faces. Independent cheek morphs are
future asset work.

### Tests

The face work is covered by motion regression tests at 30, 60 and 144 FPS,
including interruptions, release, frame-to-frame movement and the absence of
repeated brow reversals. The geometry layer adds 10,000 apply/restore cycles,
protected mouth vertices, native blink priority, mismatched vertex counts and
handback to another writer. Dialogue cue classification, authored priority, morph
mapping, conflicting shapes, attack/crossfade/release and invalid input are
covered separately.

## 1.4.1

The trade menu, end to end. The camera is handed back in the right order and at
the right moment, the seam either side of a menu stops lurching, Improved Camera
is recognised and left to its own zoom, and the black bars come back.

### An angle you switched off no longer comes back as a fallback

Reported with a screenshot: the PC section reading **0 of 14 on**, and the camera
still cutting to Over Their Shoulder the moment the NPC stopped talking.

When the camera needs an angle and every candidate has been ruled out — by the
room, by the geometry, by your own settings — there is a last-resort answer that
picks something sensible for whichever person it belongs on. That answer was
seeded with a shoulder shot *before* it checked what you had left enabled, so if
you had switched off every angle of yourself it handed that shot back anyway. The
one branch meant to be honouring your choices was the one overruling them.

It now falls back in order: an angle you left on for the right person, then a
two-shot, then anything you left on at all. Only if you have switched off every
angle in the mod does it pick for you, and at that point there is no preference
left to honour.

### "Cut To You On Your Turn", switched off, now means what it says

The Shots page has always said: *"Turn at least one on, or switch off 'Cut To You
On Your Turn'."* That sentence promises the setting is an alternative to having
player angles enabled, and it was not one.

Switching it off only made a two-shot an *acceptable* way to cover your turn. It
never stopped the camera wanting you — so the moment the NPC finished a line the
subject changed hands, and any player angle that framed well still won.

Off now means the subject does not change hands at all. The camera stays on the
angle it was already using, and the next change comes from the ordinary rhythm —
the line count, or the timers if you have them on — rather than from the turn
passing.

### Improved Camera is recognised, and the zoom is left to it

Reported: Scene Director does not work with Improved Camera — the view zooms in
and out repeatedly.

Handing the camera back at the end of a conversation puts nine fields of the
third-person camera state back the way you had them. Four of those are the zoom,
and for Improved Camera the zoom is not a setting — it is the **mechanism**.
Moving between first and third person *is* driving those four fields, frame by
frame. So Scene Director stamping a reading taken before the conversation into
the middle of that transition was a second author on one animation, and a view
with two authors pumps.

SmoothCam never had this problem, and the difference is not luck. SmoothCam and
Scene Director talk to each other: there is an interface for asking for the
camera and for giving it back, and the ordering fix below settles which of them
is writing when. The Improved Camera builds this was tested against — the 1.1.x
line — publish nothing of the kind, so the only way not to fight them is not to
write.

It is now detected at startup, by name, and while it is installed the
third-person zoom is left alone. **The aim is still handed back** — that is the
fix that stops a conversation ending with the camera pitched at the floor,
Improved Camera does not drive it, and giving it up to be careful about something
else would trade one bug for another.

Some of what was reported here is fixed by the menu work elsewhere in this
release rather than by anything Improved Camera specific. In 1.4, opening a
merchant's stock *ended* the cinematic and closing it started a fresh one, so
every trade was a full trip out of first person and back again. A menu suspends
the conversation now and the view is left exactly where it is.

### Closing a menu no longer lands on one angle and snaps off it

Reported: *"just opened a menu, camera goes crazy and picks different angles
randomly"*.

Closing an inventory or a container resumes the conversation on the angle it was
on, which is 1.4's promise and the right one. What it was not doing was checking
whether that angle still points at the right person.

Close a container while nobody is speaking and the camera belongs on **you** —
that is what it does at the start of any conversation where nobody has said
anything yet. The resume was putting back a close-up of the person you were
talking to instead, and the rule that keeps the camera on whoever is talking
noticed immediately and cut away from it. Not after the two and a half seconds an
angle is normally given, because a shot pointed at the wrong person is allowed to
be replaced at once — a quarter of a second.

So you closed a container, landed on a shot of a silent NPC, and were pulled off
it before you could read it. Twice in a row it looks like the camera choosing at
random, and in a sense it was: which two angles you got depended on whatever
happened to be on screen when the menu opened.

The stored angle is now only put back when it still frames the right person.
Closing a merchant's stock while they are still talking — the case this was
written for — returns to exactly the shot it left, as before. Closing one in a
silence keeps the opening angle, which is one clean landing instead of two.

### Four ownership and timing faults around menus, found by review

None of these were reported from play. They came out of an adversarial read of
the work above, and all four are the same shape: something that only happens on
the far side of a menu, where the mod cannot see.

**The half-second settle after a menu had never once run.** The camera's resting
aim, zoom and lens are not sampled for half a second after a menu lets go,
because whatever else manages the camera is still settling. That window was timed
from the last frame that *saw* a menu open — and a menu that pauses the game
stops the frame source, so the freshest thing the mod knew was the moment the
menu **opened**. Spend four seconds in a container and the window had expired
before you closed it. The menu watch runs on the engine's own events, which keep
arriving while the game is stopped, so it now reports the closing edge directly.

**Ending a conversation inside a menu wrote to a camera already handed back.**
Suspending gives the camera back to SmoothCam. If the conversation then turned
out to have ended while the menu was open, the mod put the aim, zoom and lens
back without asking for the camera again — writing nine fields of a state
SmoothCam had been driving for as long as the menu was open. That is the same
write-after-hand-back the ordering fix below removes, arriving through the one
path that skipped it. It now takes the camera back, writes,
and gives it back again; if another mod holds it, the writes are skipped and the
log says so.

**A refused camera lost the player's first-person view.** If another plugin held
the camera when a conversation resumed, the mod declined to stage — correct — but
had already discarded the record of the suspension, including the fact that a
first-person player was owed their own eyes back. They stayed in third person
with nothing left that knew. The record now survives a refusal and is paid when
the conversation ends.

**Resuming during a line the NPC never stopped could cut away from them.** The
mod learns who is speaking from the start of each line, and deliberately does not
re-announce a line that is already running. So a line that began before you
opened a container and was still going when you closed it produced no such
signal, the resume concluded nobody was talking, and the angle on the speaker was
discarded as stale — then cut back to a quarter second later. It now asks the
conversation itself who is speaking rather than waiting for an announcement that
is not coming.

### Four more from a second review pass, all on the same seam

**A refused hand-back no longer throws the debt away.** The fix above takes the
camera back before putting your view right, and skips the write if another mod
refuses it. What it then did was forget it had ever owed one — so a refusal
lasting a fraction of a second left you holding a cinematic lens and the game's
dialogue aim for the rest of the session. "Can this conversation resume" and "is
the view still owed" are two questions now: the first is answered and done with,
the second waits and is retried, once a second, until no conversation is running,
no menu owns the screen, and the camera can actually be had.

**A suspension knows which conversation it belongs to, not just who with.** It
was matched on the person alone, which cannot tell two consecutive conversations
with the same actor apart — the same ambiguity the rest of the mod already keys a
serial number against. A session that ended and restarted behind a menu could
come back wearing the previous conversation's angle.

**Walking out of a menu into somebody else no longer strands a first-person
player in third.** Suspending leaves a first-person player in third person on
purpose and records that they are owed their own eyes back. If the screen
returned into a conversation with a *different* person, that record was dropped —
and the new conversation could not work it out for itself, because by then they
were already in third person. The debt now transfers.

**The original Improved Camera is no longer told to edit a file it does not
have.** Both generations are detected, but only Improved Camera SE's
configuration layout is known. A legacy-only install was being handed a path
built out of two guesses. It now says what it cannot determine instead, and the
SE path is checked to exist before being offered.

### The frame stops lurching on the way into a menu, and on the way out

Two more from the same report, both of them the seam either side of a menu rather
than the menu itself.

**Going in, the view zoomed out before the menu appeared.** Handing the screen
back for a container took the shot's lens with it — snapping from the thirty-odd
degrees the angle was composed on to the eighty you play at, on the frame before
the menu drew. The world behind a menu is frozen, so it then sat on that framing
for as long as you were in there: not the cinematic, not your normal view, but a
close-up pose seen through a wide lens.

The fix below already stops the aim and the zoom being handed back for a menu, on
the grounds that the conversation is coming straight back and there is nothing to put
back. The lens was simply left out of that. It is in now, so the frame freezes
exactly as it was — nothing moves at all. If the conversation turns out to have
*ended* while the menu was open, the lens is restored there instead, along with
the aim, which is the one place it was genuinely owed.

**Coming out, the camera cut in tight on the NPC before finding the real angle.**
This one was a cost, not a decision. Every time a conversation opens, the mod
re-reads its settings — every angle's enable, weight, lens, zoom and move — so
that editing the ini takes effect on the next conversation without a restart.
Thirty-nine angles of that is a few hundred separate reads off disk, and it takes
about an eighth of a second.

Opening a conversation, nobody sees that: the dialogue menu is still coming up.
Resuming one, the conversation is already on screen — and for that eighth of a
second the only thing driving the camera is the game's own dialogue camera, which
sits in close on whoever you are talking to. So closing a container gave you a
hard push into the NPC's face and then a jump to the angle that was supposed to
be there.

A resume does not need the re-read. The settings were read when the conversation
opened and a container does not edit your ini. Editing settings between
conversations works exactly as before.

### And the one setting that decides whether the two can share a camera is named at startup

There is a second half to this, and it is not something Scene Director can fix
from its own side.

Skyrim takes your movement controls away for the length of a conversation.
Improved Camera reads that as a **scripted third-person event** — the same
category as a cutscene — so every dialogue in the game is one to it, whether or
not this mod is installed. Whether it then acts on that is a single setting in
its profile, `[EVENTS] bScripted`, and it ships **on**.

With it on, a player who was in first person gets Improved Camera's *fake first
person*: it pins the third-person zoom to its minimum every frame and pulls the
camera back to your head. Scene Director spends those same frames driving a shot
from across the room. Both write the camera, neither gives way, and what you see
is the view flipping between the angle and your own eyes.

On these builds there is nothing to negotiate. Improved Camera 1.1.x publishes no
interface — 1.1.2.4228 was the version measured — and where it asks SmoothCam for
the camera it discards the answer, so the refusal Scene Director's own hold
produces is simply ignored. Backing off quietly would mean no cinematic at all,
for a setting you did not know was on. Later Improved Camera versions are
reported to offer camera ownership to other plugins; if those become a supported
target this should be a negotiation rather than a setting to read.

So it is **reported instead**, at startup, by name, with the path of the file to
edit — which is not the file named after the mod, and that catches people out.

Be clear about what `bScripted=0` costs, because it is not a dialogue switch: it
is Improved Camera's whole scripted-forced-third-person category, so its
first-person handling goes for every event in that category and not only for
talking to people. Dialogue is simply the one that collides with this mod.

**This only affects first-person players.** If you play in third person the two
mods never meet, and the log says so rather than warning you about something that
cannot happen to you.

### The field of view stops taking readings in the wrong moment

Scene Director keeps a note of the field of view you play at, so it can compose
against it and put it back afterwards. That note was taken on any frame with no
conversation running, on the reasoning that nothing else moves the lens.

Plenty else moves the lens — ENB presets, SmoothCam's own offsets, Improved
Camera's first-person view — and the worst possible moment to ask is the frame
after a shop closes. The frame source stops while the game is paused, so the
first reading in however long you spent in there lands while somebody else is
still settling.

The menu-sampling fix below covers exactly this for the camera's aim and its
zoom, and the lens was left out of it. It is in now: same rule, same
half-second. It matters more here than it
looks, because that number is not only what gets put back at the end of a
conversation — it is also the lens every angle without an opinion of its own is
composed against. One bad reading at a shop door and the next conversation was
framed on it.

### The settings panel was hiding the bars while you configured them

Reported: *"i set the black bar from the last version zero, and now black bar
cannot be brought up no matter what i set in skse menu, ticking the black bar
switch, or setting any bar value"*.

The bars are drawn over the finished frame, from a hook that keeps running while
the game thread is stopped — so that hook has to decide for itself whether they
belong on screen. It decided by asking whether the game was paused.

That is a broader question than the one it meant. The pause counter goes up for
the **console**, which this mod deliberately treats as an overlay over a scene
that has not moved. It goes up for any overlay a mod puts up that freezes time.
And it goes up for **SKSE Menu Framework's own settings panel**, whose
`FreezeTimeOnMenu` option is shipped as `true` by more than one mod that bundles
the framework.

So the bars were forced off the screen for exactly as long as the panel that
configures them was open. Tick Black Bars, drag Bar Height, watch nothing
happen. Both settings were applying live the whole time and neither could be
seen.

The hook now asks the menu watch — which already knows the difference between a
menu that owns the screen and an overlay over a scene that is still there, and
which already catches the crafting screen that owns the screen without pausing
anything. Open the panel mid-conversation now and the bars stay up and move as
you drag them.

### Turning the bars on gives them a height

Before 1.4 the only letterbox control was the height, so "I do not want bars"
was said by dragging it to zero. 1.4 added the switch the setting always needed,
and left anybody who had done that with a switch that does nothing when ticked —
bars of zero height are invisible however the switch is set.

Switching them on at zero now sets the shipped 12%. Only from zero, and only on
the tick: a height you set is never overwritten, and neither is a zero you are
looking straight at with the switch already on.

### Scene Director no longer writes a camera it has handed back

Reported: exiting a trade menu left SmoothCam's settings corrupted.

Handing the camera back at the end of a conversation was three separate writes
in the wrong order. Scene Director told SmoothCam *"the camera is yours again"*
and then, on the far side of that, set the field of view, wrote nine fields of
the third-person camera state, and changed the camera state outright. For
however many frames that took, two mods believed they owned one camera — and the
one that had just been told it did was the one being written over.

One of those nine fields is the game's **persistent** third-person zoom. It is
not a value that gets corrected on the next frame; it is the number the camera
keeps, so whatever is managing that camera carries it forward from there.

Every write now happens **before** the hand-back, and the hand-back is the last
thing the close does. Nothing was removed and nothing about the view you are
left in has changed — only the order.

### And it does not write it at all for a menu

A trade is not the end of a conversation. Scene Director steps out of the way,
the merchant's stock takes the screen, and two frames later the same
conversation resumes on the same angle — so there was never anything to put
back. It was putting the resting aim and zoom back anyway, on the way in, and
taking them again on the way out: the persistent zoom written twice per trade,
against a camera another mod was in the middle of re-establishing.

Suspending for a menu now leaves the view exactly where it is. The resting state
is still restored in full when the conversation genuinely ends — including the
case where it ended *while* the menu was open, which is the one path that would
otherwise have been left holding the engine's dialogue aim.

### And it stops taking readings while a menu owns the screen

The resting aim and zoom that get restored are sampled every frame the mod is
idle. The frames just after a screen-owning menu closes are the worst possible
moment to take that reading: the once-a-frame update does not run while the game
is paused, so the first frame back is the first sample in however long you spent
in a shop, and it lands while the camera is still settling rather than at rest.

Taken there, a transient nobody ever saw would be recorded as your resting camera
and stamped back at the end of the next conversation. Menus are now skipped
outright, and so is the first half-second after the last one lets go.

**If your SmoothCam settings are already wrong**, this stops it happening again
but cannot undo it — restore your preset in SmoothCam's own MCM, or reinstall the
preset mod if you use one.

## 1.4.0

A settings panel you can read, a look that ships already chosen, two ways of
changing angle that finally do what they say, and the inventory bug.

### Your inventory is yours again

Reported: *"I reverted to version 1.2.2 because the newer versions clip the
inventory header when viewing inventories in alone view and during
conversations. Version 1.2.2 automatically exits the cinematic for inventory
viewing and smoothly returns to the cinematic once the interaction with another
NPC is complete, whether it's for viewing or trading."*

This is the 1.4 blocker and it is fixed.

Opening an inventory, a container, a merchant's stock or a crafting station now
hands the whole cinematic back **before** the menu draws: the camera, the field
of view, the black bars and the HUD. Nothing of this mod is on screen while a
menu owns it, so nothing of this mod can be across the top of it.

Two things were wrong. The first is that the bars eased away over a third of a
second, and a third of a second of black over the top of an inventory is a
clipped header — they are taken off in one frame now. The second is that one
menu in the game owns the screen without pausing it, which is the crafting
screen, and every check this mod had for "somebody else has the screen" asked
whether the game was paused. Smithing and enchanting opened from a topic kept
the full cinematic frame over the top of them.

**And it comes back properly.** This used to end the conversation outright and
let it stage again from the top, which meant a new opening angle every time you
closed a merchant's stock. A conversation interrupted by a menu is now
*suspended*: when you close it the same conversation resumes, with the same
partner, on the same angle. If the conversation genuinely ended while the menu
was open, it does not come back at all.

Opening your own inventory while alone has never started a cinematic and still
does not.

### Talking to somebody who is already talking

Reported: activating an NPC who is mid-line does not start the conversation
properly, and "Select dialogue Text" repeats.

Leaving a conversation while the NPC is still speaking is meant to keep this mod
watching until they finish — cutting away from a farewell is the worst thing a
dialogue camera can do. What it also did was make the *next* conversation with
that same person invisible: the camera is staged once per partner, the partner
had not changed, so nothing staged. You got a second conversation with no
camera, no bars and a dialogue list nothing was driving.

Conversations are now counted rather than identified by who you are talking to,
and re-entering dialogue with somebody who never stopped talking ends the old
one exactly once and starts a new one. Forcegreets, scripted scenes and every
case where the game correctly refuses to talk to you are untouched — nothing
here forces a menu open, intercepts a click, or retries anything per frame.

### The camera changes angle every few lines

**"Also cut when a line ends" is gone**, and the behaviour with it. A Skyrim
exchange has three edges close together and all three used to change the angle:
they start talking, they stop talking, you pick a topic and they start again.
The middle one was always the weakest — nothing has happened except that a
sentence finished — and on a short line the three landed inside about two
seconds. The angle a line was framed on is held through the pause after it now.

The camera still moves to whoever has started speaking. That is shot and reverse
shot, it is what this mod is for, and it is a different rule: it fires only when
the angle on screen is framing the wrong person and it always lands on the other
one.

**Per line and timed angle changes are two separate features**, and either,
both or neither is a legitimate setting.

* **Per Line Angle Change** counts lines and changes angle after three to six of
  them, re-rolled each time so the rhythm is not countable. On by default. This
  is the one that never worked: the dials shipped at one, and the turn changing
  asked for an angle on its own account, so raising them did nothing you could
  see. The turn no longer asks.
* **Ignore Short Lines** is on now, and it means ignored. A run of "Yes." and
  "Hmm." no longer walks the camera toward its next angle.
* **Timer While Talking** and **Timer While Choosing** are both off, which makes
  every angle change in the mod a motivated one.

### Close is the look this ships with

A clean install comes up on **Close**, and the Presets page says so.

Close is rebuilt and is six angles rather than ten: over your shoulder, a close
single on each of you, an extreme close on each of you for the lines somebody
means, and one shot from across the room on a long lens, drawn about one line in
twenty-six. Everything is on 35 to 50 degrees, which is what makes it read as
compressed rather than merely near.

The shipped `SD.ini`, the built-in defaults, the reset button and the preset
itself are now four statements of one set of numbers rather than four that
happened to agree. Applying another preset and coming back to Close gives
exactly Close, every time.

Your own settings are untouched by the update, as always — anything you have
changed in the panel lives in `SD_user.ini`, which no release has ever shipped
and no update has ever overwritten.

### The settings panel

Rebuilt.

**Every name is on the left and every control is on the right**, in a grid that
reflows with the window instead of at pixel offsets that were only ever right at
one resolution. That is what the report of a shot name drawn straight through
the control beside it was.

* Values read in their own units, inside the control. Durations say `8.00 s`.
  Bar height says `12%` instead of `120`.
* Icons on every page and every action that matters, always beside the word
  rather than instead of it.
* Controls that depend on something switched off are visibly switched off, and
  the section says why rather than leaving you to work it out.
* **Shots** groups are **NPC**, **PC** and **Room**. Inside an angle: **Effect**,
  **Amount**, **Duration**, **FOV** and **Frequency**. Set Effect to **None** and
  the two dials that mean nothing without it are not drawn at all.
* **Camera** is two collapsible sections, one per way of changing angle, plus the
  holds under both and the framing rules.
* Far less blue. It marks the page you are on and the one action worth pressing,
  and nothing else.

### Your options wait for you to finish speaking

**Fade After PC Line** is on, holds your dialogue options up for as long as your
own voiced line is playing, and starts the fade when it ends.

The old setting was called "Hold until your voice ends" and stored the opposite
of what it said — ticking the box that promised to hold was what stopped it
holding, and the shipped default meant the hold never ran for anybody. It is a
new setting rather than a relabel, because inverting somebody's saved value on
their behalf would be wrong for anyone who had already worked the old meaning
out.

Under **Dragonborn ReVoiced** the end of your line is announced, including a
line you skipped, so the hold releases on the frame you skip it. Under DBVO or
vanilla it is read from your character's own sound handles. Either way there is
a fifteen-second ceiling: a signal that never arrives costs you one delay rather
than a dialogue list nailed to the screen.

Held state is cleared on a skip, on the menu closing, on walking into a
different conversation, and on a load.

Fade delay is 1.5 seconds and the fade itself 2 seconds, both slower and
gentler than before.

### Faces are the game's again

**Expressions**, **eye contact** and **the player head draw flag** are no longer
this mod's to write, and the controls for them are gone.

All three worked. All three also wrote channels that belong to the game and to
whatever face mods you already run — an expression stamped over theirs every
frame, an averted gaze layered on the engine's own head tracking, your head
pinned into the drawn set for the length of a conversation. This is a camera
mod. Old settings for any of them are ignored rather than migrated.

What is left on the Faces page is the one thing nothing else does: your own
mouth and brows, which vanilla never animates because vanilla's player never
speaks. **Lip Sync Fallback** is on by default and warns that it is for DBVO
only — Dragonborn ReVoiced drives your mouth itself and does it better.
**Eyebrow Movement** is on.

### The Light page is gone

The lighting rig is still here, still off, and still works. What it stopped
doing is asking a question: the page opened by wanting to know which of several
lighting rigs you would like, and the honest answer to that is "I do not know, I
wanted the faces to look better".

Set `[Lighting] bLights=1` by hand if you want it. No preset switches it on, and
per-angle lighting still appears in the shot editor with `bPerShot=1`.

### Black bars are a switch you can find

`bLetterbox` decided at launch whether the drawing hook was installed at all, so
the bars could not be turned off without quitting Skyrim and editing a file, and
there was no control for them anywhere. It is a live setting on the Screen page
now, and bar height is set in whole percent.

## 1.3.6

Every menu a conversation can put in front of you — the game's own, and other
mods'.

### "It quickly disappears and the camera pops back toward my player"

Reported: *"When I pick a dialogue option to open Training (like with Faendal in
Riverwood) it quickly disappears and the camera pops back toward my player
without any option to do anything but tab out of the conversation."*

Some dialogue options do not lead to another line — they open a menu. Asking for
training, asking to trade, offering a gift, being handed a book. Every one of
those menus pauses the game, and a paused game stops the once-a-frame update
this mod does all of its thinking in.

So Scene Director did not end the scene when the training menu appeared. It
**froze**, still holding everything a staged conversation holds — including the
topic list, which it had just taken off the screen because you had made your
choice. When the menu closed and the world started again, that list was still
hidden, and the part of the mod that would normally bring it back had long since
decided the conversation was over. You came back to a conversation with nothing
in it and a camera that had let go, and Tab was the only key that did anything.

Scene Director has a rule for this and always has: *anything that pauses the game
has taken over the screen, so hand it back*. The rule could never fire, because
by the time such a menu exists the code that checks for it has already stopped
running. The check has been moved to the one place that keeps running while the
game is paused — the menu watch — so it now happens **before** the freeze rather
than never.

Pick a training topic now and the scene is handed back in full first: the camera,
the lens, the bars, the HUD and the options are all yours before the menu opens.
When you close it, the conversation stages again from the top, so the options
come back with it. Trading, gifts and books all took the same path and are all
fixed by the same change.

Opening the console mid-conversation is deliberately not treated this way. It
pauses the game like anything else, but it does not take the conversation with
it, and ending a scene because somebody looked something up would be worse than
the problem.

### Mod confirmation pop-ups are not hidden any more

Reported: *"Now that the UI is defaulted to being hidden, it blocks usage of mods
that add confirmation dialog pop ups. Like using NFF's [Import Into Framework]
dialog option brings up a confirmation popup but now this mod hides it completely
so you just kind of sit there staring at each other with no dialog options and no
UI."*

Clearing the screen for a conversation means taking down whatever else is on it —
a compass replacer, a stamina bar, a durability readout, a damage-number overlay.
Scene Director did that by hiding **everything** it did not recognise by name,
against a list of twelve. That list is the set of things this mod must not touch;
it was never a list of what *you* might need, and there is no version of it that
could be, because any mod may register any menu under any name.

So a follower framework putting up a confirmation box during a conversation had
its box hidden — and then held hidden, because the same sweep re-asserts itself
every quarter second. A question you could not see, waiting for an answer.

It now asks the menu what it is instead of what it is called. A menu that pauses
the game, claims modality, wants the cursor or takes the menu control context is
one somebody is expected to answer, and it is left alone. Widgets are none of
those things, so everything that was correctly cleared before is still cleared.
Anything spared this way is named in the log, so a widget that starts surviving
conversations takes one line to move back across.

This only started in 1.3.5. Before that the whole sweep sat behind the HUD hide
setting, so switching that off switched this off with it.

### The topic list can no longer be left invisible

The other half of the report, and worth fixing separately because it is the
older bug of the two.

When a conversation ends, Scene Director puts the topic list back at full
opacity. That hand-back writes into the dialogue menu — and it is called from
places where the dialogue menu has already gone, in which case the write reached
nothing at all. It was still recorded as done, so it was never tried again. A
list left that way is invisible and still clickable, which is the single worst
state this mod can leave the screen in.

The hand-back is now only marked done when it actually lands, and it is retried
every frame until it does. If it ever takes more than one attempt the log says
so, once, naming it.

### Dragonborn Voice Over: your mouth now knows which line it is saying

This is for the people running DBVO 1 or DBVO 2 without Dragonborn ReVoiced.
Nothing changes if you have ReVoiced — it already tells this mod everything
below, and it is still the better source because it also announces when a line
is cut short.

To move your mouth, Scene Director needs three things: that you spoke, when you
started, and how long the recording runs. With ReVoiced all three arrive as
events. Without it, Scene Director was reduced to watching for a sound starting
somewhere on your character and then *guessing* which file it was — rebuilding
the filename from the words on screen and trying that name in every voice pack
folder you have installed, taking whatever turned up first.

That guess missed on roughly half the lines in the last recorded session. When
it missed, there was no length to work from. When it landed in the wrong pack —
and this test machine has fifteen installed — it measured a completely different
actor reading the same words at their own pace. And because a sound playing on
your character is not necessarily *you speaking*, your mouth moved through lines
that had no recording at all.

None of that has to be guessed, because your voice mod says the answer out loud.
Every version of DBVO plays your line by asking the game to speak a specific
file, by name. Scene Director already listened to that request to learn which
voice pack you had chosen; it now reads the rest of it.

So on a DBVO profile your mouth now starts when the line starts, runs for as
long as that exact recording runs, and stays shut when you did not speak. Lines
whose filename could never have been guessed work like any other. It also means
players of non-English versions get a correctly timed mouth without the mod
having to read a single word of the dialogue.

If your voice mod does not go through that request, nothing changes and the old
behaviour continues untouched — Scene Director waits until it has actually seen
one of your lines announced before it trusts the new path at all.

## 1.3.5

A clean frame that keeps your notifications, a camera that hands the view back
where you left it, and a mod that reads Skyrim's words in whatever alphabet they
were written in.

### The camera no longer hands you back staring at the floor

Reported: *"after the dialogue ends, the third person camera snaps to a different
angle, most of it were looking down completely. It feels disorienting."*

The snap was not Scene Director moving the camera. It was Scene Director
**stopping**.

Staging works by stamping the camera node every frame with a pose of its own, so
for the length of a conversation nobody sees where the *engine* thinks the camera
is — and the engine spends that whole time aiming its own dialogue camera at the
person you are speaking to. Talk to a seated blacksmith, a child, or anyone much
shorter than you and that aim is pitched hard down. Vanilla eases out of it when
the menu closes and you watch it happen. Here the last directed frame was
followed immediately by the engine's, so you never saw the ease — you saw the
pose it starts from, arriving as a cut, and that pose is a camera looking at the
ground.

Scene Director now reads the third-person camera's aim, zoom and shoulder offset
while nothing is staged — the only moment those values are the player's own — and
puts them back when the conversation ends, the same way it has always put the
field of view back. You are handed the view you walked up with.

The last shot still cuts rather than gliding back, the way vanilla cuts when a
dialogue camera has moved. What has gone is cutting to the wrong place.

Every conversation now writes one line to the log naming the aim it handed back,
the zoom, and your character's own pitch — so if this ever misbehaves again, the
number responsible is already written down.

### The camera cuts, and your face reacts, in every language

Two more places where Scene Director was reading Skyrim's words as bytes instead
of as characters. Both are fixed, and neither changes anything if you play in
English.

**The camera stopped cutting in Japanese and Chinese.** "Hold on short lines"
counted words by looking for spaces. Japanese and Chinese do not put spaces
between words — 少し待ってくれ is seven characters, three words and no spaces at
all — so every line in the game counted as *one* word, landed under the
four-word floor, and the camera held its angle for the entire conversation. Turn
the setting on in a Japanese game and it read as the mod having quietly died.

Runs of kana and CJK characters are now measured by their length instead. On the
shipped build: はい。 counts 1, わかった。 2, 少し待ってくれ。 4,
我不知道你在说什么。 5 — interjections under the floor, sentences over it, which
is the same shape the English count always had. Lines that mix scripts are
counted both ways at once. Korean and Russian are untouched: they space their
words like English and were always counted correctly.

**Your face ignored every question you asked.** The expression reader looks for a
question mark and pulls a puzzled face — its own note calls that "the rule that
earns most of this function's keep", because a question is the one emotional cue
that survives translation. It was looking for the ASCII `?`. Japanese and Chinese
write it `？`, which is three bytes and contains no `?` at all, so it never fired
once. Same for `！` driving emphasis, and for the Spanish `¿` and `¡`.

All of them are read properly now. `!!` still means two marks *in a row* rather
than two anywhere, so "No! Get out!" stays emphatic and "No!!" stays a shout.

The keyword list behind the other emotions is still English, and stays English —
sixty phrases in nine languages would be nine translations nobody here can check.
On a non-English profile it scores zero and the punctuation rules do the work,
which is exactly why those were worth fixing. It costs the *player's* face on a
flat statement and nothing else: NPC expressions come from a number in the
dialogue record, identical in every localisation.

### Your mouth stops when your voice does, in every language

If you play Skyrim in Japanese, Russian, Chinese, Korean — anything not written
in the Latin alphabet — the player lipsync has been running the mouth on the
**whole conversation's** length instead of the line's. On a line of a second and
a half that is several seconds of a character still mouthing at somebody in
silence: the same five-times overrun that was fixed for English players a
fortnight ago, still fully present for everyone else.

The cause was one `||`. Scene Director shapes the mouth from the words and takes
the *length* from the recording, and both answers were being thrown away
together. The shape builder reads a–z, so a Japanese subtitle produces no
shapes — and the measurement taken from the actual `.fuz` a line earlier went
out with them, because they shared a single early return.

They are separate answers to separate questions now. Without readable letters
the mouth still falls back to a synthesized cadence, which is correct and
unchanged — but it now **ends on the measured length of the recording**, so it
closes on the last syllable like everyone else's.

The log says which of the two happened, and why, instead of reporting every
non-English line as "no topic text readable".

Nothing changes for English players: the same measurement was already reaching
the same place by the same route.

### The HUD hide is part of the mod now, and it stopped costing you notifications

`bHideInterface` and `bHideHudWholesale` are **gone**. Any value left in your INI
for either is simply not read, and both controls have been taken out of the
in-game panel and the MCM.

The HUD is always hidden during a conversation. That was never really a setting
— a staged shot with a compass, three meters and a crosshair across it is not a
shot, and an option to switch that off is an option to switch off the thing you
installed.

**What changed underneath is the more useful half.** Until now the hide had two
modes and both were wrong in the same direction:

* **By name.** A list of HUD elements to hide, written from vanilla. Measured
  against the three `hudmenu.swf` files on this profile — SkyHUD, Edge UI, Edge
  UI Explorer Addon — that list matched **three of twenty-nine** children. The
  crosshair, all three meters, the activate prompt, the clock and the charge
  meters stayed on screen for every conversation, and the log reported success.
* **Wholesale.** Hide the HUD movie and everything in it goes. Perfectly
  reliable, and it took the notifications with it — an item picked up
  mid-conversation, an objective completing — and those were *lost*, not
  deferred, because the movie is what queues them.

Neither is used now. Scene Director reads the HUD movie's own display list at
the start of a conversation, hides everything it finds, and **releases the two
elements the game uses to tell you something**: the notification stack and the
quest-update banner. So you get a clean frame *and* your item pickups, gold,
skill increases, quest objectives, level-ups and shout words — during the
conversation, when they happened, rather than not at all.

It also means the hide works on a HUD it has never seen, because it is not
matching names any more.

If the movie ever cannot be read, the whole HUD is hidden as before and the log
says so in as many words — and it keeps asking for two seconds, so a HUD that
was simply a frame late gets its notifications back rather than losing them for
the conversation.

### The camera can stop flinching at things that walk past

New setting, off by default: **Ignore obstructions mid-shot**
(`bHoldPlacement`), on the Direction page under Framing.

Until now the camera measured the room in front of it on every single frame it
was on screen. That is what lets it back away from a wall the conversation walks
into — and it is also why a cart crossing behind the lens, or a guard walking in
front of it, eases the camera in and then lets it drift back out. The angle was
already fixed at the cut; the distance was not, and it moved for reasons nobody
watching could see.

Turn this on and every check still runs when the angle is chosen. The camera is
placed clear of walls, out of collision, with a line to whoever it is framing —
and then the shot holds. People and carts pass through frame and the camera
stays where it was put.

**The cut is not affected, and cannot be.** The setting only reaches a shot that
has already placed, so the frame that decides where the camera stands runs the
full sweep, the wall margin, the crowd test and the refusal exactly as before.
There is no configuration in which this puts the lens inside a wall at a cut.

**What you give up.** A held shot will not get out of the way. If the two of you
walk somewhere while talking, the camera follows at the distance and bearing it
settled on, through whatever is between. Best in a market or on a street, worst
in a conversation that moves — which is why it is a setting and not a fix.

It is also cheaper: a held angle casts no rays at all, and skips the crowd walk
with them.

### Settings files saved with a byte order mark

If you have ever hand-edited `SD_user.ini` and your editor saved it as "UTF-8
with BOM", the first section of that file was being ignored — silently, with
nothing in the log to say so.

Windows recognises a UTF-16 byte order mark in an ini and no other. A UTF-8 one
is three bytes it treats as ordinary text, so they end up glued to the front of
line one. If line one is a `[Section]` header, that header stops matching and
every setting under it falls back to its built-in default. Sections further down
the file are unaffected, which is what made this so hard to see from the
outside: *some* of your settings revert and others do not, with no pattern.

`SD.ini` was never affected — it opens with a comment, so the mark lands
harmlessly there. `SD_user.ini`, which is the one the in-game menu writes and
therefore the one people open when settings look wrong, begins with a section
header, so it was. Changing a setting did not fix it either: the game cannot see
the shadowed header, so it appended a second copy of the section and left your
original values stranded above it.

Both files are now checked once at startup and repaired if a mark is found —
removed outright, or the file rewritten as UTF-16 if it contains non-ASCII text,
which Windows reads correctly and which keeps a preset you named in your own
language intact. The repair is logged, with the path.

### A push-in on a tight angle could ask to stand closer than the floor

Internal. Every angle has a minimum distance it may stand at, and the framing
solver respects it — but the push-in is applied afterwards and takes up to forty
per cent off. A close angle already near the floor, with Amount turned up on the
Shots page, therefore arrived asking for less than the minimum, and the clamp
that was supposed to catch it was being handed a lower bound above its upper one
— which is undefined behaviour rather than a clamp.

It resolved to the floor in practice, which is the intended answer, so nothing
was visibly wrong. Now it says so in a way that is defined.

## 1.3.4

Japanese, Chinese and Korean installs, which have been crashing on startup.

### The crash

If your Windows user folder, your Skyrim install path or your mod manager's
folder contains characters outside your system's legacy codepage — which is
normal on a Japanese, Chinese or Korean system, where the user folder is often
your own name — this mod threw an exception the moment it loaded:

```
std::system_error
"No mapping for the Unicode character exists in the target multi-byte code page."
```

It happened before any hook was installed and before any conversation, so
nothing about the camera was involved. The plugin was turning the path to its
own log file into a narrow string through the legacy codepage, and that
conversion throws rather than failing quietly when a character has no mapping.

Two places did it: the log file's path, and the line that reports where the DLL
was loaded from. Neither can throw now, and neither uses the legacy codepage for
anything it does not have to.

The log itself also stopped being able to take the mod down with it. If the file
cannot be created for any reason at all, you now lose the log and keep the mod,
which is the correct trade for a diagnostic.

Thanks to the reporter who tracked this down and wrote it up properly — the
mechanism was exactly right.

### Your voice lines were being measured wrong too

Found while fixing the above, same cause, no crash. Scene Director measures how
long your spoken line lasts by finding the recording on disk, and it was
building that filename from text the game supplies as UTF-8 while Windows read
it back as the legacy codepage. A Japanese topic, or a voice pack in a folder
with a Japanese name, addressed a file that does not exist.

It never failed loudly — it just missed, and fell back to guessing the length
from the number of characters in the line. So on a non-English install every
line your character spoke was timed by estimate rather than measured. That is
fixed.

## 1.3.3

Where the camera stands, and a settings menu that stopped explaining itself.

### The camera looks before it stands somewhere

It was measuring the room carefully and then throwing almost every measurement
away. Eight things came out of that, and the ones you will notice first:

**It picks the best angle now, not the first one that fits.** When it was time
to change angle, the camera asked one question about each option — can I
physically stand there? — and took the first yes. An angle jammed against a wall
at its closest allowed distance, swung eighty degrees off where it was supposed
to be, scored exactly the same as one standing in open space at the size it
asked for. In small rooms, which is most of Skyrim, that is the difference
between choosing well and taking whatever came up first.

**Halls and big rooms get big shots.** Whether wide angles were allowed at all
came from two measurements, taken straight out to the left and right of the
conversation. Stand in a long hall talking to somebody *down* its length and
both of them hit the side walls a few feet away, so the mod decided there was no
room and switched off half its vocabulary in one of the best-looking spaces in
the game. It now uses twelve measurements it was already taking and discarding.

**The breathing zoom is fixed at the cause.** The camera easing in and back out
for no visible reason was one pencil-thin measuring beam flickering on and off
as somebody walked past. It measures across the width of the shot now.

**Nothing ever checked you could actually see them.** Over-the-shoulder shots
with the shoulder behind a beam, doorframes landing exactly where the framing
had deliberately been pushed. Both are tested now, and so is whether a third
person is standing in the shot — a guard's back filling the frame in a market
was invisible to it before.

**Nothing ever looked up, either.** Overhead angles punching through low
ceilings were held back only by a hand-tuned number. There is one measurement
now.

### You keep your side of the screen

Film crews call this the 180-degree rule and it is the one they do not break:
keep the camera on one side of the line between two people, and you stay on the
left of frame while they stay on the right, however many times the angle
changes. Cross it and the two of you appear to swap places.

Scene Director had the rule written down, and every angle in it was authored to
respect it — and then the search that looks for somewhere to stand could put the
camera on the wrong side anyway. Every angle in the mod could be moved across
the line it was declared to obey.

### Two keys

Set them under **Keys**. Both start unassigned, and they take any key you press
rather than a list somebody else chose.

- **Next angle** — change angle now, without waiting.
- **Who to look at** — first press goes to whoever is *not* on screen, so it
  always does something. It hands back to automatic at the end of the turn, so
  looking at yourself during their line no longer strands the camera on you
  through their reply.

### No opening shot

Conversations opened on a shot of the two of you standing apart, held it, then
cut to them, then cut to you — three angles in about two seconds, played across
the start of a line nobody had finished saying. It is gone. The camera is on
whoever is talking from the first frame, and on you when nobody is.

### Your face while you speak

Measured against a real NPC: hers ran five facial morphs at once, the strongest
at full. Yours ran exactly one. That is why you looked posed next to somebody
who looked like they were talking, and it was never a strength problem — turning
one morph up gives a stronger mask, where what an NPC has is anger with a trace
of disgust and a knit brow underneath.

Yours is a blend now. The squinting comes from the same place, which is why it
is not an eyebrow setting: these are whole-face shapes, and anger and disgust
narrow the eyes as part of what they are. A smile carries a little of it, which
is the difference between a real one and a rictus.

Ordinary lines also stopped sitting still. Most of what you say matches no
emotional keyword, which resolved to a neutral face at a strength low enough to
see nothing — on the majority of everything you said.

### Three presets instead of five

The old five overlapped so heavily that the descriptions were doing the work of
telling them apart. Two of them shared eight angles, and all five changed angle
on every single line, so the rhythm — the thing you actually feel — was
identical across every one of them.

**Standard**, **Close** and **Room**. Two to four angles per side, each holding
between two and five lines. Short enough to describe in a sentence, which is the
test they were rebuilt to pass.

### The settings menu

Every control carried three to six paragraphs behind a question mark. All of it
was true and almost none of it was for somebody opening a menu for the first
time. It is gone — the label says what it does, and that is all.

Checkboxes are in two columns, so the pages are about half as tall. Pages are
named for what is on them rather than what the code calls them: **Camera**,
**Screen**, **Faces**, **Keys**. Shots is untouched.

- **Turning the mod off is a checkbox now**, at the top of Camera. It used to
  need quitting Skyrim and editing a file, because the setting was only read
  once at launch.
- The opening-shot slider and the open-on-speaker toggle are gone with the
  behaviour they controlled.

## 1.3.2

Your character's face. It turns out almost nothing about it was working
properly, and the parts that were had been undone by the parts that were not.

### Your mouth stops when your voice does

It had no idea when to stop. The sound the game reports as "still playing" never
switches off, so the mouth ran until an eight-second safety cutoff — on every
line, including one-second ones. Your character carried on mouthing for five
seconds after they had finished speaking.

### Every line was timed 15–50% too long

The length of a line is read out of the voice file, and it was being read wrong
— on every line, for as long as the feature has existed. The audio is
compressed, and the number being divided by describes the compressed size rather
than the sound.

```
"You father was a traditionalist?"        read as 2.97s   actually 1.95s
"I imagine he didn't react well."         read as 2.23s   actually 1.95s
"Given your family history, I take it..." read as 4.83s   actually 3.95s
```

So the mouth was spreading a two-second line across three. That is where "the
timing is off" came from.

### It was following the wrong recording

With several voice packs installed, your line was often being measured against
**somebody else's performance of it** — a session playing Bella timed a line off
the Vampire pack's copy. A different actor at a different pace, so the mouth was
following a performance you could not hear.

Scene Director now reads which pack is playing out of the call that plays it.
Nothing to configure and no restart: it picks this up from the first line you
speak, and falls back to the old search if it cannot tell.

### The lips barely moved

The strength setting was being applied to the whole face. It is a jaw control —
the shape table deliberately keeps the jaw low and lets the lips carry each
sound — so turning the jaw down was also flattening the rounding on "oo" and the
lip-bite on "f". The jaw still follows the setting exactly; the lips now keep
much more of their movement at the same value.

### Your face moves with your words

The largest of these, and the one you actually see.

Your expression was set once when a line began and held, flat, until it ended.
Measured against a real NPC mid-conversation: hers reached full strength on her
strongest expression and moved across several others as she talked, while yours
sat at half strength in one slot for the whole line. That is a mask, not a face.

Your expression now swells on the syllables you stress and settles between them,
using the same phrasing the mouth is shaped from — so the whole face moves
together on the word the voice lands on. A neutral line stays neutral; there is
nothing to swell when there is no feeling behind it.

### Your eyebrows move when you speak

New, and off by default — **Faces → Your eyebrows**, or `bBrowSync`.

NPCs move their brows constantly while talking and your character never has,
because the game does not expect you to speak. Yours now lift as you open a
phrase and on stressed words, climb through a question, narrow slightly on
emphasis, and pull down and together when you are angry.

Blinking and head tracking are untouched — only the brow and squint controls are
written, and only while you are speaking.

### Quest updates and items you pick up show again

Scene Director cleared the HUD during conversations by hiding the whole thing,
which also hid everything the game uses to tell you something happened — quest
updates, items received, skill increases. Those were not delayed, they were lost.

It now hides the furniture by name — compass, meters, crosshair, activate prompt
— and leaves anything it does not recognise alone. An unusual HUD replacer may
leave a stray meter on screen during a conversation; that is the better mistake.
The old behaviour is still available as `bHideHudWholesale`.

### Your dialogue options fade again, whatever else you have switched off

"Fade the options out" did nothing unless "Hide the rest of the HUD" was also
on. Not less — nothing. The whole fade lived inside the HUD setting, so turning
the HUD hide off (a reasonable thing to want, since hiding it wholesale takes
your quest updates with it) switched the fade off too, silently, while the
setting still read as on.

They are two separate things now and either one works on its own.

### The screen settings work while you are playing

"Hide the rest of the HUD", "Hide it all at once" and "Hide the speaker's name"
were read once when the game started and never again. Changing one wrote the
value down and did nothing else; the panel admitted as much with a caption
telling you to restart. All three now take effect the moment you click them,
including in the middle of the conversation you are in.

### The HUD stays hidden

Hiding the HUD was a single instruction given as a conversation opened, and
things that came back afterwards stayed back — a widget that reopened, a compass
returning after a cell load, anything a HUD mod redrew on its own schedule. It
is now held down for as long as the conversation lasts, and anything that turns
up partway through is caught within a quarter of a second.

If the HUD was not ready at the moment a conversation started — which happens
after a load or a fast travel — Scene Director gave up on it for that entire
conversation. It now keeps trying until the HUD is there.

### The options no longer freeze when the camera leaves

The fade was driven from the camera, so anything that took the camera away
stopped it dead: first person, a mount, a chair, a killcam. The options would
freeze half-faded and stay that way. It is driven from the conversation now, and
the camera has nothing to do with it.

### Dragonborn ReVoiced

If you use **Dragonborn ReVoiced 1.4.4 or newer**, Scene Director now asks it
directly when you start and stop speaking rather than working it out by watching
the game. ReVoiced knows things nothing else can tell us — the exact file, the
exact length, whether you skipped the line, and how long the conversation waits
afterwards — so this is the most accurate the mouth gets.

Everything above still applies without it. Dragonborn Voice Over 1 and 2 are
fully supported and are not going anywhere; ReVoiced simply answers questions
the other path has to infer.

## 1.3

Ten commits since 1.2.6. The short version: a preset now decides everything
about how a conversation is shot, the five of them were rebuilt from scratch,
every angle got a plain English name, and dragons work.

### The five presets do far more than they did

A preset used to pick which angles were in play and set the cutting speed. That
was not enough to make five looks that actually felt different — they all drew
from the same angles, shot on the same lenses, and moved the same way.

Each preset now decides four things about every angle it uses:

- whether it is in play
- what it does while it is on screen, how much, and for how long
- **what it is shot on** — new
- **how often it comes up** — new

The lens is the big one. The same angle on the same person is a completely
different picture depending on whether the camera is standing well back and
zoomed in, or standing close and taking in the room. Every preset now sets that
per angle, so *From a Distance* and *Up Close* can share half their angles and
still look nothing alike.

How often mattered just as much, because it decides how much of a conversation
is spent on the room rather than on faces. Two presets were being undercut by
it:

- **Show the Room** was spending about two thirds of every speech on faces, with
  its tightest angle coming up more often than any of its wide ones. The preset
  named for the room barely showed it.
- **From a Distance** was drawing its close-up as often as everything else, when
  a close shot from across a room should be a rare punctuation mark.

Both are fixed. Roughly how much of a conversation each preset now spends on the
room rather than on the two of you:

| Preset | Room |
|---|---|
| Over the Shoulder | about one cut in seven |
| Up Close | never |
| From a Distance | about half |
| Always Moving | never |
| Show the Room | two thirds at first, about half once a speech runs on |

### All five were rebuilt

Not retuned — rebuilt, angle by angle, against what each angle actually does.

- **Over the Shoulder** — nothing wider than a portrait lens anywhere in it, so
  the camera never looks like it has been put somewhere odd. Both close-ups
  creep forward now, not just theirs, so the pair still match. Very short lines
  are left on the angle already up; nobody cuts for "Yes."
- **Up Close** — everything is wide and near, except the very tightest angles,
  which physically cannot be (see below).
- **From a Distance** — nothing over 50 degrees. Long, flat and still.
- **Always Moving** — every one of its seventeen angles now has something
  specific to do. Four of them used to arc around their subject, which swung the
  shoulder the shot is named for straight out of frame; they rise and sink now.
- **Show the Room** — was missing two of their angles while carrying the
  matching two of yours, so their half of a conversation had one close angle to
  come back to against your three. Both added. Even the close shots stay wide,
  so cutting to a face does not feel like landing in a different scene.

### Every angle has its own movement

Movement used to belong to the shot table with one global dial scaling all of
it. Each angle now carries what it does, how much, and how long, and all three
are yours to change.

Fourteen movements, including two new ones that are easy to confuse and are
completely different pictures:

- **Orbit** arcs around the person and keeps pointing at them, so they stay put
  in frame and the room slides behind them.
- **Slide** moves the camera sideways without turning to follow, so the person
  drifts across the frame and out of it.

The amount is now a straight 0–100% of whatever that movement's full travel is,
so switching an angle from a push to an orbit keeps its intensity instead of
jumping.

### Plain names for every angle

Every film-crew term that had an ordinary equivalent lost:

| Was | Now |
|---|---|
| Dirty Single | Over Your / Their Shoulder (Tight) |
| Medium | Head And Shoulders |
| Low Angle / High Angle | From Below / From Above |
| Long | Full Figure |
| Profile | Side On |
| Two-Shot | Both Of You |
| Master / Ground | The Whole Room / From The Floor |
| Distant | From Far Off |

The shoulder shots are now named by **whose shoulder it is**. "Over Your
Shoulder" is a shot of *them*, with the camera behind you. Both sides used to
read "Over The Shoulder", which was the easiest pair in the mod to mix up.

Some names still appear twice, once on each side of the conversation — Close Up,
Head And Shoulders, Three Quarters and a few others. That is deliberate. Those
are the same framing pointed the other way, which is exactly what a reverse shot
is, and matching names are how the page shows the two go together.

### Your own presets

Three slots you can save whatever you are running into, with rename, apply,
overwrite and delete. They are kept visibly separate from the built-in five and
never called presets, because a built-in look is a designed set of angles and a
slot is a photograph of your settings — both useful, not the same thing.

Green when yours is the one running, amber at rest, grey when empty. If your own
slot happens to match a built-in look, yours is the one shown as in use.

### Creatures are measured instead of assumed

Dragons and animals were being framed with numbers written for a standing
person, and one of them broke them outright: the check for whether the camera
has room to stand starts 48 units out, which clears a person and does not begin
to clear a dragon, so every direction came back blocked and the emergency
fallback ran the whole conversation.

Everything is measured from the actual creature now. Big subjects get their own
framing, a gentler camera height so an overhead does not go through the ceiling,
and a step to the side so the lens is not down a snout. A dragon is still framed
over your shoulder, and you are never framed over a wing.

Ordinary human conversations are untouched — that path measures 1.0 and gets
exactly the numbers it always did.

### Fixes

- **The camera stopped swinging and popping.** It was searching for the best
  angle every single frame with no memory of the last one, so two near-equal
  candidates traded places and the camera jumped 36 degrees at a time. The angle
  is now chosen at the cut and held for the life of the shot. Separately,
  anything walking between the camera and its subject used to pop it to the floor
  and back; that movement is now rate limited.
- **Your dialogue options no longer flash, go stale, or wait on a held shot.**
  The list follows the game's own menu state machine instead of nine different
  guesses about it.
- **Saved presets that would not apply.** Settings longer than 127 characters
  were being silently cut in half when read back, which is exactly what a saved
  slot is. Existing slots were written correctly and only ever read back wrong,
  so they do not need re-saving.
- **Retired settings are now swept out of your ini.** Removed settings used to
  sit in the file for ever, reading as though they still did something.

### The menu

- **Presets are a checkbox list.** Tick one to apply it. The one in use is green;
  ticking another unticks it.
- **The Shots page** keeps a row per angle with Enabled, Move, How often, Amount,
  Lens and Duration, plus a Default button that puts an angle back to how it
  ships without touching whether it is switched on.
- Every group can be collapsed, and the ones worth seeing are open by default.
- Groups say how many angles are on, have All on / All off, and warn you in
  colour when a group is empty — which is a real fault rather than a preference,
  because the camera then has nothing to use for that half of a conversation.
- Help text throughout was rewritten for somebody who has never seen a film set.
- `FOV` is now called `Lens`, and `Weight` is now `How often`.

### Settings file

- Six keys per angle, all documented in SD.ini: on/off, how often, lens,
  movement, amount and duration. A preset writes all six.
- `i<Name>Zoom` is retired. It is read once to carry a pre-1.3 choice over to
  `i<Name>Move` and does nothing after that — safe to delete.
- `iDollyAmount`, `iDollyWindow`, `iRoomSwing` and `iLensBias` are gone. Each of
  them moved every angle in the mod at once, which flattens the very thing it
  adjusts; all four are per-angle settings now.
- Saved slots changed format to carry the lens and how-often. Slots from older
  builds still load, keep whatever they had no opinion about, and quietly
  re-save themselves the first time you use them.
- Your own settings still live in `SD_user.ini`, which no update ever touches.

### For anyone reading the source

- `Preset` gained `lenses` and `weights`, resolved by `PresetLens()` and
  `PresetWeight()` alongside the existing `PresetMotion()`. Apply, drift and the
  menu all call the same three functions, because they disagreed once and a
  preset that applies one set and is measured against another can never report
  itself as active.
- Five static asserts hold the preset tables together: every angle a preset uses
  must name a lens and a how-often, and no lens, weight or motion entry may name
  an angle the preset does not use. All five failures were silent at runtime.
- Lens authoring is bounded by geometry. Distance solves for the fill at the
  chosen lens and is then floored at the subject's minimum, so vertical field of
  view times fill must stay under about 34 degrees or the frame renders looser
  than it was authored for. A push-in multiplies distance after the solve with
  no re-clamp. This is why *Up Close* runs its tightest framings on its longest
  lens — you cannot get a wide lens that close to a face either.
- The `Move` enum ordering is the ini storage format. Append only.
- `bCoverPlayerTurn` is written by nothing — not a preset, not a slot. It is the
  difference between a dialogue camera and a landscape camera.

## 1.2.6

The player gets a face, and weights finally mean something.

#pragma once

namespace SD::Camera
{
	// The camera and the cut policy that drives it.
	//
	// Other Skyrim dialogue cameras work as an offset from the player, which can
	// orbit the player but never stand behind the NPC, so they can't cut to a
	// reverse shot. This writes an absolute world position and look direction
	// instead.
	//
	// The rhythm of a Skyrim conversation is the player's menu turn against the
	// NPC's spoken reply; the dialogue menu and the response cue provide both
	// edges.
	//
	// Tunables: the direction settings, in the units the ini uses (times in
	// hundredths of a second, the letterbox in thousandths of screen height), so
	// the ini, the menu and this struct agree and ApplyTunables does the one
	// conversion. Everything here applies live; other SD.ini keys are only read
	// when a conversation opens.
	struct Tunables
	{
		// Master switch. Live: turning it off mid-conversation hands the camera back
		// through Runtime's normal close path.
		bool enabled{ true };

		int minShotTime{ 240 };
		int minTurnTime{ 25 };
		int maxShotTime{ 900 };

		// Every default in this struct matches the Close preset. A fresh install reads
		// them (via SD.ini or these fallbacks), and the Presets page detects the
		// active look by comparing, so if they differed a clean install would show no
		// preset selected. These initializers, config/SD.ini, the About page's reset
		// and Camera::kCloseStyle must all agree.

		// Black bars. The Present hook is always installed; a retracted letterbox
		// costs one atomic read per frame.
		bool letterbox{ true };

		int letterboxHeight{ 120 };
		int cutEveryMin{ 3 };
		int cutEveryMax{ 6 };
		int playerBeat{ 45 };

		// Hundredths of a second to stay on the player after a voiced line ends before
		// cutting to the NPC. 0 cuts immediately. playerBeat covers unvoiced lines and
		// is timed from the start of the reply instead.
		int playerVoiceHold{ 0 };

		// Reaction shots: after every reactionEvery NPC lines in a reply, a
		// reactionChance percent chance the next line is shown on the player.
		bool reactionShots{ false };
		int  reactionEvery{ 3 };
		int  reactionChance{ 50 };

		// Hundredths of a second the spent topic list stays before fading once the
		// turn has passed. Kept here rather than read from the ini because the menu
		// draws it as a slider, and sliders need live state (re-reading the file every
		// frame would snap the slider back mid-drag).
		int choiceFadeDelay{ 150 };

		// Hundredths of a second the spent list takes to fade.
		int choiceFadeTime{ 200 };

		// Lines this short (in words) don't count toward the cadence when
		// holdOnShortLines is on.
		int shortLineWords{ 4 };

		// Per Line Angle Change, the first of two independent cut modes: count
		// eligible lines and take a new angle after a rolled number between
		// cutEveryMin and cutEveryMax. With both modes off, only coverage moves the
		// camera.
		bool perLineAngleChange{ true };

		// Ignore Short Lines: whether very short lines count toward the cadence. See
		// ApplyCue.
		bool holdOnShortLines{ true };

		// Timed Angle Change, the second cut mode, driven purely by a clock. Off on
		// both sides of the exchange by default.
		bool timedCutsWhileSpeaking{ false };
		bool timedCutsWhileChoosing{ false };

		// Keep each shot on its side of the 180-degree line. The angle sweep could
		// otherwise search a setup onto the far side of the line; on, it's folded
		// back. Off is the old behaviour, for configs tuned before the rule existed.
		// true180 is what keeps the whole conversation on one side.
		bool enforceLine{ true };

		// True 180-degree rule. With one shared `side`, NPC and player shots landed on
		// opposite sides of the line, so both over-the-shoulders used the same
		// shoulder. On, the player's shots turn the other way and the camera never
		// crosses. Implies enforceLine.
		bool true180{ false };

		// Close-ups and tighter singles frame where the head actually is, still
		// anchored to the subject's stable point, with the bearing and height turned
		// toward the face at the cut. Helps most with someone bent over a workbench.
		// [Direction] bFollowFace.
		bool followFace{ true };

		// When the player's voiced line starts, send DBVO's PlayDBVOTopic mod event so
		// player-gesture add-ons fire with the line rather than after it. Never sent
		// while DBVO 1 is loaded, which plays the voice from that event. [Performance]
		// bPlayerGestureCue.
		bool playerGestureCue{ true };

		// An NPC at a bench, smelter, forge, tanning rack and the like stops and
		// stands 3 to 5 seconds after they start speaking. [Performance]
		// bStopWorkToTalk.
		bool stopWorkToTalk{ true };

		// Whether someone standing in the shot lowers its score. The Havok probes
		// ignore actors, so this is done geometrically with a short loop; off skips
		// it.
		bool avoidCrowds{ true };

		// Whether a shot keeps checking for obstructions after it has cut.
		//
		// Off (default): the distance is re-solved every frame against fresh raycasts,
		// which keeps the camera out of geometry but also makes it move in and out as
		// things pass behind or in front of it.
		//
		// On: everything is checked at the cut, then the shot holds and things can
		// pass through the frame. A subject who walks somewhere new takes the camera
		// along at a fixed bearing and distance. Better on a busy street, worse when
		// the conversation moves. The cut itself is always checked; see
		// Shot::Subjects::holdPlacement.
		bool holdPlacement{ false };

		// Admit shots by subject visibility and recover without ending dialogue. The
		// legacy hold preference wins over conflicting ini values.
		bool protectSubject{ false };
		bool firstPersonFallback{ true };

		// The player's turn belongs to the player: while they read topics and while
		// their reply plays, the camera goes to them rather than the room. Off, room
		// shots share that half of the exchange.
		bool coverPlayerTurn{ true };

		// Fade the topic list out while someone is speaking.
		//
		// Hiding the list is safe against the keyboard (ClickGuard blocks the
		// control-map Accept path) but not the mouse: a click still commits the first
		// topic through a route that bypasses the list's own flags. A player-voice
		// framework is the better owner for hiding the tree; DBVO 2's
		// hide_dialogue_tree does that from inside the dialogue input code.
		bool fadeTopicList{ true };

		// Interface settings, live like everything else in Tunables. The HUD itself is
		// always hidden while staged; only the speaker name is optional.
		bool hideSpeakerName{ true };

		// Fade After PC Line. On: the spent list stays up while the player's own
		// voiced line is playing, and the delay and fade start when it ends. Off: the
		// fade starts at the click, like the engine does.
		//
		// This replaces bHideChoicesWhilePlayerSpeaks, whose stored value meant the
		// opposite of its label; that key isn't read.
		bool fadeAfterPlayerLine{ true };

		// Persuade, intimidate or bribe: their answer gets a close-up of them, held
		// for the whole reply with a slow push in. [Direction] bPersuasionBeat.
		bool persuasionBeat{ true };

		// The line being spoken sits centred in the bottom bar and follows it; with no
		// bar, or one too thin for the text, it goes back over the picture. Draws the
		// bars beneath the interface while on. [Direction] bSubtitlesInBar.
		bool subtitlesInBar{ false };

		// Film other people: two NPCs talking to each other are filmed like a
		// conversation of the player's own, either with the Film Their Conversation
		// key or, with this on, once the player has stood still within sceneRange of
		// an exchange in front of them for sceneWait. Moving hands the camera back.
		// [Direction] bFilmScenesAuto, iSceneRange (world units, to the nearer of the
		// two), iSceneWait (hundredths).
		bool sceneAuto{ false };
		int  sceneRange{ 600 };
		int  sceneWait{ 150 };

		// Diagnostic: how ApplyPose writes the camera transform (0 is normal). A
		// tunable so the menu can change it between conversations. See ApplyPose.
		int poseMode{ 0 };
	};

	// Who the camera is on when the player overrides it with the hotkey. Not
	// saved; resets to kAuto when the conversation ends.
	enum class Framing : std::uint8_t
	{
		kAuto,  // the director decides, from who is speaking. Shipped behaviour.
		kThem,  // the other party, whoever is talking
		kYou,   // the player, whoever is talking
		kRoom   // neutrals only: the space, and both of you in it
	};

	[[nodiscard]] std::string_view FramingLabel(Framing a_framing) noexcept;

	class Director
	{
	public:

		// Read the current settings, or apply a new set. Applying takes effect on the
		// next frame, so sliders can be tuned during a conversation.
		[[nodiscard]] static Tunables GetTunables();
		static void                   ApplyTunables(const Tunables& a_tunables);

		// Load the settings from the ini into the live set. Called at startup as well
		// as per conversation, so the menu shows the file's values before the first
		// conversation.
		static void LoadSettings();

		// a_restaging: the same conversation coming back after the director released
		// it while the player was still in it (see Runtime's Stranded()). Skips the
		// per-conversation settings re-read, like a resume does; it's hundreds of
		// synchronous profile reads.
		//
		// a_provisional: staged on a dialogue menu that reopened with only the
		// previous line's speaker known, before the engine hands over the live one.
		// The conversation that follows with the same person is adopted rather than
		// restaged.
		static void Open(RE::Actor* a_speaker, bool a_restaging = false, bool a_provisional = false);
		static void Close();

		// Runs every frame from the always-on frame source, whatever the camera is
		// doing. Release is decided here rather than in the third-person camera hook,
		// which stops firing in first person, on a mount, in furniture or in another
		// menu.
		static void Tick(float a_delta);

		// A response has begun. Carries the emotion, so the policy can decide whether
		// the line has earned a closer shot.
		static void OnCue(RE::Actor* a_speaker, RE::DialogueResponse* a_response);

		// The dialogue menu opened or closed. The menu closing is the signal that the
		// player has left; the session itself ends only after the NPC stops talking.
		static void OnDialogueMenu(bool a_opening);

		// Another menu has taken the screen. Called from MenuWatch, which still runs
		// while a pausing menu is up (Tick doesn't, since it runs in
		// PlayerCharacter::Update). A notification only; MenuWatch owns the state.
		// a_menu is for the log.
		static void OnScreenTaken(std::string_view a_menu);

		// The last screen-owning menu closed. Delivered by MenuWatch from the engine's
		// menu events because the frame source is stopped while a pausing menu is
		// open. Starts the settle window before the camera's resting state is sampled
		// again.
		static void OnScreenReleased();

		// Is a conversation suspended behind a menu rather than finished? Runtime asks
		// on the frame after the menu closes and calls AbandonSuspension if the
		// session ended in the meantime.
		[[nodiscard]] static bool Suspended() noexcept;

		// The conversation the suspension was taken for, or 0.
		[[nodiscard]] static RE::FormID SuspendedFor() noexcept;

		// Give up on resuming: the conversation ended while the menu was up. Logged
		// once.
		//
		// a_handBackView restores the camera's resting aim, zoom and lens, which a
		// suspension deliberately leaves alone. False only for a load, where the saved
		// state belongs to the world being unloaded.
		static void AbandonSuspension(bool a_handBackView = true);

		// Called from the ThirdPersonState::Update hook after the game's own camera
		// work, so this write lands last.
		static void OnThirdPersonUpdate(RE::ThirdPersonState* a_state);

		[[nodiscard]] static bool Staging() noexcept;

		// Hotkey requests. Called from the input thread; each sets an atomic that the
		// next Tick picks up, so nothing touches the camera or Scaleform on the
		// caller's thread. Outside a conversation they're dropped.

		// Cut now: a new angle immediately, ignoring the minimum hold and the "is
		// there a reason to cut" test.
		static void RequestCut() noexcept;

		// Cycle who the camera is on: auto, them, you, the room.
		static void RequestFraming() noexcept;

		// Film the conversation in front of the player, or stop filming it. The one
		// key that works outside a conversation. Ignored during the player's own
		// conversation.
		static void RequestScene() noexcept;

		// The camera is filming other people rather than the player's conversation.
		[[nodiscard]] static bool FilmingScene() noexcept;

		[[nodiscard]] static Framing CurrentFraming() noexcept;

		// Is the mod switched on? Mirror of Tunables::enabled, read by Runtime.
		[[nodiscard]] static bool Directing() noexcept;

		// The dialogue menu is open and the topic list may be hidden. Set from the
		// menu-open event, which arrives before Open().
		[[nodiscard]] static bool DialogueMenuUp() noexcept;
	};
}

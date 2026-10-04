#include "SD/Camera/Director.h"
#include "SD/Camera/FaceFrame.h"

#include "SD/Camera/Presets.h"
#include "SD/Camera/PersuasionBeat.h"
#include "SD/Camera/PlayerVoiceHandoff.h"
#include "SD/Camera/ReactionShots.h"
#include "SD/Camera/ReplyBoundary.h"
#include "SD/Camera/Shot.h"
#include "SD/Camera/ShotAngles.h"
#include "SD/Camera/ShotSelection.h"
#include "SD/Camera/Space.h"
#include "SD/Camera/VisibilityRecovery.h"
#include "SD/Compat/DBReV.h"
#include "SD/Compat/ImprovedCamera.h"
#include "SD/Compat/SmoothCam.h"
#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"
#include "SD/Core/Text.h"
#include "SD/Dialogue/MenuWatch.h"
#include "SD/Dialogue/SceneScript.h"
#include "SD/Dialogue/SceneTrigger.h"
#include "SD/Dialogue/SceneWatch.h"
#include "SD/Dialogue/Session.h"
#include "SD/Render/Letterbox.h"
#include "SD/Runtime.h"
#include "SD/Scene/Focus.h"
#include "SD/Scene/Interface.h"
#include "SD/Scene/KeyLight.h"
#include "SD/Scene/LightRig.h"
#include "SD/Scene/FaceGen.h"
#include "SD/Scene/KeepPosed.h"
#include "SD/Scene/LipSync.h"
#include "SD/Scene/Performance.h"
#include "SD/Scene/Presence.h"
#include "SD/Scene/StopWork.h"
#include "SD/Scene/Subtitles.h"

#include <chrono>

namespace SD::Camera
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		bool              staging{ false };
		RE::ActorHandle   subject{};
		float             side{ 1.0f };

		// Set when staging started on a reopened dialogue menu before its speaker was
		// known. See Open().
		bool              provisionalStage{ false };

		// ---- Scene mode --------------------------------------------------------
		//
		// Filming two NPCs talking to each other. `subject` is the first speaker and
		// `counterpart` stands in for the player in every shot, so the shot table
		// works unchanged. The player-only systems (topic list, player voice, faces,
		// head tracking, stop-work) stay off. The counterpart is only the player when
		// the key films a single NPC talking to nobody in particular.
		bool              sceneMode{ false };
		RE::ActorHandle   counterpart{};
		bool              sceneAutomatic{ false };
		std::uint64_t     sceneKey{ 0 };

		// The game scene being filmed, or 0 when the two NPCs aren't in one.
		RE::FormID        sceneForm{ 0 };

		// Who has the floor. Sticky between lines, since a pause in a scene usually
		// isn't a change of speaker.
		bool              sceneSpeakerIsA{ true };
		Clock::time_point sceneLineAt{};
		float             sceneLineSeconds{ 0.0f };

		// When the player's own dialogue started during a scene. Runtime normally
		// takes over in the same frame; this is the fallback.
		Clock::time_point sceneTalkSince{};
		bool              sceneTalkSeen{ false };

		// The last filmed scene and when it ended, so auto mode doesn't re-film lines
		// it already covered.
		std::uint64_t     sceneEndedKey{ 0 };
		Clock::time_point sceneEndedAt{};

		std::atomic<bool>                requestScene{ false };
		Dialogue::SceneTrigger           sceneTrigger;
		Dialogue::SceneTrigger::Settings sceneTriggerSettings{};
		float                            sceneRange{ 600.0f };

		// Silence after the last line before a scene counts as over. See SceneOver().
		constexpr float   kSceneGapSeconds = 2.5f;
		constexpr float   kSceneLongGapSeconds = 9.0f;
		Clock::time_point sceneQuietSince{};

		// Once a scene has only repeating lines (or nothing) left, let go this soon
		// after the last real line. See SceneScript.h.
		constexpr float kHandOffSeconds = 1.0f;

		// Set when the key started filming a scene that was already only repeating
		// itself. Cleared once the scene moves on.
		bool sceneWaitExempt{ false };

		// Scene and phase last logged by SceneHandingOff(), so it logs once per phase.
		std::uint64_t sceneAheadReported{ 0 };

		// How close somebody must stand to the speaker to join their scene.
		constexpr float kJoinReach = 700.0f;

		// Whether Close() should put the player back in first person. Recorded at
		// Open(), because by Close() a third-person camera looks the same either way.
		bool              returnToFirstPerson{ false };

		enum class ViewMode { kCinematic, kFirstPersonFallback };
		ViewMode viewMode{ ViewMode::kCinematic };
		bool fallbackRequested{ false };
		bool fallbackThirdPersonOwed{ false };
		bool suspendedInFallback{ false };
		bool suspendedThirdPersonOwed{ false };
		bool restoreThirdPersonPending{ false };
		VisibilityRecovery visibilityRecovery{};
		std::atomic<bool> protectionSettingsDirty{ false };
		bool visibilityFailedObservation{ false };
		std::optional<Subjects> frameSubjects;
		Pose protectedPose{};
		ShotType protectedShot{ ShotType::kTwoShot };
		Clock::time_point visibilityCheckedAt{};
		Clock::time_point protectedRetryAt{};
		RE::NiPoint3 checkedNpc{}, checkedPlayer{}, checkedCamera{};
		float checkedLens{ 0.0f };
		std::uint32_t fallbackTurn{ 0 };
		std::uint32_t fallbackCue{ 0 };

		struct ProtectedSearch
		{
			std::array<ShotType, static_cast<std::size_t>(ShotType::kCount)> order{};
			std::size_t count{ 0 }, cursor{ 0 };
			bool active{ false }, recovery{ false };
			std::uint32_t turn{ 0 };
			Framing framingAtStart{ Framing::kAuto };
			Pose best{};
			ShotType bestType{ ShotType::kTwoShot };
		};
		ProtectedSearch protectedSearch{};

		// ---- Suspension --------------------------------------------------------
		//
		// A menu that owns the screen (inventory, barter, container) can open in the
		// middle of a conversation. The conversation is still live underneath, so the
		// camera is suspended and comes back when the menu closes instead of ending.
		bool       suspendedForMenu{ false };
		RE::FormID suspendedPartner{ 0 };

		// Session serial of the suspended conversation, so a new conversation with the
		// same actor isn't mistaken for a resume. 0 means nothing is suspended.
		std::uint32_t suspendedSerial{ 0 };

		// Carried across a suspension so a first-person player isn't dropped back into
		// first person while the inventory is open.
		bool suspendedFirstPersonOwed{ false };

		// Shot on screen when suspended. Restored on resume so it doesn't look like a
		// new conversation starting.
		ShotType suspendedShot{ ShotType::kTwoShot };

		// Session serial of the staged conversation (0 = none). The form ID alone
		// can't tell two conversations with the same actor apart.
		std::uint32_t stagedSerial{ 0 };

		ShotType          currentShot{ ShotType::kTwoShot };
		ShotType          previousShot{ ShotType::kTwoShot };
		Clock::time_point shotSince{};
		Clock::time_point stagingSince{};
		bool              haveShot{ false };

		bool              npcSpeaking{ false };
		bool              wasSpeaking{ false };
		bool              pendingSpeaking{ false };
		Clock::time_point pendingSince{};
		Clock::time_point turnBeganAt{};

		// NPC lines delivered so far this turn. The environmental shot waits for a
		// couple of them.
		std::uint32_t linesThisTurn{ 0 };

		// Long enough to bridge the gap between two responses, short enough that a
		// real handover still feels immediate.
		constexpr float kTurnDebounceSeconds = 0.40f;

		// Delay before the spent topic list starts fading once the NPC has the turn. 0
		// fades it immediately.
		float           choiceFadeDelay{ 2.4f };

		// Fade-out time for the spent list. Kept short so it reads as acknowledging
		// the pick rather than lagging behind it.
		float           choiceFadeOut{ 0.25f };

		// Fade-in time. Also the timebase for the return branch.
		constexpr float kChoiceFadeSeconds = 0.55f;

		// Don't hide the topic list during the opening greeting. Accept commits the
		// highlighted topic whether or not the list is visible, so the click that
		// started the conversation would pick topic one.

		// How long the player's turn has to hold before the topic list comes back.
		// Only needs to absorb a dropped frame of speech; choiceSpentThisTurn covers
		// the gap between responses.
		float listReturnDelay{ 0.20f };

		// There is deliberately no timeout that forces the list back. A timer can't
		// tell a stuck menu from a long line.

		// Last logged menu phase, so only transitions are logged.
		Scene::Interface::MenuPhase lastMoviePhase{ Scene::Interface::MenuPhase::kUnknown };

		// The player's voice handle. See VoicePlaying() for why it's also timed.
		std::uint32_t     voiceHandleID{ RE::BSSoundHandle::kInvalidID };
		std::uint32_t     retiredVoiceID{ RE::BSSoundHandle::kInvalidID };
		Clock::time_point voiceHandleSince{};
		std::uint16_t cueIntensity{ 50 };

		// Last look passed to SetLook(). Only re-sent on change, since sending it
		// again restarts the lamp's fade.
		int lastLookApplied{ -1 };

		// Per-angle looks and nudges, plus the global nudge.
		bool lightPerShot{ false };
		int  lightOffsetX{ 0 };
		int  lightOffsetY{ 0 };
		int  lightOffsetZ{ 0 };

		std::uint32_t cueCount{ 0 };
		bool          roomy{ true };
		RE::NiPoint3  openDirection{ 1.0f, 0.0f, 0.0f };
		float         openDistance{ 400.0f };
		float         screenAspect{ 1.78f };

		// Room type from the twelve clearance probes: a corridor (a couple of long
		// bearings), an ordinary room, or open ground. Each wants different wide
		// shots.
		enum class Space : std::uint8_t
		{
			kTight,  // a corridor, a stairwell, a small cell
			kRoom,   // an ordinary interior
			kOpen    // a hall, a square, outdoors
		};

		Space roomSpace{ Space::kRoom };

		[[nodiscard]] std::string_view SpaceName(Space a_space) noexcept
		{
			switch (a_space) {
			case Space::kTight: return "tight"sv;
			case Space::kOpen:  return "open"sv;
			default:            return "room"sv;
			}
		}

		// Clear height above the conversation in world units. 0 means not measured,
		// which means no clamp.
		float ceilingRoom{ 0.0f };

		// Whether `side` has been chosen for this conversation. See ChooseSide().
		bool sideCommitted{ false };

		// Requests from the input thread, consumed on the tick. Several presses before
		// a frame runs collapse into one.
		std::atomic<bool> requestCut{ false };
		std::atomic<bool> requestFraming{ false };

		// Set from requestCut on the tick and kept until the cut gate actually runs.
		bool forcedCut{ false };

		Framing framing{ Framing::kAuto };

		// A framing override lasts one turn, then auto takes over again. Counted in
		// turns rather than time, since that's how the player thinks about it.
		std::uint32_t turnSerial{ 0 };
		std::uint32_t framingUntilTurn{ 0 };

		// Mirror of Tunables::coverPlayerTurn, read by SubjectIsNpc() and Coverage().
		bool coverPlayerTurn{ true };
		PlayerVoiceHandoff playerVoiceHandoff;

		// See Tunables::playerVoiceHold. Applied on the tick because ApplyTunables
		// runs on the menu thread.
		float playerVoiceHoldSeconds{ 0.0f };

		ReactionShots           reactionShots;
		ReactionShots::Settings reactionSettings{};

		// Persuasion beat (see PersuasionBeat.h). beatInfo is the last reply
		// classified and is cleared on every pick. pickedCheck is what the chosen
		// prompt said, for checks that aren't expressed as a condition.
		PersuasionBeat          persuasion;
		bool                    persuasionBeatEnabled{ true };
		const RE::TESTopicInfo* beatInfo{ nullptr };
		SpeechCheck             pickedCheck{ SpeechCheck::kNone };
		ShotType                beatShot{ ShotType::kCount };
		Clock::time_point       beatShotSince{};
		float                   beatPushSeconds{ 4.0f };
		std::uint32_t           lastCueWords{ 0 };

		// How far the beat pushes in, as a fraction of a full push.
		constexpr float kBeatPush = 0.6f;

		// Mirror of Tunables::subtitlesInBar.
		bool subtitlesInBar{ false };

		// Topic-pick detection for replyBoundary: the last values the tick saw.
		ReplyBoundary               replyBoundary;
		Scene::Interface::MenuPhase lastPickPhase{ Scene::Interface::MenuPhase::kUnknown };
		std::uint64_t               lastPlayerLineSerial{ 0 };

		[[nodiscard]] bool ReactionActive()
		{
			return reactionSettings.enabled && reactionShots.Active();
		}

		// Who the camera should be on right now. Everything that needs this asks here,
		// so the framing override and the current speaker can't disagree. kRoom isn't
		// a person and is handled at each call site.
		[[nodiscard]] bool SubjectIsNpc()
		{
			switch (framing) {
			case Framing::kThem: return true;
			case Framing::kYou:  return false;

			// With Cut To You On Your Turn off, the subject doesn't change when the NPC
			// stops talking; the turn is covered by holding or going wide.
			//
			// A reaction overrides normal coverage, but not manual framing.
			//
			// In scene mode "you" is the second NPC, so their lines are normal coverage
			// and a reaction is the other NPC listening.
			default:
				if (sceneMode) {
					return ReactionActive() ? !sceneSpeakerIsA : sceneSpeakerIsA;
				}
				if (ReactionActive()) {
					return false;
				}
				return npcSpeaking || playerVoiceHandoff.Active() || !coverPlayerTurn;
			}
		}

		[[nodiscard]] bool FramingIsRoom()
		{
			return framing == Framing::kRoom;
		}

		[[nodiscard]] bool Drawable(ShotType a_type);
		[[nodiscard]] bool NeedsRoom(ShotType a_type);

		// Whether any enabled shot could serve this framing. Without this check an
		// empty framing makes the cut search run, and fail, every frame.
		[[nodiscard]] bool FramingViable(Framing a_framing)
		{
			if (a_framing == Framing::kAuto) {
				return true;
			}

			for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
				const auto type = static_cast<ShotType>(i);

				// Same test the pickers use, room gate included: a master shot that needs room
				// isn't available in a corridor.
				if (!Drawable(type) || (NeedsRoom(type) && !roomy)) {
					continue;
				}

				const bool neutral = IsNeutral(type);
				switch (a_framing) {
				case Framing::kRoom:
					if (neutral) {
						return true;
					}
					break;
				case Framing::kThem:
					if (!neutral && FavoursNpc(type)) {
						return true;
					}
					break;
				case Framing::kYou:
					if (!neutral && !FavoursNpc(type)) {
						return true;
					}
					break;
				default:
					return true;
				}
			}

			return false;
		}

		// Next framing with something enabled behind it, so a key press always changes
		// the picture. From auto it jumps to whichever side isn't on screen; after
		// that it cycles in order.
		[[nodiscard]] Framing NextFraming(Framing a_from)
		{
			auto candidate = a_from;
			for (int step = 0; step < 4; ++step) {
				candidate = candidate == Framing::kAuto ?
					(npcSpeaking ? Framing::kYou : Framing::kThem) :
					static_cast<Framing>((static_cast<std::uint8_t>(candidate) + 1u) % 4u);

				if (FramingViable(candidate)) {
					return candidate;
				}
			}
			return Framing::kAuto;
		}

		// The last angle that actually placed, per subject, stored as an offset so
		// it still tracks a subject who moves.
		struct RememberedPose
		{
			Pose         pose{};
			RE::NiPoint3 anchor{};
			ShotType     type{ ShotType::kCount };
		};

		RememberedPose lastNpcPose{};
		RememberedPose lastPlayerPose{};
		Clock::time_point enabledRetryAt{};
		bool nativeView{ false };

		// What the current shot settled on, held until the next cut. Reset by watching
		// shotSince rather than at each place that starts a shot.
		float             heldSweep{ kUnheld };
		float             heldStandoff{ kUnheld };

		// The room measured on the frame the shot cut, kept for the life of the shot.
		// Unlike heldSweep and heldStandoff this is never re-committed, because a
		// holding frame just reports the held value back out of Solve.
		float heldRoom{ kUnheld };

		Clock::time_point heldSince{};

		constexpr const char* kIniPath = ".\\Data\\SKSE\\Plugins\\SD.ini";

		// A cut needs a reason: a new line starting, or the turn passing between the
		// player's menu and the NPC's reply.
		bool              cueSinceCut{ false };
		bool              turnSinceCut{ false };
		Clock::time_point replyStartedAt{};

		// How long the camera stays on the player once the NPC starts replying.
		// Without a player-voice mod this only has to keep the cut off the same frame.
		// With one, the player's line already plays first.
		float playerBeatSeconds{ 0.45f };

		// DBVO ships as an ESP with Papyrus and voice packs, not a DLL, so the plugin
		// is what actually identifies it. The DLL check stays in case a build ever
		// ships one.
		[[nodiscard]] bool DetectPlayerVoice()
		{
			constexpr std::array kModules{
				L"DBVO.dll",
				L"DragonbornVoiceOver.dll",
				L"DBVO_SE.dll",
			};
			for (const auto* name : kModules) {
				if (::GetModuleHandleW(name)) {
					Log::Info(Log::Category::kCamera, "Player voice detected by DLL."sv);
					return true;
				}
			}

			// LookupModByName covers full plugins; light plugins live in a separate table.
			if (auto* handler = RE::TESDataHandler::GetSingleton()) {
				constexpr std::array kPlugins{ "DBVO.esp"sv, "DBVO.esl"sv };
				for (const auto& name : kPlugins) {
					if (handler->LookupModByName(name) || handler->LookupLoadedLightModByName(name)) {
						Log::Info(Log::Category::kCamera, "Player voice detected by plugin {}."sv, name);
						return true;
					}
				}
			}

			Log::Info(Log::Category::kCamera, "No player-voice mod detected; using the unvoiced beat."sv);
			return false;
		}

		// Sends DBVO's PlayDBVOTopic event when a voiced player line starts, so
		// gesture add-ons that listen for it animate during the line. ReVoiced and
		// DBVO 2 never send it. Only for lines with a known start (a sound handle
		// could be anything), and never under DBVO 1, which sends the event itself and
		// plays the voice from it.
		//
		// Gesture mods keyed on OAR's "player has chosen" condition can't be helped
		// from here: that turns true when the NPC starts answering, which ReVoiced
		// delays until the player's line has finished.
		bool                playerGestureCue{ true };
		std::uint64_t       gestureCueSerial{ 0 };
		std::optional<bool> dbvoOneLoaded;

		void CuePlayerGesture()
		{
			const auto serial = Scene::LipSync::PlayerLineSerial();
			if (serial == gestureCueSerial) {
				return;
			}
			gestureCueSerial = serial;
			if (!playerGestureCue || !Scene::LipSync::PlayerLineAnnounced()) {
				return;
			}

			if (!dbvoOneLoaded) {
				auto* handler = RE::TESDataHandler::GetSingleton();
				dbvoOneLoaded = handler &&
					(handler->LookupModByName("DBVO.esp"sv) || handler->LookupLoadedLightModByName("DBVO.esl"sv));
				if (*dbvoOneLoaded) {
					Log::Info(Log::Category::kStaging,
						"DBVO 1 is loaded and sends PlayDBVOTopic itself; SD will not send a second one."sv);
				}
			}
			if (*dbvoOneLoaded) {
				return;
			}

			auto* source = SKSE::GetModCallbackEventSource();
			if (!source) {
				return;
			}
			const std::string text{ Scene::LipSync::PlayerLineText() };
			SKSE::ModCallbackEvent event{ RE::BSFixedString{ "PlayDBVOTopic" }, RE::BSFixedString{ text.c_str() }, 0.0f, nullptr };
			source->SendEvent(&event);
			Log::Info(Log::Category::kStaging, "Player gesture cue (PlayDBVOTopic) sent as your line starts: \"{}\"."sv, text);
		}

		Clock::time_point lastFrameAt{};
		bool              haveFrameTime{ false };

		// The player's own field of view and the one this conversation composes
		// against. Sampled on idle frames (see SampleCameraRest) rather than read at
		// Open(), so a camera left narrowed by a crash or a skipped Close() can't
		// become the new baseline. Readings below the floor are rejected for the same
		// reason.
		constexpr float kSaneMinFov = 45.0f;
		float           restingFov{ 0.0f };
		float           baseFov{ 75.0f };

		// The third-person camera as the player had it before the conversation.
		//
		// While staged, SD writes the camera node every frame, and underneath it the
		// engine aims its own dialogue camera at the NPC (often pitched down at
		// someone seated). When SD lets go, the engine's starting pose would appear as
		// a sudden cut, so these values are put back at Close().
		//
		// Translation and rotation are deliberately not saved: after a walk-and-talk
		// they would put the camera back where the conversation started. The engine
		// recomputes both from the fields below.
		struct CameraRest
		{
			RE::NiPoint2 freeRotation{};        // yaw, pitch (the one that reads as "looking down")
			RE::NiPoint3 posOffsetExpected{};
			RE::NiPoint3 posOffsetActual{};
			float        targetZoomOffset{ 0.0f };
			float        currentZoomOffset{ 0.0f };
			float        savedZoomOffset{ 0.0f };
			float        pitchZoomOffset{ 0.0f };
			float        targetYaw{ 0.0f };
			float        currentYaw{ 0.0f };
			bool         freeRotationEnabled{ false };
		};

		CameraRest cameraRest{};
		bool       cameraRestPrimed{ false };

		// Whether the current Close() is a suspension for a menu rather than an
		// ending. A suspension comes straight back, so the third-person state
		// (including SmoothCam's zoom) isn't handed back across it. Written only by
		// OnScreenTaken().
		bool suspendingForMenu{ false };

		// When a screen-owning menu last closed, for the settle window in
		// SampleCameraRest. Set from the menu event rather than the tick, because the
		// tick doesn't run while a pausing menu is open.
		Clock::time_point screenReleasedAt{};

		// How long after such a menu closes before the camera's resting state is worth
		// sampling again. SmoothCam takes a few frames to settle.
		constexpr float kRestSettleSeconds = 0.5f;

		// Set when the dialogue menu closes. The camera is handed back shortly after
		// unless the menu comes straight back.
		bool              releasePending{ false };
		Clock::time_point releaseSince{};

		// ---- Direction settings ------------------------------------------------
		//
		// Re-read in Open(), so ini edits apply from the next conversation.

		// Minimum time on a shot before a cut. Long enough that a cut has to be
		// earned.
		float minShotSeconds{ 2.4f };

		// Floor that keeps a coverage cut off the same frame as the previous one. Not
		// a pacing control.
		float minTurnSeconds{ 0.25f };


		// Backstop so a shot can't sit forever through a long silence. Cuts are meant
		// to land on a new line or a change of turn.
		float maxShotSeconds{ 9.0f };

		// Per Line Angle Change: hold a setup for this many eligible lines, then take
		// a new angle. The count is re-rolled between min and max after every cut; min
		// > max is swapped.
		std::uint32_t cutEveryMin{ 3 };
		std::uint32_t cutEveryMax{ 6 };

		// Whether the line count may ask for a new angle. Per-line and timed cuts are
		// independent, and both can be off (coverage still cuts).
		bool perLineAngleChange{ true };

		// Whether a short line ("Yes.", "Hmm.") counts toward the cadence. Word count
		// is used because it's known when the line starts; the line's duration isn't.
		bool          holdOnShortLines{ true };
		std::uint32_t shortLineWords{ 4 };

		// Timed Angle Change: whether the staleness ceiling may fire, per side of the
		// exchange. Both off by default, so every cut has a reason.
		bool timedCutsWhileSpeaking{ false };
		bool timedCutsWhileChoosing{ false };

		// Line rule and crowd test, mirrored from Tunables because Solve reads them
		// for every candidate.
		bool enforceLine{ true };
		bool true180{ false };
		bool avoidCrowds{ true };

		// Set when true180 changes mid-conversation; handled on the tick.
		std::atomic<bool> lineRuleChanged{ false };

		// Whether a shot stops re-probing once it has cut. Unlike the two above, this
		// only reaches the shot being rendered, and only once it has a held pose.
		bool holdPlacement{ false };
		std::atomic<bool> protectSubject{ false };
		std::atomic<bool> firstPersonFallback{ true };

		// Whether the topic list fades out under the NPC's line. See Tunables.
		bool fadeTopicList{ true };

		// Mirror of Tunables::letterbox.
		bool letterboxWanted{ true };

		// The topic list is never hidden until the menu has shown it at least once. A
		// hidden list still accepts input, and Accept commits the highlighted topic,
		// so hiding it during an opening greeting would let the click that started the
		// conversation pick topic one. Read from the menu state (eMenuState reaching
		// topicList), not inferred from turn changes.
		bool listWasLive{ false };

		// Speaker-name hide as last requested, against Interface's own copy as last
		// applied. ApplyTunables runs on the settings panel's thread and the Scaleform
		// work belongs to the tick, so the panel sets a flag and SyncInterfaceSettings
		// applies it on the next frame.
		bool wantHideSpeakerName{ true };
		bool interfaceDirty{ true };

		// Topic list animation state. listWanted is the target the menu phase last
		// asked for, listEdgeAt is when it changed, and choiceEaseFrom is the alpha it
		// was easing from.
		bool              listWanted{ true };
		Clock::time_point listEdgeAt{};
		float             choiceAlpha{ 100.0f };
		float             choiceEaseFrom{ 100.0f };

		// Stopping a line's audio doesn't shorten the line: the engine still ends it
		// at its full length, and nothing on the actor holds it open. A key press
		// can't end a line early from here.

		// bFadeAfterPlayerLine. A new key rather than a rename, because the old flag
		// stored the opposite of what its label said.
		bool fadeAfterPlayerLine{ true };

		// When the hold for the player's line began. The hold pins the fade clock
		// while the player's voice plays, so it's capped: a sound handle stuck in
		// kPlaying would otherwise leave the spent list up for the rest of the
		// conversation. Measured from the start of the hold, so a missing end signal
		// costs one delay.
		Clock::time_point playerLineHeldSince{};
		bool              playerLineHeld{ false };

		// When the player's line length is known, the spent list's fade is timed to
		// finish with the line rather than starting after it. voiceFadeFrom is the
		// alpha the fade started from (so it never brightens), voiceFadeSerial ties it
		// to its line, and voiceServedDelay waives iChoiceFadeDelay once a line has
		// held the list.
		bool          voiceFadeTimed{ false };
		float         voiceFadeFrom{ 100.0f };
		std::uint64_t voiceFadeSerial{ 0 };
		bool          voiceServedDelay{ false };

		// Well past any authored player line (the longest vanilla ones run about nine
		// seconds).
		constexpr float kMaxPlayerLineHold = 15.0f;

		// Logged once per conversation; reset in Open().
		Log::OnceFlag playerLineHoldExpired;

		// Whether the dialogue menu is up and the topic list may be hidden. Read by
		// the click guard on the input thread. Set from the menu itself rather than
		// Staging(), because the greeting starts about 120 ms before Open() runs.
		std::atomic_bool dialogueMenuUp{ false };

		// Mirror of Tunables::enabled, read by Runtime. Filled by LoadSettings before
		// anything can stage.
		bool directing{ false };


		// Diagnostic: hold a frontal close-up on the player when a conversation opens,
		// in milliseconds. 0 is off. See HoldingOpenShot().
		int               openShotHold{ 0 };
		Clock::time_point openShotUntil{};

		// Opening shots when the above is on. Hand-picked rather than drawn from the
		// coverage pool, because the room hasn't been probed yet when Open() runs:
		// every setup here is tight or medium and needs no space. Each listed once so
		// the draw respects the weights.
		constexpr std::array kOpeners{
			ShotType::kOverPlayerShoulder,
			ShotType::kMediumNpc,
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kCloseUp,
		};

		// The same on the player's side, for a conversation that opens in silence.
		// Mirrors kOpeners setup for setup.
		constexpr std::array kPlayerOpeners{
			ShotType::kOverNpcShoulder,
			ShotType::kMediumPlayer,
			ShotType::kDirtyPlayer,
			ShotType::kThreeQuarterPlayer,
			ShotType::kClosePlayer,
		};

		// Words in the subtitle, for the short-line rule. Splits on whitespace runs
		// without std::isspace (undefined for chars above 0x7F). Punctuation isn't
		// stripped, so "Yes." is one word.
		//
		// Scripts that don't use spaces (kana, CJK ideographs) are counted by length
		// instead; without that every Japanese or Chinese line would count as one
		// word. Hangul and Cyrillic use spaces and go through the normal path.
		[[nodiscard]] bool UnspacedScript(char32_t a_cp) noexcept
		{
			return (a_cp >= 0x3040 && a_cp <= 0x30FF && a_cp != 0x30FB) ||  // kana, minus the middle dot
				(a_cp >= 0x3400 && a_cp <= 0x4DBF) ||                       // CJK extension A
				(a_cp >= 0x4E00 && a_cp <= 0x9FFF) ||                       // CJK unified
				(a_cp >= 0xF900 && a_cp <= 0xFAFF) ||                       // CJK compatibility
				(a_cp >= 0xFF66 && a_cp <= 0xFF9D);                         // halfwidth katakana
		}

		// Full-width punctuation ends an unspaced run without counting as part of it,
		// so はい。 is one word. ASCII punctuation isn't listed on purpose (see above).
		[[nodiscard]] bool UnspacedBreak(char32_t a_cp) noexcept
		{
			return (a_cp >= 0x3000 && a_cp <= 0x303F) ||   // 、。「」《》 and the ideographic space
				a_cp == 0x30FB ||                          // ・
				(a_cp >= 0xFF01 && a_cp <= 0xFF0F) ||      // ！ through ／
				(a_cp >= 0xFF1A && a_cp <= 0xFF20) ||      // ： through ＠, which is where ？ lives
				a_cp == 0x2026;                            // …
		}

		// Characters per word in unspaced scripts. A Chinese word averages about 1.5
		// characters and a Japanese bunsetsu about 2.5; this only has to sort lines
		// around a floor of four.
		constexpr std::uint32_t kUnspacedCharsPerWord = 2;

		// Unspaced runs are measured by length, everything else by spaces, so mixed
		// lines work.
		[[nodiscard]] std::uint32_t WordCount(const char* a_text)
		{
			if (!a_text) {
				return 0;
			}

			const std::string_view text{ a_text };

			std::uint32_t words = 0;
			bool          inWord = false;
			std::uint32_t unspacedRun = 0;

			// Rounds up, so a single kanji still counts as a word.
			const auto flush = [&]() {
				if (unspacedRun > 0) {
					words += (unspacedRun + kUnspacedCharsPerWord - 1) / kUnspacedCharsPerWord;
					unspacedRun = 0;
				}
			};

			for (std::size_t pos = 0; pos < text.size();) {
				const char32_t cp = Text::NextCodepoint(text, pos);

				if (UnspacedScript(cp)) {
					// Close any open Latin word first, so the "Fus" in Fusロ・ダー isn't merged into
					// the kana after it.
					inWord = false;
					++unspacedRun;
					continue;
				}

				flush();

				const bool space = cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
					UnspacedBreak(cp);
				if (space) {
					inWord = false;
				} else if (!inWord) {
					inWord = true;
					++words;
				}
			}

			flush();
			return words;
		}

		// Lines delivered on the current setup, and how many it holds for. Re-rolled
		// at every cut.
		std::uint32_t linesSinceCut{ 0 };
		std::uint32_t cutEveryTarget{ 1 };

		[[nodiscard]] std::uint32_t NextRandom();

		[[nodiscard]] std::uint32_t RollCutEvery()
		{
			const std::uint32_t lo = std::min(cutEveryMin, cutEveryMax);
			const std::uint32_t hi = std::max(cutEveryMin, cutEveryMax);
			return hi <= lo ? lo : lo + (NextRandom() % (hi - lo + 1));
		}

		// The live copy, so the menu shows what's actually in force.
		Tunables tunables{};

		// Defined below with the pools. Called from ReadTuning, which is where every
		// shot gets its weight.
		void AuditPools();

		// Read the direction settings. Once per conversation.
		void ReadTuning()
		{
			const Config::ReadScope settings;
			Tunables read{};
			read.minShotTime = Config::Int("Direction", "iMinShotTime", 240);
			read.minTurnTime = Config::Int("Direction", "iMinTurnTime", 25);
			read.maxShotTime = Config::Int("Direction", "iMaxShotTime", 900);
			// Every fallback below is the Close preset's value. The Tunables initializers,
			// this list, config/SD.ini and Camera::kCloseStyle all state the same numbers;
			// a fresh install has to come up reading Close or the Presets page shows
			// nothing selected.
			read.letterbox = Config::Bool("Direction", "bLetterbox", true);
			read.letterboxHeight = Config::Int("Direction", "iLetterboxHeight", 120);
			read.cutEveryMin = Config::Int("Direction", "iCutEveryMin", 3);
			read.cutEveryMax = Config::Int("Direction", "iCutEveryMax", 6);
			read.perLineAngleChange = Config::Bool("Direction", "bPerLineAngleChange", true);
			read.holdOnShortLines = Config::Bool("Direction", "bHoldOnShortLines", true);
			read.shortLineWords = Config::Int("Direction", "iShortLineWords", 4);
			read.timedCutsWhileSpeaking = Config::Bool("Direction", "bTimedCutsWhileSpeaking", false);
			read.timedCutsWhileChoosing = Config::Bool("Direction", "bTimedCutsWhileChoosing", false);
			// bCutOnLineEnd was removed and is ignored if an old ini still has it.
			read.enabled = Config::Bool("Direction", "bEnabled", true);
			read.coverPlayerTurn = Config::Bool("Direction", "bCoverPlayerTurn", true);
			read.enforceLine = Config::Bool("Direction", "bEnforceLine", true);
			read.true180 = Config::Bool("Direction", "bTrue180", false);
			read.followFace = Config::Bool("Direction", "bFollowFace", true);
			read.playerGestureCue = Config::Bool("Performance", "bPlayerGestureCue", true);
			read.stopWorkToTalk = Config::Bool("Performance", "bStopWorkToTalk", true);
			read.avoidCrowds = Config::Bool("Direction", "bAvoidCrowds", true);
			read.holdPlacement = Config::Bool("Direction", "bHoldPlacement", false);
			read.protectSubject = Config::Bool("Direction", "bKeepSubjectVisible", false);
			read.firstPersonFallback = Config::Bool("Direction", "bFirstPersonFallback", true);
			read.fadeTopicList = Config::Bool("Direction", "bFadeTopicList", true);

			// The interface flags are read here rather than once at startup, so ini edits
			// apply from the next conversation like every other setting.
			read.hideSpeakerName = Config::Bool("Direction", "bHideSpeakerName", true);
			read.fadeAfterPlayerLine = Config::Bool("Direction", "bFadeAfterPlayerLine", true);

			read.poseMode = Config::Int("Diagnostics", "iPoseMode", 0);
			openShotHold = std::clamp(Config::Int("Diagnostics", "iOpenShotHold", 0), 0, 10000);
			listReturnDelay =
				std::clamp(Config::Int("Direction", "iListReturnDelay", 20), 0, 200) / 100.0f;
			// Hundredths of a second, like the setting above.
			read.choiceFadeDelay = std::clamp(Config::Int("Direction", "iChoiceFadeDelay", 150), 0, 600);
			choiceFadeDelay = static_cast<float>(read.choiceFadeDelay) / 100.0f;
			read.choiceFadeTime = std::clamp(Config::Int("Direction", "iChoiceFadeTime", 200), 5, 200);
			choiceFadeOut = static_cast<float>(read.choiceFadeTime) / 100.0f;

			// The only setting whose default depends on the install. DetectPlayerVoice
			// runs once.
			static const bool playerVoiced = DetectPlayerVoice();
			read.playerBeat = Config::Int("Direction", "iPlayerBeat", playerVoiced ? 90 : 45);
			read.playerVoiceHold = std::clamp(Config::Int("Direction", "iPlayerVoiceHold", 0), 0, 300);

			read.reactionShots = Config::Bool("Direction", "bReactionShots", false);
			read.reactionEvery = std::clamp(Config::Int("Direction", "iReactionEvery", 3), 1, 10);
			read.reactionChance = std::clamp(Config::Int("Direction", "iReactionChance", 50), 0, 100);

			read.persuasionBeat = Config::Bool("Direction", "bPersuasionBeat", true);
			read.subtitlesInBar = Config::Bool("Direction", "bSubtitlesInBar", false);
			read.sceneAuto = Config::Bool("Direction", "bFilmScenesAuto", false);
			read.sceneRange = std::clamp(Config::Int("Direction", "iSceneRange", 600), 150, 2000);
			read.sceneWait = std::clamp(Config::Int("Direction", "iSceneWait", 150), 0, 600);

			Director::ApplyTunables(read);

			// Read here rather than in Open(), which doesn't run with bEnabled=0, and the
			// probe still has to report in that case.
			Scene::Performance::ConfigureProbe(Config::Bool("Diagnostics", "bLogFaceAnim", false));

			// Re-read with the rest, so the forced viseme can be toggled against the same
			// face in one session. The hook itself is installed once at startup.
			Scene::FaceGen::SetForcedViseme(Config::Int("Diagnostics", "iForceViseme", -1));

			// The toggle and strength apply per conversation; the hook installs at
			// startup, so turning it on mid-session needs a restart.
			Scene::LipSync::Configure(Config::Bool("Performance", "bSynthLipSync", true),
				Config::Int("Performance", "iLipSyncStrength", 55));

			// Expressions are independent of the optional mouth/brow layer.
			Scene::Performance::Configure(Config::Bool("Performance", "bExpressions", true), false);

			// Re-read with the rest so it can be toggled against the same face.
			Scene::Performance::SetForcedExpression(
				Config::Int("Diagnostics", "iForceExpression", -1));

			// The shot list is read straight into Shot rather than through Tunables;
			// there's nothing to convert.
			//
			// Fallbacks come from the shipped preset for the setups it uses, and from the
			// shot table for the rest. That way a fresh install doesn't report drift
			// against its own preset, and a setup the preset leaves off comes up with the
			// table's values when it's switched on.
			const auto& shipped = DefaultPreset();

			std::uint32_t off = 0;
			for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
				const auto type = static_cast<ShotType>(i);
				const bool shippedOn = PresetUses(shipped, type);

				const bool on = Config::Bool("Shots", Key(type), shippedOn);
				Shot::SetEnabled(type, on);
				Shot::SetWeight(type,
					Config::Int("Shots", WeightKey(type),
						shippedOn ? PresetWeight(shipped, type) : Camera::AuthoredWeight(type)));
				Shot::SetLens(type, Config::Int("Shots", LensKey(type),
									  shippedOn ? PresetLens(shipped, type) :
												  static_cast<int>(Camera::AuthoredLens(type))));

				// Before 1.3 the only per-setup move setting was iNameZoom (0 authored, 1 zoom
				// in, 2 zoom out). A saved 1 or 2 becomes the matching move.
				const auto shippedMotion = PresetMotion(shipped, type);

				const int legacyZoom = Config::Int("Shots", ZoomKey(type), 0);
				const auto seeded = legacyZoom == 1 ? Move::kZoomIn :
					legacyZoom == 2                 ? Move::kZoomOut :
					shippedOn                       ? shippedMotion.move :
					                                  Shot::AuthoredMove(type);

				const int stored = Config::Int("Shots", MoveKey(type),
					static_cast<int>(seeded));
				Shot::SetMove(type, stored >= 0 && stored < static_cast<int>(Move::kCount) ?
									   static_cast<Move>(stored) :
									   Shot::AuthoredMove(type));

				Shot::SetMoveAmount(type, Config::Int("Shots", MoveAmountKey(type),
											  shippedOn ? shippedMotion.amount :
														  Shot::AuthoredMoveAmount(type)));
				Shot::SetMoveTime(type, Config::Int("Shots", MoveTimeKey(type),
											shippedOn ? shippedMotion.time :
														Shot::AuthoredMoveTime(type)));

				// Lighting look and this angle's nudge, read even when per-angle lighting is
				// off so turning it on applies from the next conversation. An unknown name
				// falls back to what the setup ships with, not the global default, and is
				// logged.
				const char* shippedLight =
					shippedOn ? PresetLight(shipped, type) : AuthoredLight(type);
				const auto wanted = Config::String("Shots", LightKey(type), shippedLight);
				int look = Scene::FindLook(wanted);
				if (look < 0) {
					look = Scene::FindLook(AuthoredLight(type));
					Log::Warn(Log::Category::kStaging,
						"{} names lighting look '{}', which does not exist; using '{}'."sv,
						Name(type), wanted, AuthoredLight(type));
				}
				Shot::SetLight(type, look >= 0 ? look : Scene::DefaultLook());

				Shot::SetLightOffset(type,
					Config::Int("Shots", LightXKey(type), 0),
					Config::Int("Shots", LightYKey(type), 0),
					Config::Int("Shots", LightZKey(type), 0));

				off += on ? 0u : 1u;
			}

			if (off > 0) {
				Log::Info(Log::Category::kCamera, "{} of {} shots disabled by settings."sv,
					off, static_cast<std::uint32_t>(ShotType::kCount));
			}

			AuditPools();
		}

		// How long a closed dialogue menu is tolerated before the camera goes back.
		// The menu can close and reopen within one conversation, so releasing on the
		// first close would drop the camera mid-exchange.
		constexpr float kReleaseGraceSeconds = 0.35f;

		// The speaker handle is briefly unset while a conversation is set up, so the
		// exit check waits for staging to settle.
		constexpr float kExitCheckDelay = 0.6f;

		constexpr std::uint16_t kCloseUpIntensity = 100;

		// Small deterministic RNG. Mixed with the cue count so the same conversation
		// doesn't repeat an identical shot list.
		std::uint32_t rngState{ 0x9E3779B9u };

		[[nodiscard]] std::uint32_t NextRandom()
		{
			rngState = rngState * 1664525u + 1013904223u;
			return rngState >> 16;
		}

		[[nodiscard]] float SecondsSince(Clock::time_point a_when)
		{
			return std::chrono::duration<float>(Clock::now() - a_when).count();
		}

		// Is a dialogue menu open right now? Separates "the player walked away" from
		// "the topic manager has no speaker at this instant".
		[[nodiscard]] bool DialogueMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}

		// A head position that doesn't twitch. The head bone carries breathing, idle
		// sway and gestures, and a camera anchored to it inherits all of that. The
		// actor's root doesn't animate, so the anchor is the root plus a head offset
		// measured once by Anatomy (which also handles non-humanoid rigs).
		[[nodiscard]] std::optional<RE::NiPoint3> StablePoint(RE::Actor* a_actor, const Anatomy& a_body)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return std::nullopt;
			}

			// Root plus the measured head offset, rotated by the actor's current heading.
			// Needed for creatures whose head is far in front of the root (dragons). Only
			// the heading is live, so the head bone's own animation still isn't inherited.
			const RE::NiPoint3 base = root->world.translate;
			const float        heading = a_actor->GetAngleZ();
			const float        c = std::cos(heading);
			const float        s = std::sin(heading);

			return RE::NiPoint3{
				base.x + a_body.headOffset.x * c - a_body.headOffset.y * s,
				base.y + a_body.headOffset.x * s + a_body.headOffset.y * c,
				base.z + a_body.eyeHeight
			};
		}

		struct Anchor
		{
			RE::NiPoint3 position{};
			bool         primed{ false };
		};

		Anchor playerAnchor{};
		Anchor npcAnchor{};

		// Measured at conversation open and again on a posture change. Not per frame:
		// it walks the skeleton, and the result describes the body, not the moment.
		Anatomy playerBody{};
		Anatomy npcBody{};

		// Posture change (sitting, sleeping, standing up). The head height is measured
		// once, which is wrong after someone lying down gets up, so a posture change
		// re-measures and re-solves the blocking.
		using Posture = RE::SIT_SLEEP_STATE;
		Posture playerPosture{ Posture::kNormal };
		Posture npcPosture{ Posture::kNormal };
		bool    posturePrimed{ false };

		[[nodiscard]] Posture PostureOf(RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			return state ? state->GetSitSleepState() : Posture::kNormal;
		}

		[[nodiscard]] std::string_view PostureName(Posture a_posture)
		{
			switch (a_posture) {
			case Posture::kNormal:              return "standing"sv;
			case Posture::kWantToSit:           return "about to sit"sv;
			case Posture::kWaitingForSitAnim:   return "sitting down"sv;
			case Posture::kIsSitting:           return "seated"sv;
			case Posture::kWantToStand:         return "standing up"sv;
			case Posture::kWantToSleep:         return "about to sleep"sv;
			case Posture::kWaitingForSleepAnim: return "lying down"sv;
			case Posture::kIsSleeping:          return "asleep"sv;
			case Posture::kWantToWake:          return "waking"sv;
			default:                            return "unknown"sv;
			}
		}

		// How quickly the anchor follows the subject. Low on purpose: the frame
		// shouldn't react to weight shifts, only to actual movement.
		constexpr float kFollowRate = 2.2f;

		void Follow(Anchor& a_anchor, const RE::NiPoint3& a_target, float a_delta)
		{
			if (!a_anchor.primed) {
				a_anchor.position = a_target;
				a_anchor.primed = true;
				return;
			}

			const float k = 1.0f - std::exp(-std::max(a_delta, 0.0f) * kFollowRate);
			a_anchor.position.x += (a_target.x - a_anchor.position.x) * k;
			a_anchor.position.y += (a_target.y - a_anchor.position.y) * k;
			a_anchor.position.z += (a_target.z - a_anchor.position.z) * k;
		}

		// Where the head actually is and which way the face points, for close-ups. The
		// stable anchors above stay as they are; this lets a close-up frame the face
		// of someone leaning over an anvil. See FaceFrame.h for the filtering.
		// Humanoid rigs only (vanilla head and neck bone names); anything else is
		// framed off the stable point.
		struct FaceTrack
		{
			// Held by reference so a detached node can't dangle. root is only compared, to
			// notice a 3D reload, and never dereferenced.
			RE::NiPointer<RE::NiAVObject> head{};
			const RE::NiAVObject*         root{ nullptr };
			int                           axis{ -1 };
			float                         sign{ 1.0f };
			FaceFrame::Vec                offset{};
			FaceFrame::Vec                facing{};
			bool                          primed{ false };
			bool                          valid{ false };
		};

		FaceTrack playerFaceTrack{};
		FaceTrack npcFaceTrack{};

		// [Direction] bFollowFace.
		bool followFace{ true };

		// Dead band before the frame follows the head, how fast it follows past that,
		// and how fast the facing turns. The dead band keeps breathing and idle sway
		// out of the shot.
		constexpr float kFaceDeadband = 6.0f;
		constexpr float kFaceFollowRate = 1.6f;
		constexpr float kFaceTurnRate = 2.0f;

		[[nodiscard]] static FaceFrame::Vec ToVec(const RE::NiPoint3& a_point)
		{
			return { a_point.x, a_point.y, a_point.z };
		}

		[[nodiscard]] static FaceFrame::Vec Column(const RE::NiMatrix3& a_rotate, int a_index)
		{
			return { a_rotate.entry[0][a_index], a_rotate.entry[1][a_index], a_rotate.entry[2][a_index] };
		}

		// Which head axis is the face. Once per 3D load: it's a property of the
		// skeleton, not the pose.
		void ResolveFace(FaceTrack& a_track, RE::Actor* a_actor, const RE::NiAVObject* a_root,
			const Anatomy& a_body, const RE::NiPoint3& a_toward)
		{
			a_track = {};
			a_track.root = a_root;

			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			const char* who = a_actor ? a_actor->GetName() : nullptr;
			who = (who && *who) ? who : "<unnamed>";
			if (!root || a_body.build != Build::kHumanoid) {
				return;
			}

			auto* head = root->GetObjectByName("NPC Head [Head]"sv);
			auto* neck = root->GetObjectByName("NPC Neck [Neck]"sv);
			if (!head || !neck) {
				Log::Info(Log::Category::kStaging,
					"Face framing: {} has no NPC Head/Neck bones; close-ups use the stable head point."sv, who);
				return;
			}

			const auto& world = head->world;
			const std::array<FaceFrame::Vec, 3> columns{
				Column(world.rotate, 0), Column(world.rotate, 1), Column(world.rotate, 2)
			};
			const FaceFrame::Vec up = ToVec(world.translate) - ToVec(neck->world.translate);

			// The magic node sits in front of the mouth on humanoid skeletons, so it's the
			// best hint. Without it, use the body's heading blended with the direction to
			// the other person.
			FaceFrame::Vec hint{};
			std::string_view from = "the head magic node"sv;
			if (auto* magic = root->GetObjectByName("NPC Head MagicNode [Hmag]"sv)) {
				hint = ToVec(magic->world.translate) - ToVec(world.translate);
			}
			if (FaceFrame::Length(hint) < 1.0f) {
				const float heading = a_actor->GetAngleZ();
				FaceFrame::Vec ahead{ std::sin(heading), std::cos(heading), 0.0f };
				FaceFrame::Vec toward = ToVec(a_toward) - ToVec(world.translate);
				toward.z = 0.0f;
				if (!FaceFrame::Normalize(toward)) {
					toward = ahead;
				}
				hint = ahead * 0.6f + toward * 0.4f;
				from = "the body's heading"sv;
			}

			const auto axis = FaceFrame::ChooseAxis(columns, up, hint);
			if (axis.index < 0) {
				Log::Info(Log::Category::kStaging,
					"Face framing: no head axis of {} clearly faces forward (best {:.2f}, next {:.2f}, "
					"from {}); close-ups use the stable head point."sv,
					who, axis.score, axis.runnerUp, from);
				return;
			}

			a_track.head = RE::NiPointer<RE::NiAVObject>(head);
			a_track.axis = axis.index;
			a_track.sign = axis.sign;

			// Logged once per actor and result. In scene mode the slots swap on every line
			// and resolve the same axis again.
			static std::unordered_map<RE::FormID, int> faceReported;
			const int answer = axis.index * 2 + (axis.sign < 0.0f ? 1 : 0);
			if (auto [it, fresh] = faceReported.try_emplace(a_actor->GetFormID(), answer); !fresh) {
				if (it->second == answer) {
					return;
				}
				it->second = answer;
			}
			Log::Info(Log::Category::kStaging,
				"Face framing: {} faces along head axis {}{} (score {:.2f}, next {:.2f}, from {})."sv,
				who, axis.sign < 0.0f ? "-"sv : "+"sv, "XYZ"[axis.index], axis.score, axis.runnerUp, from);
		}

		// a_stable is the stable point before easing, so the offset is relative to the
		// body and doesn't include the anchor's lag behind a walking subject.
		void TrackFace(FaceTrack& a_track, RE::Actor* a_actor, const Anatomy& a_body,
			const RE::NiPoint3& a_stable, const RE::NiPoint3& a_toward, float a_delta)
		{
			const auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				a_track.valid = false;
				return;
			}
			if (root != a_track.root) {
				ResolveFace(a_track, a_actor, root, a_body, a_toward);
			}
			if (!a_track.head || a_track.axis < 0) {
				a_track.valid = false;
				return;
			}

			const auto&    world = a_track.head->world;
			FaceFrame::Vec facing = Column(world.rotate, a_track.axis) * a_track.sign;
			if (!FaceFrame::Normalize(facing)) {
				a_track.valid = false;
				return;
			}

			// Capped at 1.5x the head-and-shoulders extent. A bone further than that is
			// doing something the framing shouldn't chase.
			const FaceFrame::Vec live = FaceFrame::Limit(
				ToVec(world.translate) - ToVec(a_stable), 1.5f * a_body.extent);

			if (!a_track.primed) {
				a_track.offset = live;
				a_track.facing = facing;
				a_track.primed = true;
			} else {
				a_track.offset = FaceFrame::Follow(a_track.offset, live,
					kFaceDeadband * a_body.scale, kFaceFollowRate, a_delta);
				a_track.facing = FaceFrame::Turn(a_track.facing, facing, kFaceTurnRate, a_delta);
			}
			a_track.valid = true;
		}

		[[nodiscard]] static Subjects::Face FaceFor(const FaceTrack& a_track)
		{
			Subjects::Face out{};
			out.offset = { a_track.offset.x, a_track.offset.y, a_track.offset.z };
			out.facing = { a_track.facing.x, a_track.facing.y, a_track.facing.z };
			out.valid = a_track.valid;
			return out;
		}

		// The face bearing and height the current shot cut on. See heldSweep.
		float heldFaceYaw{ kUnheld };
		float heldFacePitch{ kUnheld };

		// Scene mode: the face offset the current shot cut on, and which shot that
		// was. See OnThirdPersonUpdate.
		RE::NiPoint3      heldFaceOffset{};
		Clock::time_point faceOffsetShot{};

		[[nodiscard]] RE::NiPoint3 Cross(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		}

		[[nodiscard]] RE::NiPoint3 Normalized(const RE::NiPoint3& a_v, bool& a_ok)
		{
			const float length = a_v.Length();
			a_ok = length > 1.0e-3f;
			return a_ok ? RE::NiPoint3{ a_v.x / length, a_v.y / length, a_v.z / length } : RE::NiPoint3{};
		}

		// How the camera transform is written. Diagnostic; 0 is normal. Kept for
		// investigating player lip sync problems.
		//
		//   mode  local  world  Update  time   flags   camera
		//   0     yes    yes    yes     delta  0x2000  moves
		//   1     yes    yes    no      -      -       doesn't move
		//   2     no     yes    yes     delta  0x2000  doesn't move
		//   3     yes    yes    yes     0.0    0x2000  moves
		//   4     yes    yes    yes     0.0    0x0000  moves
		//   5     yes    yes    yes     delta  0x2000  moves, also syncs ThirdPersonState
		//
		// Mode 2 runs everything else exactly as mode 0 and only skips the `local`
		// write, so it's the control for "is it the write mechanism or the camera's
		// final position". The `world` write is effectively a dead store: Update
		// recomputes world from the parent and `local`.
		int poseMode{ 0 };

		// Diagnostic: hold a frontal close-up on the player for the opening of a
		// conversation, in milliseconds. 0 is off. Used to test whether the engine
		// decides on the player's facegen morphs from the camera position at the
		// moment a conversation opens.
		[[nodiscard]] bool HoldingOpenShot()
		{
			return openShotHold > 0 && Clock::now() < openShotUntil;
		}

		[[nodiscard]] std::string_view PoseModeName(int a_mode) noexcept
		{
			switch (a_mode) {
			case 0:  return "normal"sv;
			case 1:  return "no traversal"sv;
			case 2:  return "world only"sv;
			case 3:  return "time = 0"sv;
			case 4:  return "time = 0, no flags"sv;
			case 5:  return "normal + ThirdPersonState synced"sv;
			default: return "unknown"sv;
			}
		}

		// Rotation matrix to quaternion (Shepperd's method). Only needed for pose mode
		// 5, since ThirdPersonState stores a quaternion.
		[[nodiscard]] RE::NiQuaternion QuaternionFrom(const RE::NiMatrix3& a_m)
		{
			RE::NiQuaternion q{};
			const float      trace = a_m.entry[0][0] + a_m.entry[1][1] + a_m.entry[2][2];

			if (trace > 0.0f) {
				const float s = std::sqrt(trace + 1.0f) * 2.0f;
				q.w = 0.25f * s;
				q.x = (a_m.entry[2][1] - a_m.entry[1][2]) / s;
				q.y = (a_m.entry[0][2] - a_m.entry[2][0]) / s;
				q.z = (a_m.entry[1][0] - a_m.entry[0][1]) / s;
			} else if (a_m.entry[0][0] > a_m.entry[1][1] && a_m.entry[0][0] > a_m.entry[2][2]) {
				const float s = std::sqrt(1.0f + a_m.entry[0][0] - a_m.entry[1][1] - a_m.entry[2][2]) * 2.0f;
				q.w = (a_m.entry[2][1] - a_m.entry[1][2]) / s;
				q.x = 0.25f * s;
				q.y = (a_m.entry[0][1] + a_m.entry[1][0]) / s;
				q.z = (a_m.entry[0][2] + a_m.entry[2][0]) / s;
			} else if (a_m.entry[1][1] > a_m.entry[2][2]) {
				const float s = std::sqrt(1.0f + a_m.entry[1][1] - a_m.entry[0][0] - a_m.entry[2][2]) * 2.0f;
				q.w = (a_m.entry[0][2] - a_m.entry[2][0]) / s;
				q.x = (a_m.entry[0][1] + a_m.entry[1][0]) / s;
				q.y = 0.25f * s;
				q.z = (a_m.entry[1][2] + a_m.entry[2][1]) / s;
			} else {
				const float s = std::sqrt(1.0f + a_m.entry[2][2] - a_m.entry[0][0] - a_m.entry[1][1]) * 2.0f;
				q.w = (a_m.entry[1][0] - a_m.entry[0][1]) / s;
				q.x = (a_m.entry[0][2] + a_m.entry[2][0]) / s;
				q.y = (a_m.entry[1][2] + a_m.entry[2][1]) / s;
				q.z = 0.25f * s;
			}
			return q;
		}

		[[nodiscard]] RE::ThirdPersonState* ThirdPersonStateNow()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return nullptr;
			}

			// Get the state by slot rather than currentState, so the restore can run from
			// Close() after the camera has already left third person. Writing a state that
			// isn't running just sets what it comes back to.
			auto* slot = camera->GetRuntimeData().cameraStates[RE::CameraState::kThirdPerson].get();
			return slot ? static_cast<RE::ThirdPersonState*>(slot) : nullptr;
		}

		// Sample the player's camera (lens, aim, zoom) while it's still theirs. Not
		// while the dialogue menu is up or a session is active: the engine starts
		// aiming its dialogue camera about 120 ms before Open() runs.
		void SampleCameraRest()
		{
			if (dialogueMenuUp.load(std::memory_order_relaxed) ||
				Dialogue::Session::GetSingleton().Active()) {
				return;
			}

			// Also skip while a screen-owning menu is up and for a short time after it
			// closes. The camera is still settling then, and these values get written back
			// into the persistent third-person zoom at the end of the next conversation.
			if (Dialogue::MenuWatch::ScreenTaken()) {
				return;
			}
			if (SecondsSince(screenReleasedAt) < kRestSettleSeconds) {
				return;
			}

			// The lens gets the same gates. ENB, SmoothCam and Improved Camera all write
			// worldFOV, and restingFov is also the base lens every shot composes against.
			// The floor catches SD's own narrowed lens leaking back after a Close() that
			// never ran.
			if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->GetRuntimeData2().worldFOV >= kSaneMinFov) {
				restingFov = cam->GetRuntimeData2().worldFOV;
			}

			auto* state = ThirdPersonStateNow();
			if (!state) {
				return;
			}

			cameraRest.freeRotation = state->freeRotation;
			cameraRest.posOffsetExpected = state->posOffsetExpected;
			cameraRest.posOffsetActual = state->posOffsetActual;
			cameraRest.targetZoomOffset = state->targetZoomOffset;
			cameraRest.currentZoomOffset = state->currentZoomOffset;
			cameraRest.savedZoomOffset = state->savedZoomOffset;
			cameraRest.pitchZoomOffset = state->pitchZoomOffset;
			cameraRest.targetYaw = state->targetYaw;
			cameraRest.currentYaw = state->currentYaw;
			cameraRest.freeRotationEnabled = state->freeRotationEnabled;
			cameraRestPrimed = true;
		}

		// A hand-back that's owed but hasn't happened yet (the camera was refused at
		// the time). Separate from the suspension, which only decides whether the
		// conversation may resume.
		bool              handBackPending{ false };
		Clock::time_point handBackSince{};

		// Retry interval for the queued hand-back. SmoothCam::Acquire logs on refusal,
		// so not every frame.
		constexpr float kHandBackRetrySeconds = 1.0f;

		// Put the lens back the way the player had it. Separate from Close() so
		// AbandonSuspension can do it too.
		void RestoreFieldOfView()
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (!cam || restingFov <= 1.0f) {
				return;
			}

			if (std::abs(cam->GetRuntimeData2().worldFOV - restingFov) > 0.01f) {
				Log::Info(Log::Category::kCamera,
					"Restoring field of view {:.1f} -> {:.1f}."sv, cam->GetRuntimeData2().worldFOV, restingFov);
			}
			cam->GetRuntimeData2().worldFOV = restingFov;
		}

		// Hand the view back pointing where it was. These fields are written by the
		// engine's dialogue camera during the conversation, not by SD.
		//
		// applyOffsets and toggleAnimCam are left alone: they belong to systems
		// unrelated to the conversation, and a stale value could switch off a camera
		// something else is running.
		void RestoreCameraRest()
		{
			if (!cameraRestPrimed) {
				return;
			}

			auto* state = ThirdPersonStateNow();
			if (!state) {
				return;
			}

			const RE::NiPoint2 was = state->freeRotation;
			const float        zoomWas = state->currentZoomOffset;
			const bool         freeWas = state->freeRotationEnabled;

			state->freeRotation = cameraRest.freeRotation;

			// Only ever turn free rotation off. Turning it back on because the player
			// happened to be free-looking when the sample was taken would leave the camera
			// detached.
			if (!cameraRest.freeRotationEnabled) {
				state->freeRotationEnabled = false;
			}

			state->posOffsetExpected = cameraRest.posOffsetExpected;
			state->posOffsetActual = cameraRest.posOffsetActual;

			// Leave the zoom alone when Improved Camera is installed. It uses these fields
			// to move between first and third person, and writing them from here makes the
			// view pump. SmoothCam is fine because it gets a proper handshake before this
			// runs. The aim is still restored either way, since Improved Camera doesn't
			// drive it.
			const bool restoreZoom = !Compat::ImprovedCamera::Present();
			if (restoreZoom) {
				state->targetZoomOffset = cameraRest.targetZoomOffset;
				state->currentZoomOffset = cameraRest.currentZoomOffset;
				state->savedZoomOffset = cameraRest.savedZoomOffset;
				state->pitchZoomOffset = cameraRest.pitchZoomOffset;
			}

			state->targetYaw = cameraRest.targetYaw;
			state->currentYaw = cameraRest.currentYaw;

			// Logged on every hand-back, moved or not, so a case where the pitch didn't
			// change (meaning the jump comes from somewhere else) still shows up. The
			// player's own pitch is only read, never written.
			auto*       player = RE::PlayerCharacter::GetSingleton();
			const float playerPitch = player ? player->data.angle.x : 0.0f;

			Log::Info(Log::Category::kCamera,
				"Handing the view back | free rot {} | pitch {:+.3f} -> {:+.3f} | yaw {:+.3f} -> {:+.3f} "
				"| zoom {:.2f} -> {:.2f}{} | player pitch {:+.3f} (radians, not touched)."sv,
				freeWas ? (cameraRest.freeRotationEnabled ? "on"sv : "on -> off"sv) : "off"sv,
				was.y, cameraRest.freeRotation.y,
				was.x, cameraRest.freeRotation.x,
				zoomWas, restoreZoom ? cameraRest.currentZoomOffset : zoomWas,
				restoreZoom ? ""sv : " (not written; Improved Camera owns it)"sv,
				playerPitch);
		}

		// Take the camera, restore the view, release it, or do nothing. Shared by
		// AbandonSuspension and the retry in Tick. Returns false when the camera was
		// refused, so the caller keeps the hand-back pending.
		[[nodiscard]] bool TryHandBackView()
		{
			if (!Compat::SmoothCam::Acquire()) {
				return false;
			}

			RestoreFieldOfView();
			RestoreCameraRest();
			if (restoreThirdPersonPending) {
				if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInFirstPerson()) {
					camera->ForceThirdPerson();
				}
				restoreThirdPersonPending = false;
			}
			Compat::SmoothCam::Release();
			return true;
		}

		void ApplyPose(RE::NiNode* a_root, const Pose& a_pose, float a_delta,
			RE::ThirdPersonState* a_state)
		{
			bool       ok = false;
			const auto forward = Normalized(
				{ a_pose.lookAt.x - a_pose.position.x,
					a_pose.lookAt.y - a_pose.position.y,
					a_pose.lookAt.z - a_pose.position.z },
				ok);
			if (!ok) {
				return;
			}

			// Match the visibility projection for a shot from directly overhead.
			const RE::NiPoint3 worldUp = protectSubject && std::abs(forward.z) > 0.999999f ?
				RE::NiPoint3{ 0.0f, 1.0f, 0.0f } : RE::NiPoint3{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(forward, worldUp), ok);
			if (!ok) {
				return;
			}
			const auto up = Cross(right, forward);

			// Columns are right / forward / up, matching the engine's own camera matrix.
			RE::NiMatrix3 rotation{};
			rotation.entry[0][0] = right.x;  rotation.entry[0][1] = forward.x;  rotation.entry[0][2] = up.x;
			rotation.entry[1][0] = right.y;  rotation.entry[1][1] = forward.y;  rotation.entry[1][2] = up.y;
			rotation.entry[2][0] = right.z;  rotation.entry[2][1] = forward.z;  rotation.entry[2][2] = up.z;

			a_root->world.translate = a_pose.position;
			a_root->world.rotate = rotation;

			// Mode 2 leaves local alone. The node has no meaningful parent, so world and
			// local are written to the same value.
			if (poseMode != 2) {
				a_root->local.translate = a_pose.position;
				a_root->local.rotate = rotation;
			}

			if (poseMode == 1) {
				return;  // no update traversal at all
			}

			RE::NiUpdateData update{};
			update.time = (poseMode == 3 || poseMode == 4) ? 0.0f : a_delta;
			update.flags = static_cast<RE::NiUpdateData::Flag>(poseMode == 4 ? 0x0000 : 0x2000);
			a_root->Update(update);

			// Mode 5 also writes ThirdPersonState's translation and rotation, so the
			// engine's own record of where the camera is matches the node. Additive: the
			// node write above still happens.
			if (poseMode == 5 && a_state) {
				a_state->translation = a_pose.position;
				a_state->rotation = QuaternionFrom(rotation);
			}
		}

		// Shot pools for each side of the exchange.
		//
		// kNpcCoverage is the NPC's coverage at different sizes (shoulder, tight,
		// medium, low, full figure, across the room), drawn on every line so a new
		// line usually brings a new size as well as a new angle. Each setup appears
		// once; how often it's drawn comes from its weight (see AuthoredWeight).
		// kExtremeClose is included so its weight slider actually does something.
		constexpr std::array kNpcCoverage{
			ShotType::kCloseUp,
			ShotType::kCloseLow,
			ShotType::kCloseHigh,
			ShotType::kCloseProfile,
			ShotType::kMediumNpc,
			ShotType::kOverPlayerShoulder,
			ShotType::kLowAngle,
			// The dirty single and the three-quarter are the most common angles in filmed
			// dialogue. They ship as staples in AuthoredWeight.
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kMediumProfile,

			// Over-the-shoulder height variants, as accents to the plain shoulder shot.
			ShotType::kOverPlayerShoulderLow,
			ShotType::kOverPlayerShoulderHigh,

			ShotType::kExtremeClose,
		};

		// Available once a couple of lines have gone by. Must stay a superset of
		// kNpcCoverage, otherwise a setup's weight would only apply on the first line
		// of a turn; AuditPools reports any gap.
		constexpr std::array kNpcLateCoverage{
			ShotType::kCloseUp,
			ShotType::kCloseLow,
			ShotType::kCloseProfile,
			ShotType::kCloseHigh,
			ShotType::kCloseWide,
			ShotType::kMediumNpc,
			ShotType::kOverPlayerShoulder,
			ShotType::kLowAngle,
			ShotType::kLongNpc,
			ShotType::kDistant,
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kMediumProfile,
			ShotType::kLowProfile,
			ShotType::kOverhead,
			ShotType::kOverPlayerShoulderLow,
			ShotType::kOverPlayerShoulderHigh,
			ShotType::kOverPlayerShoulderWide,

			// Listed here as well as in kNpcCoverage so its weight applies for the whole
			// turn, like kExtremeClosePlayer does on the player's side.
			ShotType::kExtremeClose,
		};

		// Every entry frames the player. Each setup once; the staples get their extra
		// weight from AuthoredWeight.
		constexpr std::array kPlayerCoverage{
			ShotType::kOverNpcShoulder,
			ShotType::kMediumPlayer,
			ShotType::kClosePlayer,

			// An accent rather than a staple. The NPC's extreme close-up is also earned by
			// line intensity, which the player's turn doesn't have.
			ShotType::kExtremeClosePlayer,

			ShotType::kDirtyPlayer,
			ShotType::kThreeQuarterPlayer,
			ShotType::kPlayerProfile,
			ShotType::kPlayerLow,
			ShotType::kHighAngle,

			// Gives the reverse the same vocabulary as the shot it answers: matching
			// over-the-shoulder heights, a full figure and an overhead.
			ShotType::kOverNpcShoulderLow,
			ShotType::kOverNpcShoulderHigh,
			ShotType::kOverNpcShoulderWide,
			ShotType::kLongPlayer,
			ShotType::kPlayerOverhead,
		};

		// The room rather than either person. Used for change of pace, mainly while
		// the player reads the topic list.
		constexpr std::array kEnvironmental{
			ShotType::kWide,
			ShotType::kDistant,
			ShotType::kProfile,
			ShotType::kTwoShot,

			// Sizes and heights the setups above don't cover.
			ShotType::kMaster,
			ShotType::kGroundLevel,
			ShotType::kDistantLow,
		};

		// Log any setup that no pool can draw, once per run. Shot::Weight() is only
		// used by the weighted walk in Coverage(), so such a setup's weight slider
		// does nothing. (The ladder and the intensity override can still reach it.)
		void AuditPools()
		{
			static Log::OnceFlag audited;
			if (!audited.Take()) {
				return;
			}

			std::array<bool, static_cast<std::size_t>(ShotType::kCount)> pooled{};
			const auto mark = [&](std::span<const ShotType> a_pool) {
				for (const auto type : a_pool) {
					const auto index = static_cast<std::size_t>(type);
					if (index < pooled.size()) {
						pooled[index] = true;
					}
				}
			};

			mark(kNpcCoverage);
			mark(kNpcLateCoverage);
			mark(kPlayerCoverage);
			mark(kEnvironmental);

			std::string orphans;
			for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
				if (pooled[i]) {
					continue;
				}
				if (!orphans.empty()) {
					orphans += ", ";
				}
				orphans += Name(static_cast<ShotType>(i));
			}

			if (orphans.empty()) {
				Log::Info(Log::Category::kCamera,
					"Shot pools cover all {} setups; every weight is live."sv,
					static_cast<std::uint32_t>(ShotType::kCount));
			} else {
				Log::Warn(Log::Category::kCamera,
					"In no shot pool, so their weight sliders do nothing: {}. "
					"They can still arrive via the intensity override or the fallback ladder."sv,
					orphans);
			}

			// The late NPC pool must contain everything the early one does, or a setup's
			// weight would stop applying after the first line of a turn.
			std::string earlyOnly;
			for (const auto type : kNpcCoverage) {
				if (std::find(kNpcLateCoverage.begin(), kNpcLateCoverage.end(), type) !=
					kNpcLateCoverage.end()) {
					continue;
				}
				if (!earlyOnly.empty()) {
					earlyOnly += ", ";
				}
				earlyOnly += Name(type);
			}

			if (!earlyOnly.empty()) {
				Log::Warn(Log::Category::kCamera,
					"In the early NPC pool but not the late one, so they stop being drawable "
					"after the first line of a turn: {}."sv,
					earlyOnly);
			}
		}

		// Shots that need space to stand in. One predicate, so new wide setups only
		// have to be added here.
		[[nodiscard]] bool NeedsRoom(ShotType a_type)
		{
			switch (a_type) {
			case ShotType::kWide:
			case ShotType::kDistant:
			case ShotType::kDistantLow:
			case ShotType::kLongNpc:
			case ShotType::kLongPlayer:
			case ShotType::kMaster:
			case ShotType::kGroundLevel:
			case ShotType::kOverPlayerShoulderWide:
			case ShotType::kOverNpcShoulderWide:
			case ShotType::kOverhead:
			case ShotType::kPlayerOverhead:
				return true;
			default:
				return false;
			}
		}

		// A cue that arrived before the conversation was staged. Only the derived
		// values are kept, not the DialogueResponse pointer, which isn't guaranteed to
		// outlive this.
		struct PendingCue
		{
			RE::ActorHandle   speaker{};
			std::uint32_t     words{ 0 };
			std::uint16_t     intensity{ 50 };
			std::uint32_t     emotion{ 0 };
			std::string       text;  // response storage can expire before greeting replay
			Clock::time_point at{};
			bool              valid{ false };
		};

		PendingCue pendingCue{};

		// How old a stashed cue may be before Open() ignores it. The usual gap is
		// about 120 ms. Being generous is cheap, since the speaker check below rejects
		// lines from anyone else (LineWatch sees every Character).
		constexpr float kGreetingGrace = 1.5f;

		// Applies a line to the direction state. Used live and when Open() replays the
		// greeting.
		void ApplyCue(RE::Actor* a_speaker, std::uint32_t a_words, std::uint16_t a_intensity,
			std::uint32_t a_emotion, std::string_view a_text);

		// Enabled and weighted above zero. Every route to the screen checks this, so a
		// weight of 0 means never.
		//
		// Over-the-shoulder shots need a humanoid in the foreground, which is the
		// party the shot is not about. kOverNpcShoulder is skipped when the NPC isn't
		// humanoid (there's no shoulder on a dragon to shoot over). Asked of the
		// skeleton via Anatomy rather than size, since a giant has shoulders but is
		// still the wrong thing to shoot over.
		[[nodiscard]] bool Composable(ShotType a_type)
		{
			if (!OverShoulder(a_type)) {
				return true;
			}
			return FavoursNpc(a_type) ? playerBody.shoulder : npcBody.shoulder;
		}

		[[nodiscard]] bool Drawable(ShotType a_type)
		{
			return Shot::Enabled(a_type) && Shot::Weight(a_type) > 0 && Composable(a_type);
		}

		// Picks one setup from a pool in proportion to its weight. Templated on the
		// predicate because it runs inside the picker. Returns nothing when no entry
		// qualifies; the caller decides the fallback.
		template <typename Predicate>
		[[nodiscard]] std::optional<ShotType> WeightedPick(
			std::span<const ShotType> a_pool, Predicate a_eligible)
		{
			std::uint32_t total = 0;
			for (const auto type : a_pool) {
				if (a_eligible(type)) {
					total += static_cast<std::uint32_t>(Shot::Weight(type));
				}
			}

			if (total == 0) {
				return std::nullopt;
			}

			std::uint32_t roll = NextRandom() % total;
			for (const auto type : a_pool) {
				if (!a_eligible(type)) {
					continue;
				}
				const auto weight = static_cast<std::uint32_t>(Shot::Weight(type));
				if (roll < weight) {
					return type;
				}
				roll -= weight;
			}

			return std::nullopt;
		}

		// Total weight a pool can currently field, for choosing between pools.
		[[nodiscard]] std::uint32_t PoolWeight(std::span<const ShotType> a_pool)
		{
			std::uint32_t total = 0;
			for (const auto type : a_pool) {
				if (Drawable(type) && !(NeedsRoom(type) && !roomy)) {
					total += static_cast<std::uint32_t>(Shot::Weight(type));
				}
			}
			return total;
		}

		constexpr auto kAllShots = [] {
			std::array<ShotType, static_cast<std::size_t>(ShotType::kCount)> types{};
			for (std::size_t i = 0; i < types.size(); ++i) {
				types[i] = static_cast<ShotType>(i);
			}
			return types;
		}();

		// Prefer the requested subject, then neutral coverage, then the other subject
		// if that's all that's enabled. An empty set has no default.
		[[nodiscard]] std::optional<ShotType> Canonical()
		{
			const bool wantNpc = SubjectIsNpc();
			const auto preferred = wantNpc ? ShotType::kOverPlayerShoulder : ShotType::kOverNpcShoulder;
			return BestAvailable(std::span{ kAllShots }, Drawable, [&](ShotType type) {
				if (FramingIsRoom() && type == ShotType::kTwoShot) {
					return 5.0f;
				}
				if (type == preferred) {
					return 4.0f;
				}
				const float preference = IsNeutral(type) ? 1.0f :
					(FavoursNpc(type) == wantNpc ? 2.0f : 0.0f);
				return preference + FillOf(type);
			});
		}

		// A different shot of the right subject. Excludes the shot already on screen,
		// so a new line brings a new angle and size.
		[[nodiscard]] std::optional<ShotType> Coverage()
		{
			// Two lines into a speech unlocks the wider NPC vocabulary (most turns are
			// only one or two lines long).
			//
			// A forced framing is honoured exactly: no environmental shots and no side
			// switching while it's active.
			if (FramingIsRoom()) {
				const auto room = WeightedPick(kEnvironmental, [&](ShotType a_type) {
					return a_type != currentShot && Drawable(a_type) &&
						!(NeedsRoom(a_type) && !roomy);
				});
				return room ? room : Canonical();
			}

			const bool wantNpc = SubjectIsNpc();
			const bool forced = framing != Framing::kAuto;

			std::span<const ShotType> pool = linesThisTurn >= 2 ?
				std::span<const ShotType>{ kNpcLateCoverage } :
				std::span<const ShotType>{ kNpcCoverage };

			if (forced) {
				// `pool` already holds the NPC side for this point in the turn. A forced
				// framing is about who, not about narrowing the choice.
				if (!wantNpc) {
					pool = std::span<const ShotType>{ kPlayerCoverage };
				}
			} else if (wantNpc) {
				// Room shots as a change of pace inside the NPC's line. Rolled against the
				// coverage pool's total weight rather than a fixed ratio, so the room's share
				// follows its weights and zero room weight means no room shots. Rolled rather
				// than counted because Coverage() runs again for every rejected candidate.
				const auto roomWeight = PoolWeight(kEnvironmental);
				const auto coverWeight = PoolWeight(pool);
				const auto pot = roomWeight + coverWeight;
				if (pot > 0 && (NextRandom() % pot) < roomWeight) {
					pool = std::span<const ShotType>{ kEnvironmental };
				}
			} else {
				// Same weighted roll on the player's turn. Coverage() can run several times
				// per cut (once per rejected candidate), so anything that alternates per call
				// would effectively be a coin toss.
				pool = std::span<const ShotType>{ kPlayerCoverage };

				// A reaction always shows the player, never the room.
				if (!coverPlayerTurn && !turnSinceCut && !ReactionActive()) {
					// Never on the first cut of the player's turn: the first shot after the reply
					// should show the player. turnSinceCut stays true until that first cut
					// commits, so it survives the retry loop.
					const auto roomWeight = PoolWeight(kEnvironmental);
					const auto coverWeight = PoolWeight(kPlayerCoverage);
					const auto pot = roomWeight + coverWeight;
					if (pot > 0 && (NextRandom() % pot) < roomWeight) {
						pool = std::span<const ShotType>{ kEnvironmental };
					}
				}
			}

			// Weighted draw. Walking the pool once always finds an eligible entry if there
			// is one, and the weight read here is the slider value.
			const auto pick = WeightedPick(pool, [&](ShotType a_type) {
				// Disabled or zero-weight setups must be unreachable. Wide or distant setups
				// need room.
				return a_type != currentShot && Drawable(a_type) &&
					!(NeedsRoom(a_type) && !roomy);
			});

			return pick ? pick : Canonical();
		}

		// Last space classification logged (class * 2 + roomy), or -1 since the camera
		// last let go. Scenes re-stage on every line, so this keeps the log to
		// changes.
		int spaceReported = -1;

		// Direction with the most open space around the conversation. Probed rather
		// than assumed, once per conversation; re-probing every frame would make the
		// distant shot wander.
		void ProbeOpenDirection(const RE::NiPoint3& a_player, const RE::NiPoint3& a_npc)
		{
			const RE::NiPoint3 midpoint{
				(a_player.x + a_npc.x) * 0.5f, (a_player.y + a_npc.y) * 0.5f, (a_player.z + a_npc.z) * 0.5f
			};

			constexpr int kSamples = 12;

			// How far the probe looks for room. Distant setups are clamped to about 90% of
			// this, so it has to be long enough for outdoor shots. Indoors the walls limit
			// it anyway.
			constexpr float kReach = 2600.0f;
			constexpr float kTwoPi = 6.28318530718f;

			// All twelve bearings are used. Two bearings perpendicular to the eyeline
			// would call a long hall cramped when the conversation runs along it.
			float best = -1.0f;
			float total = 0.0f;
			int   longBearings = 0;

			for (int i = 0; i < kSamples; ++i) {
				const float angle = kTwoPi * (static_cast<float>(i) / static_cast<float>(kSamples));
				const RE::NiPoint3 direction{ std::cos(angle), std::sin(angle), 0.0f };

				const float room = Clearance(midpoint, direction, kReach);
				total += room;
				if (room >= 500.0f) {
					++longBearings;
				}
				if (room > best) {
					best = room;
					openDirection = direction;
					openDistance = room;
				}
			}

			const float mean = total / static_cast<float>(kSamples);

			// A wide shot needs one place to stand, not open space in every direction. A
			// corridor passes on its long axis.
			roomy = openDistance >= 320.0f;

			// Classified by the mean, not the max: a corridor and a square can share a
			// max.
			roomSpace = mean >= 620.0f ? Space::kOpen :
					mean >= 210.0f ? Space::kRoom :
									 Space::kTight;

			// One ray up. `rise` is the only absolute-units column in the shot table, and
			// Solve clamps it against this so overheads don't go through ceilings.
			constexpr float kUpReach = 520.0f;
			ceilingRoom = Clearance(midpoint, RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, kUpReach);

			const int answer = static_cast<int>(roomSpace) * 2 + (roomy ? 1 : 0);
			if (answer != spaceReported) {
				spaceReported = answer;
				Log::Info(Log::Category::kStaging,
					"Space probed: widest {:.0f}u, mean {:.0f}u, {} of {} bearings long, {:.0f}u overhead -> {} space; wide shots {}."sv,
					openDistance, mean, longBearings, kSamples, ceilingRoom,
					SpaceName(roomSpace), roomy ? "allowed"sv : "suppressed"sv);
			}
		}

		// Line cadence and reaction counts restart with each NPC reply.
		void ResetReplyCounters()
		{
			if (reactionShots.Active()) {
				Log::Info(Log::Category::kContinuity, "Reaction over; a new reply started."sv);
			}
			reactionShots.Reset();
			linesSinceCut = 0;
			cutEveryTarget = RollCutEvery();
			linesThisTurn = 0;
		}

		// Full-intensity lines count toward a reaction but can't be one, since
		// Choose() gives those to the NPC close-up.
		void UpdateReaction(bool a_eligible, std::uint16_t a_intensity)
		{
			const bool wasActive = reactionShots.Active();
			const bool wasOwed = reactionShots.Owed();
			const std::uint32_t roll = reactionSettings.enabled ? NextRandom() % 100u : 0u;
			reactionShots.OnNpcLine(reactionSettings, a_eligible, a_intensity < kCloseUpIntensity,
				framing == Framing::kAuto, roll);

			if (reactionShots.Active()) {
				Log::Info(Log::Category::kContinuity, "Reaction: this line plays on you."sv);
			} else if (wasActive) {
				Log::Info(Log::Category::kContinuity, "Reaction over; back to them."sv);
			}
			if (reactionShots.Owed() && !wasOwed) {
				Log::Info(Log::Category::kContinuity,
					"Reaction rolled ({}% after {} line(s)); next line plays on you."sv,
					reactionSettings.chance, reactionSettings.every);
			}
		}

		// Feeds topic picks to replyBoundary: the menu's click, or a voiced player
		// line starting.
		void TrackTopicPicks(const Scene::Interface::DialoguePhase& a_phase, float a_delta)
		{
			using Phase = Scene::Interface::MenuPhase;
			replyBoundary.Advance(a_delta);

			const bool clicked = a_phase.valid && a_phase.phase == Phase::kTopicClicked &&
				lastPickPhase != Phase::kTopicClicked;
			if (a_phase.valid) {
				lastPickPhase = a_phase.phase;
			}

			const auto serial = Scene::LipSync::PlayerLineSerial();
			const bool spoke = serial != lastPlayerLineSerial;
			lastPlayerLineSerial = serial;

			if (clicked || spoke) {
				replyBoundary.OnPick();

				// A pick is a new question, so its reply is classified again even when it's
				// the same record as last time (a failed persuasion asked again).
				beatInfo = nullptr;

				// What the prompt said, read while the highlight is still on the chosen row.
				const auto prompt = clicked ? Scene::Interface::ReadSelectedTopic() :
				                              std::string{ Scene::LipSync::PlayerLineText() };
				if (const auto check = SpeechChecks::ClassifyPrompt(prompt); check != SpeechCheck::kNone) {
					pickedCheck = check;
				}
			}
		}

		// ---- Persuasion beat ---------------------------------------------------

		// The speech check a reply record carries, if any.
		[[nodiscard]] SpeechCheck ConditionsOf(const RE::TESTopicInfo* a_info)
		{
			for (auto* item = a_info ? a_info->objConditions.head : nullptr; item; item = item->next) {
				const auto& data = item->data;
				const CheckCondition condition{
					static_cast<std::uint16_t>(data.functionData.function.underlying()),
					static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data.functionData.params[0])),
					static_cast<std::uint8_t>(data.flags.opCode),
					static_cast<bool>(data.flags.global)
				};
				if (const auto check = SpeechChecks::Classify(condition); check != SpeechCheck::kNone) {
					return check;
				}
			}
			return SpeechCheck::kNone;
		}

		// Is this reply the answer to a speech check? The reply's own conditions
		// first; then, for a small topic, its siblings, since the failure reply is the
		// unconditioned fallback. See PersuasionBeat.h.
		[[nodiscard]] SpeechCheck ClassifyReply(const RE::TESTopicInfo* a_info)
		{
			if (!a_info) {
				return SpeechCheck::kNone;
			}
			if (const auto own = ConditionsOf(a_info); own != SpeechCheck::kNone) {
				return own;
			}

			const auto* topic = a_info->parentTopic;
			if (!topic || !topic->topicInfos || topic->numTopicInfos > SpeechChecks::kMaxCheckTopic) {
				return SpeechCheck::kNone;
			}
			for (std::uint32_t i = 0; i < topic->numTopicInfos; ++i) {
				const auto* sibling = topic->topicInfos[i];
				if (sibling && sibling != a_info) {
					if (const auto check = ConditionsOf(sibling); check != SpeechCheck::kNone) {
						return check;
					}
				}
			}
			return SpeechCheck::kNone;
		}

		// Shots the beat may cut to, tightest first.
		constexpr std::array kBeatShots{
			ShotType::kCloseUp,
			ShotType::kExtremeClose,
			ShotType::kCloseLow,
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kMediumNpc,
		};

		[[nodiscard]] bool IsBeatShot(ShotType a_type)
		{
			return std::find(kBeatShots.begin(), kBeatShots.end(), a_type) != kBeatShots.end();
		}

		// The first of those that's enabled and not already on screen, so the answer
		// arrives on a cut.
		[[nodiscard]] std::optional<ShotType> BeatShot()
		{
			for (const auto type : kBeatShots) {
				if (type != currentShot && Drawable(type)) {
					return type;
				}
			}
			return std::nullopt;
		}

		void EndBeat(std::string_view a_why)
		{
			if (persuasion.Active()) {
				Log::Info(Log::Category::kContinuity, "Persuasion beat over: {}."sv, a_why);
			}
			persuasion.End();
		}

		// Watches for the reply to a speech check. Runs every staged frame of the
		// player's own conversation; each reply record is looked at once.
		void TickPersuasion()
		{
			if (!persuasionBeatEnabled) {
				EndBeat("switched off"sv);
				pickedCheck = SpeechCheck::kNone;
				return;
			}

			auto*       manager = RE::MenuTopicManager::GetSingleton();
			const auto* info = manager ? manager->currentTopicInfo : nullptr;
			if (!info || info == beatInfo) {
				return;
			}
			beatInfo = info;

			auto       check = ClassifyReply(info);
			const bool fromPrompt = check == SpeechCheck::kNone && pickedCheck != SpeechCheck::kNone;
			if (fromPrompt) {
				check = pickedCheck;
			}
			pickedCheck = SpeechCheck::kNone;

			if (check == SpeechCheck::kNone) {
				EndBeat("the conversation moved on"sv);
				return;
			}
			if (persuasion.Active()) {
				return;  // a reply chained onto the answer is still the answer
			}

			persuasion.Begin(check);
			reactionShots.Reset();
			Log::Info(Log::Category::kContinuity,
				"Persuasion beat: {} answered; holding on them for the reply ({})."sv,
				SpeechCheckName(check), fromPrompt ? "from the prompt"sv : "from the reply's conditions"sv);

			// The camera may have just cut to them as the player's voiced line ended.
			// Cutting again a fraction of a second later would stutter, so a shot that
			// arrived recently is adopted instead: pushed in if it's a close single,
			// otherwise held.
			constexpr float kAdoptSeconds = 1.0f;
			if (!IsNeutral(currentShot) && FavoursNpc(currentShot) && SecondsSince(shotSince) < kAdoptSeconds) {
				persuasion.Cut();
				if (IsBeatShot(currentShot)) {
					beatShot = currentShot;
					beatShotSince = shotSince;
					beatPushSeconds = SpeechChecks::PushSeconds(lastCueWords);
				}
				Log::Info(Log::Category::kContinuity,
					"Persuasion beat: already on {}; {} it for the answer."sv,
					Name(currentShot), IsBeatShot(currentShot) ? "pushing in on"sv : "holding"sv);
			}
		}

		// ---- Scene mode helpers ------------------------------------------------

		void Notify(const char* a_text)
		{
			// CommonLibSSE-NG 7 dropped RE::DebugNotification; this calls the same engine
			// function through the same Address Library IDs.
			static REL::Relocation<void (*)(const char*, const char*, bool)> showNotification{
				REL::RelocationID(52050, 52933)
			};
			showNotification(a_text, nullptr, true);
		}

		[[nodiscard]] float Distance(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		// The other half of every shot: the player, or the second NPC in scene mode.
		[[nodiscard]] RE::NiPointer<RE::Actor> OtherParty()
		{
			if (sceneMode) {
				return counterpart.get();
			}
			return RE::NiPointer<RE::Actor>{ RE::PlayerCharacter::GetSingleton() };
		}

		// Movement input from the player (not camera input).
		[[nodiscard]] bool PlayerMoving()
		{
			auto* controls = RE::PlayerControls::GetSingleton();
			if (!controls) {
				return false;
			}
			const auto& move = controls->data.moveInputVec;
			return move.x * move.x + move.y * move.y > 0.04f;
		}

		// Any sound the actor is playing, which during a scene is their line.
		[[nodiscard]] bool SoundPlaying(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			auto* high = process ? process->high : nullptr;
			if (!high) {
				return false;
			}
			for (const auto& handle : high->soundHandles) {
				if (handle.soundID != RE::BSSoundHandle::kInvalidID &&
					handle.state.get() == RE::BSSoundHandle::AssumedState::kPlaying) {
					return true;
				}
			}
			return false;
		}

		// Anything that rules out filming other NPCs, by key or automatically.
		[[nodiscard]] bool SceneBlocked(bool a_automatic)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			if (!player || !ui || !directing) {
				return true;
			}
			if (player->IsInCombat() || player->IsOnMount() || ui->GameIsPaused() ||
				Dialogue::MenuWatch::ScreenTaken() || DialogueMenuOpen() ||
				Dialogue::Session::GetSingleton().Active() || suspendedForMenu) {
				return true;
			}
			// A drawn weapon usually means the player is about to do something else. The
			// key still works; auto mode stays out of it.
			if (a_automatic) {
				const auto* state = player->AsActorState();
				if (state && state->IsWeaponDrawn()) {
					return true;
				}
			}
			return false;
		}

		// A scene the game is playing is filmed as a whole, not as a fixed pair. Every
		// line from anyone in it is taken, and the camera pairs the speaker with
		// whoever they're talking to. Filming lasts as long as the scene does.

		// An actor's body heading, flattened.
		[[nodiscard]] RE::NiPoint3 Heading(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return {};
			}
			const float z = a_actor->GetAngleZ();
			return { std::sin(z), std::cos(z), 0.0f };
		}

		[[nodiscard]] RE::BGSScene* SceneOf(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->GetCurrentScene() : nullptr;
		}

		// What auto mode and the snooze know a filmed thing by: the game scene when
		// there is one, so every pair from it counts as the same thing; otherwise the
		// pair.
		[[nodiscard]] std::uint64_t KeyFor(RE::Actor* a_speaker, std::uint64_t a_pairKey)
		{
			if (auto* scene = SceneOf(a_speaker)) {
				return (std::uint64_t{ 0xFFFFFFFFu } << 32) | scene->GetFormID();
			}
			return a_pairKey;
		}

		[[nodiscard]] bool InFilmedScene(RE::Actor* a_actor)
		{
			auto* scene = SceneOf(a_actor);
			return sceneForm != 0 && scene && scene->GetFormID() == sceneForm;
		}

		[[nodiscard]] bool FilmedScenePlaying()
		{
			if (sceneForm == 0) {
				return false;
			}
			auto* scene = RE::TESForm::LookupByID<RE::BGSScene>(sceneForm);
			return scene && scene->isPlaying;
		}

		// What a playing scene has left to say from its current phase. See
		// SceneScript.h.
		//
		// currentPhaseIndex is a 0-based index into the phase array (that's how 1.5.97
		// BGSScene::Update at 0x348490 uses it), -1 when nothing is running, and the
		// scene winds down once it reaches the phase count. Action phase numbers use
		// the same indexing.
		//
		// Returns kScripted when the scene can't be read, so unknown cases behave as
		// before.
		[[nodiscard]] Dialogue::Ahead AheadIn(const RE::BGSScene* a_scene)
		{
			if (!a_scene || !a_scene->isPlaying) {
				return Dialogue::Ahead::kScripted;
			}
			const std::uint32_t phase = a_scene->currentPhaseIndex;
			const auto          count = static_cast<std::uint32_t>(a_scene->phases.size());
			if (phase == 0xFFFFFFFFu || count == 0) {
				return Dialogue::Ahead::kScripted;
			}
			if (phase >= count) {
				return Dialogue::Ahead::kNothing;
			}

			static std::vector<Dialogue::ScriptLine> lines;
			lines.clear();
			for (const auto* action : a_scene->actions) {
				if (!action || action->GetType() != RE::BGSSceneAction::Type::kDialogue) {
					continue;
				}
				// Dialogue actions without a topic say nothing; they're editor placeholders.
				if (!static_cast<const RE::BGSSceneActionDialogue*>(action)->topic) {
					continue;
				}
				lines.push_back({ action->startPhase, action->endPhase,
					action->flags.all(RE::BGSSceneAction::Flag::kLooping) });
			}
			return Dialogue::Look(lines, phase, count);
		}

		[[nodiscard]] Dialogue::Ahead FilmedSceneAhead()
		{
			return sceneForm != 0 ? AheadIn(RE::TESForm::LookupByID<RE::BGSScene>(sceneForm)) :
			                        Dialogue::Ahead::kScripted;
		}

		// Whether the filmed scene has handed the player their turn: only repeats or
		// nothing ahead, and the player didn't explicitly ask to watch it.
		[[nodiscard]] bool SceneHandingOff(Dialogue::Ahead a_ahead)
		{
			if (a_ahead == Dialogue::Ahead::kScripted) {
				sceneWaitExempt = false;
				return false;
			}
			if (sceneWaitExempt) {
				return false;
			}

			auto*               scene = RE::TESForm::LookupByID<RE::BGSScene>(sceneForm);
			const std::uint32_t phase = scene ? scene->currentPhaseIndex : 0;
			const std::uint64_t key = (std::uint64_t{ sceneForm } << 32) | phase;
			if (key != sceneAheadReported) {
				sceneAheadReported = key;
				Log::Info(Log::Category::kCamera, "Scene {:08X} phase {}: {}; handing the camera back after the line."sv,
					sceneForm, phase,
					a_ahead == Dialogue::Ahead::kMarkingTime ?
						"only repeating lines ahead, the game is waiting on you"sv :
						"no lines left"sv);
			}
			return true;
		}

		[[nodiscard]] bool Present(RE::Actor* a_actor)
		{
			return a_actor && a_actor->Is3DLoaded() && !a_actor->IsDead() && !a_actor->IsDisabled();
		}

		// How far away someone can be and still be the one a line is said to.
		constexpr float kAddresseeReach = 1500.0f;

		// Who the speaker is talking to. The engine's answer first (dialogue target or
		// head-track target); otherwise whoever the speaker faces most squarely among
		// the recent voices, the two being filmed and the player. Null when nobody is
		// in front of them.
		[[nodiscard]] RE::NiPointer<RE::Actor> Addressee(RE::Actor* a_speaker)
		{
			if (!Present(a_speaker)) {
				return {};
			}
			const auto here = a_speaker->GetPosition();

			const auto forward = Heading(a_speaker);
			const auto facingOf = [&](RE::Actor* a_actor, float& a_flat) {
				const auto  there = a_actor->GetPosition();
				const float dx = there.x - here.x;
				const float dy = there.y - here.y;
				a_flat = std::sqrt(dx * dx + dy * dy);
				return a_flat < 1.0f ? 1.0f : (forward.x * dx + forward.y * dy) / a_flat;
			};

			// Trust the engine's listener unless they're behind the speaker. Filming a
			// line "to" someone behind you would show the back of the speaker's head.
			if (auto listener = Dialogue::SceneWatch::Listener(a_speaker);
				listener && listener.get() != a_speaker && Present(listener.get())) {
				float flat = 0.0f;
				if (facingOf(listener.get(), flat) > -0.2f && flat <= kAddresseeReach) {
					return listener;
				}
			}

			std::vector<RE::NiPointer<RE::Actor>> candidates;
			candidates.push_back(subject.get());
			candidates.push_back(counterpart.get());
			candidates.push_back(RE::NiPointer<RE::Actor>{ RE::PlayerCharacter::GetSingleton() });
			auto* speakerScene = SceneOf(a_speaker);
			for (const auto& handle : Dialogue::SceneWatch::RecentSpeakers(30.0f)) {
				auto actor = handle.get();
				if (actor && (!speakerScene || SceneOf(actor.get()) == speakerScene)) {
					candidates.push_back(actor);
				}
			}

			RE::NiPointer<RE::Actor> best{};
			float                    bestScore = -1.0f;
			for (auto& candidate : candidates) {
				if (!candidate || candidate.get() == a_speaker || !Present(candidate.get())) {
					continue;
				}
				float       flat = 0.0f;
				const float facing = facingOf(candidate.get(), flat);
				if (flat < 1.0f || flat > kAddresseeReach) {
					continue;
				}
				if (facing < 0.3f) {
					continue;  // not in front of them
				}
				const float score = facing - flat / 3000.0f;
				if (score > bestScore) {
					bestScore = score;
					best = candidate;
				}
			}
			return best;
		}

		// One half of the frame changing hands. Everything measured from the old
		// occupant is reset and the camera cuts, so it never glides between people.
		void ResetSlot(bool a_npcSlot)
		{
			if (a_npcSlot) {
				npcBody = {};
				npcAnchor = {};
				npcFaceTrack = {};
				lastNpcPose = {};
			} else {
				playerBody = {};
				playerAnchor = {};
				playerFaceTrack = {};
				lastPlayerPose = {};
			}
			posturePrimed = false;
			frameSubjects.reset();
			protectedPose = {};
			protectedSearch = {};
			haveShot = false;  // a new eyeline, and a new room to look across
			heldSince = {};
			forcedCut = true;
		}

		[[nodiscard]] const char* NameOf(RE::Actor* a_actor)
		{
			const char* name = a_actor ? a_actor->GetName() : nullptr;
			return (name && *name) ? name : "<unnamed>";
		}

		// Put a_speaker in the frame with a_listener, moving as little as possible:
		// anyone already in a slot stays there. The player is only ever the second
		// slot. Returns whether the speaker ended up in the first slot.
		bool Pair(RE::Actor* a_speaker, RE::Actor* a_listener)
		{
			auto  first = subject.get();
			auto  second = counterpart.get();
			auto* player = static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton());

			const auto setFirst = [&](RE::Actor* a_actor) {
				if (first.get() != a_actor) {
					subject = a_actor->GetHandle();
					ResetSlot(true);
				}
			};
			const auto setSecond = [&](RE::Actor* a_actor) {
				if (second.get() != a_actor) {
					counterpart = a_actor->GetHandle();
					ResetSlot(false);
				}
			};

			bool speakerFirst = true;
			if (a_listener == player || (a_speaker == first.get() && !a_listener)) {
				setFirst(a_speaker);
				if (a_listener) {
					setSecond(a_listener);
				}
			} else if (a_speaker == first.get()) {
				setSecond(a_listener);
			} else if (a_speaker == second.get()) {
				if (a_listener && a_listener != first.get()) {
					setFirst(a_listener);
				}
				speakerFirst = false;
			} else if (a_listener && a_listener == first.get()) {
				setSecond(a_speaker);
				speakerFirst = false;
			} else if (a_listener && a_listener == second.get()) {
				setFirst(a_speaker);
			} else {
				setFirst(a_speaker);
				if (a_listener) {
					setSecond(a_listener);
				}
			}

			auto nowFirst = subject.get();
			auto nowSecond = counterpart.get();
			if (nowFirst.get() != first.get() || nowSecond.get() != second.get()) {
				Log::Info(Log::Category::kContinuity, "Scene: {} speaking{}{}; filming {} and {}."sv,
					NameOf(a_speaker), a_listener ? " to "sv : ""sv, a_listener ? NameOf(a_listener) : "",
					NameOf(nowFirst.get()), NameOf(nowSecond.get()));
			}
			return speakerFirst;
		}

		// Someone being filmed died, was disabled or unloaded while the scene is still
		// going. Carry on with whoever is left instead of letting go.
		[[nodiscard]] bool ReplaceGone()
		{
			auto first = subject.get();
			auto second = counterpart.get();
			auto* player = static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton());

			RE::Actor* stays = Present(first.get()) ? first.get() :
				(Present(second.get()) && second.get() != player ? second.get() : nullptr);
			if (!stays) {
				return false;
			}
			auto other = Addressee(stays);
			Pair(stays, other ? other.get() : player);
			return Present(subject.get().get()) && Present(counterpart.get().get());
		}

		bool StartScene(const Dialogue::SceneCast& a_cast, bool a_automatic)
		{
			auto speaker = a_cast.speaker.get();
			auto other = a_cast.other.get();
			if (!speaker || !other || staging) {
				return false;
			}

			// In a game scene, the other half of the frame is whoever the speaker is
			// addressing, not just the last other voice.
			auto* scene = SceneOf(speaker.get());
			if (scene) {
				if (auto addressed = Addressee(speaker.get()); addressed && addressed.get() != speaker.get()) {
					other = addressed;
				}
			}

			sceneMode = true;
			sceneForm = scene ? scene->GetFormID() : 0;
			counterpart = other->GetHandle();
			sceneAutomatic = a_automatic;
			sceneKey = KeyFor(speaker.get(), a_cast.key);
			sceneSpeakerIsA = true;
			sceneLineAt = Clock::now();
			sceneLineSeconds = 0.0f;
			sceneTalkSeen = false;

			// The key was pressed on a scene that's already only repeating itself; the
			// player wants to watch it, so don't hand it straight back.
			sceneWaitExempt = !a_automatic && AheadIn(scene) != Dialogue::Ahead::kScripted;
			sceneAheadReported = 0;
			if (sceneWaitExempt) {
				Log::Info(Log::Category::kCamera,
					"The scene is only repeating itself; filmed anyway because you asked, until you move."sv);
			}

			// Time for the next line before silence counts. Longer for the key, which is
			// often pressed in a pause between lines.
			sceneQuietSince = Clock::now() + std::chrono::seconds(a_automatic ? 2 : 6);

			Log::Info(Log::Category::kCamera,
				"Filming {} and {} ({}; the second found as {}){}."sv, NameOf(speaker.get()), NameOf(other.get()),
				a_automatic ? "you stood still"sv : "by key"sv, a_cast.how,
				scene ? "; the whole scene is filmed, whoever speaks"sv : ""sv);

			Director::Open(speaker.get());

			if (!staging) {
				// SmoothCam refused, or Open() found nothing to stage. Only the scene fields
				// need undoing.
				sceneMode = false;
				sceneForm = 0;
				counterpart = {};
				Log::Info(Log::Category::kCamera, "Filming declined; the camera was not available."sv);
				return false;
			}
			return true;
		}

		// The key: film whatever is in front of the player, or stop filming.
		void ToggleScene()
		{
			if (staging && sceneMode) {
				Log::Info(Log::Category::kCamera, "Stopped filming: by key."sv);
				sceneTrigger.Snooze(sceneKey);
				Director::Close();
				return;
			}
			if (staging) {
				Log::Info(Log::Category::kCamera,
					"Film key ignored: you are in a conversation of your own."sv);
				return;
			}
			if (SceneBlocked(false)) {
				Log::Info(Log::Category::kCamera, "Film key ignored: not now (combat, a menu, a mount, or the mod is off)."sv);
				return;
			}

			// The key reaches further than auto mode (a deliberate press at a scene across
			// the room), looks 20 seconds back (banter has long pauses), and accepts about
			// 65 degrees from the view direction.
			const auto cast = Dialogue::SceneWatch::Find(std::max(sceneRange, 1000.0f), false, 0.42f, 20.0f);
			if (!cast) {
				Log::Info(Log::Category::kCamera, "Film key: nobody in front of you has spoken lately."sv);
				Notify("Nobody nearby is talking.");
				return;
			}
			static_cast<void>(StartScene(*cast, false));
		}

		// Auto mode, every frame nothing is staged.
		void TickAutoScene(float a_delta)
		{
			if (!sceneTriggerSettings.enabled) {
				return;
			}

			const bool blocked = SceneBlocked(true);
			const bool still = !PlayerMoving();

			std::optional<Dialogue::SceneCast> cast{};
			if (!blocked && still) {
				// Only lines from the last few seconds, in strict mode, and closer to the
				// center of the view (about 40 degrees). Standing still is a weaker signal
				// than a key press. See SceneWatch::Find.
				cast = Dialogue::SceneWatch::Find(sceneRange, true, 0.77f, 8.0f);
				if (cast) {
					// Keyed by scene when there is one, so any pair from an already filmed scene
					// counts as that scene.
					auto speaker = cast->speaker.get();
					cast->key = KeyFor(speaker.get(), cast->key);

					// A scene repeating itself while it waits on the player is the game asking the
					// player to move, not something to film. Two NPCs nagging in turn would
					// otherwise look like a conversation.
					if (AheadIn(SceneOf(speaker.get())) != Dialogue::Ahead::kScripted) {
						cast.reset();
					}
				}
				if (cast && cast->key == sceneEndedKey && cast->sinceLine >= SecondsSince(sceneEndedAt)) {
					cast.reset();  // nothing said since it was last filmed
				}
			}

			const Dialogue::SceneTrigger::Input input{
				cast.has_value(), cast ? cast->key : 0, still, blocked, std::clamp(a_delta, 0.0f, 0.25f)
			};
			if (sceneTrigger.Update(sceneTriggerSettings, input) && cast) {
				static_cast<void>(StartScene(*cast, true));
			}
		}

		// Whether the filmed scene is over, and why.
		[[nodiscard]] bool SceneOver(std::string_view& a_why)
		{
			auto  a = subject.get();
			auto  b = counterpart.get();
			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool playing = FilmedScenePlaying();

			if (!a || !b || !player || a->IsDead() || b->IsDead() || !a->Is3DLoaded() || !b->Is3DLoaded()) {
				if (!(playing && ReplaceGone())) {
					a_why = "one of them is gone"sv;
					return true;
				}
				a = subject.get();
				b = counterpart.get();
			}

			// The player fighting always ends it. The filmed NPCs fighting only ends it
			// outside a game scene, where combat can be part of the scene.
			if (player->IsInCombat() || (!playing && (a->IsInCombat() || b->IsInCombat()))) {
				a_why = "combat"sv;
				return true;
			}

			// Moving takes the camera back. The scene is snoozed so auto mode doesn't
			// start again the moment the player stops.
			if (PlayerMoving()) {
				a_why = "you moved"sv;
				sceneTrigger.Snooze(sceneKey);
				return true;
			}

			// Runtime normally takes the camera for the player's own conversation in the
			// same frame; this is the fallback.
			if (Dialogue::Session::GetSingleton().Active() || DialogueMenuOpen()) {
				if (!sceneTalkSeen) {
					sceneTalkSeen = true;
					sceneTalkSince = Clock::now();
				} else if (SecondsSince(sceneTalkSince) > 1.0f) {
					a_why = "your own conversation started"sv;
					return true;
				}
			} else {
				sceneTalkSeen = false;
			}

			const auto  here = player->GetPosition();
			const float toA = Distance(here, a->GetPosition());
			const float toB = b.get() == player ? toA : Distance(here, b->GetPosition());
			if (std::min(toA, toB) > std::max(sceneRange * 2.0f, 1200.0f)) {
				a_why = "they walked away"sv;
				return true;
			}

			// Silence is measured from when the last line actually stopped. A line counts
			// as running while its estimated length hasn't elapsed, or while either person
			// has a voice playing or a subtitle up; the sound and subtitle are only
			// trusted for a while past the estimate, so a stuck handle can't hold the
			// camera forever. After that, kSceneGapSeconds of silence ends it.
			//
			// If the scene has handed the player their turn, its repeats aren't filmed
			// (see SceneLine), the voice is only trusted briefly past the last real line,
			// and a short beat of quiet is enough.
			const auto  ahead = playing ? FilmedSceneAhead() : Dialogue::Ahead::kScripted;
			const bool  handOff = playing && SceneHandingOff(ahead);
			const float sinceLine = SecondsSince(sceneLineAt);
			const bool  heard = SoundPlaying(a.get()) || SoundPlaying(b.get()) ||
				Dialogue::SceneWatch::Talking(a.get()) || Dialogue::SceneWatch::Talking(b.get());
			const float believed = handOff ? sceneLineSeconds + 2.0f : sceneLineSeconds * 2.0f + 4.0f;
			const bool  running = sinceLine < sceneLineSeconds || (heard && sinceLine < believed);
			// A scene that's still playing gets a longer gap, since it has walks and
			// pauses between lines.
			const float gap = handOff ? kHandOffSeconds : (playing ? kSceneLongGapSeconds : kSceneGapSeconds);
			if (running) {
				sceneQuietSince = Clock::now();
			} else if (SecondsSince(sceneQuietSince) >= gap) {
				a_why = handOff ? (ahead == Dialogue::Ahead::kMarkingTime ? "the scene is waiting on you"sv :
				                                                           "the scene has nothing more to say"sv) :
				        playing ? "the scene went quiet"sv :
				                  (sceneForm != 0 ? "the scene ended"sv : "the conversation ended"sv);
				return true;
			}
			return false;
		}

		// A line in the filmed scene. Like ApplyCue: cadence, the short-line rule and
		// reactions work as in the player's conversation, with a change of speaker as
		// the turn.
		void ApplySceneCue(bool a_fromA, std::uint32_t a_words, std::uint16_t a_intensity)
		{
			const bool turn = a_fromA != sceneSpeakerIsA;
			const auto now = Clock::now();

			sceneSpeakerIsA = a_fromA;
			npcSpeaking = a_fromA;
			wasSpeaking = a_fromA;
			pendingSpeaking = a_fromA;
			pendingSince = now;
			sceneLineAt = now;
			// Minimum length for the line (voiced lines run about 3.5 words per second).
			// The sound and subtitle say when it actually ends.
			sceneLineSeconds = a_words > 0 ? std::clamp(static_cast<float>(a_words) / 3.5f + 0.3f, 0.8f, 15.0f) : 2.0f;
			sceneQuietSince = now;
			lastCueWords = a_words;

			const bool worthCutting =
				!holdOnShortLines ||
				a_words == 0 ||
				a_words >= shortLineWords ||
				a_intensity >= kCloseUpIntensity;

			if (turn) {
				++turnSerial;
				if (worthCutting) {
					turnSinceCut = true;
				}
				replyStartedAt = now;
				turnBeganAt = now;
				ResetReplyCounters();
			}

			UpdateReaction(worthCutting, a_intensity);
			cueIntensity = a_intensity;
			if (worthCutting) {
				cueSinceCut = true;
			}
			++linesThisTurn;
			if (worthCutting) {
				++linesSinceCut;
			}
			rngState ^= ++cueCount * 0x85EBCA6Bu;
		}

		// Can this NPC join the filmed scene as its second half?
		[[nodiscard]] bool CanJoin(RE::Actor* a_actor, RE::Actor* a_first)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			return a_actor && a_first && a_actor != a_first && a_actor != static_cast<RE::Actor*>(player) &&
			       a_actor->Is3DLoaded() && !a_actor->IsDead() && !a_actor->IsInCombat() &&
			       Distance(a_actor->GetPosition(), a_first->GetPosition()) <= kJoinReach;
		}

		// A scene staged on its first line has nobody for the other half of the frame,
		// so the player stands in. The first reply from someone near the speaker takes
		// their place, and the camera cuts to them.
		void Recast(RE::Actor* a_other)
		{
			auto first = subject.get();
			counterpart = a_other->GetHandle();
			sceneKey = Dialogue::PairKey(first ? first->GetFormID() : 0, a_other->GetFormID());

			playerBody = {};
			playerAnchor = {};
			playerFaceTrack = {};
			posturePrimed = false;
			lastPlayerPose = {};
			frameSubjects.reset();
			protectedPose = {};
			protectedSearch = {};

			// New pair, new eyeline.
			haveShot = false;
			sideCommitted = false;
			heldSince = {};
			forcedCut = true;

			const char* name = a_other->GetName();
			Log::Info(Log::Category::kCamera, "Filming: {} joined in; filming the two of them."sv,
				(name && *name) ? name : "<unnamed>");
		}

		// One line of the filmed scene. The same line can arrive from both the
		// dialogue hook and the subtitle list a frame apart, so a repeat from the same
		// speaker is dropped.
		struct SceneHeard
		{
			RE::FormID        id{ 0 };
			std::string       text{};
			Clock::time_point at{};
		};
		SceneHeard lastSceneLine{};

		void SceneLine(RE::Actor* a_speaker, std::uint32_t a_words, std::uint16_t a_intensity, std::string_view a_text)
		{
			if (!a_speaker) {
				return;
			}
			const auto id = a_speaker->GetFormID();
			if (lastSceneLine.id == id && lastSceneLine.text == a_text && SecondsSince(lastSceneLine.at) < 5.0f) {
				return;
			}
			lastSceneLine = { id, std::string{ a_text }, Clock::now() };

			auto first = subject.get();
			auto second = counterpart.get();
			bool fromFirst = first && first.get() == a_speaker;
			bool fromSecond = second && second.get() == a_speaker;

			// The game moved on to the next scene with the same people. Long sequences are
			// chains of scenes, so follow along instead of letting go in between. A voice
			// from a new scene nearby also takes over once the filmed one stops.
			if (auto* scene = SceneOf(a_speaker); scene && scene->GetFormID() != sceneForm) {
				const bool nearby = (first && Distance(first->GetPosition(), a_speaker->GetPosition()) <= kAddresseeReach) ||
					(second && Distance(second->GetPosition(), a_speaker->GetPosition()) <= kAddresseeReach);
				if (fromFirst || fromSecond || (sceneForm != 0 && !FilmedScenePlaying() && nearby)) {
					sceneForm = scene->GetFormID();
					sceneKey = KeyFor(a_speaker, sceneKey);
					Log::Info(Log::Category::kContinuity, "Scene: the game moved on to its next scene ({:08X}); following it."sv,
						sceneForm);
				}
			}

			// The scene has handed the player their turn, so anything said now is filler
			// (repeated nags). Not filmed and not counted as the scene still talking;
			// SceneOver lets go once the last real line ends.
			if (sceneForm != 0 && (fromFirst || fromSecond || InFilmedScene(a_speaker)) &&
				SceneHandingOff(FilmedSceneAhead())) {
				Log::Info(Log::Category::kContinuity, "Scene: {} repeating while the game waits on you; not filmed."sv,
					NameOf(a_speaker));
				return;
			}

			// In a game scene anyone may speak, and the frame goes to them and whoever
			// they're talking to.
			if (sceneForm != 0 && Present(a_speaker) && (fromFirst || fromSecond || InFilmedScene(a_speaker))) {
				// A one- or two-word bark from someone outside the frame ("Halt!", "Archers!")
				// keeps the scene alive but doesn't move the camera, unless it's delivered at
				// full intensity.
				if (!fromFirst && !fromSecond && a_words > 0 && a_words < 3 && a_intensity < kCloseUpIntensity) {
					sceneLineAt = Clock::now();
					sceneQuietSince = sceneLineAt;
					Log::Info(Log::Category::kContinuity, "Scene: {} called out; holding the frame."sv,
						NameOf(a_speaker));
					return;
				}

				auto listener = Addressee(a_speaker);
				ApplySceneCue(Pair(a_speaker, listener.get()), a_words, a_intensity);
				return;
			}

			if (!fromFirst && !fromSecond) {
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (second && second.get() == static_cast<RE::Actor*>(player) && CanJoin(a_speaker, first.get())) {
					Recast(a_speaker);
					fromSecond = true;
				} else {
					const char* who = a_speaker->GetName();
					Log::Info(Log::Category::kDialogue,
						"Ignored a line from {} — not one of the two being filmed."sv,
						(who && *who) ? who : "<unnamed>");
					return;
				}
			}

			ApplySceneCue(fromFirst, a_words, a_intensity);
		}

		// Which menu the bars go under: the one whose subtitle is in them.
		[[nodiscard]] Render::Letterbox::Beneath BarsBeneath()
		{
			using Beneath = Render::Letterbox::Beneath;
			if (!subtitlesInBar) {
				return Beneath::kOff;
			}
			return staging && sceneMode ? Beneath::kHud : Beneath::kDialogue;
		}

		// Who the camera may cut to, kept fully animated (see KeepPosed). The two in
		// frame, plus the rest of the scene's recent voices in scene mode.
		void PublishPosed()
		{
			std::array<RE::Actor*, Scene::KeepPosed::kMaxActors> actors{};
			std::size_t                                          count = 0;
			const auto add = [&](RE::Actor* a_actor) {
				if (!a_actor || count >= actors.size() ||
					std::find(actors.begin(), actors.begin() + static_cast<std::ptrdiff_t>(count), a_actor) !=
						actors.begin() + static_cast<std::ptrdiff_t>(count)) {
					return;
				}
				actors[count++] = a_actor;
			};

			add(subject.get().get());
			add(OtherParty().get());
			if (sceneMode && sceneForm != 0) {
				for (const auto& handle : Dialogue::SceneWatch::RecentSpeakers(30.0f)) {
					if (auto actor = handle.get(); actor && InFilmedScene(actor.get())) {
						add(actor.get());
					}
				}
			}
			Scene::KeepPosed::Set(std::span<RE::Actor* const>{ actors.data(), count });
		}

		// Subtitles in the black bar, every staged frame.
		void PlaceSubtitles(float a_delta)
		{
			Render::Letterbox::SetBeneath(BarsBeneath());
			if (!subtitlesInBar) {
				Scene::Subtitles::Release();
				return;
			}
			// Place against the bar's target height, not the height currently drawn;
			// otherwise the first line sits over the picture and slides down as the bars
			// ease in.
			Scene::Subtitles::Place(Render::Letterbox::TargetFraction(), sceneMode, std::clamp(a_delta, 0.0f, 0.25f));
		}

		void ApplyCue(RE::Actor* a_speaker, std::uint32_t a_words, std::uint16_t a_intensity,
			std::uint32_t a_emotion, std::string_view a_text)
		{
			// A cue settles the speaking debounce directly. It isn't a turn change,
			// though: one topic info can hold several responses back to back, and treating
			// each as a turn would drop the cut floor to the turn floor for the whole
			// reply.
			const bool alreadySpeaking = npcSpeaking;
			const bool alreadyCoveredReply = playerVoiceHandoff.Active() &&
				framing == Framing::kAuto && !IsNeutral(currentShot) && FavoursNpc(currentShot);

			// The NPC answering retires the player's voice handle. Some voices never
			// release theirs, which kept the topic list hidden. See VoicePlaying().
			retiredVoiceID = voiceHandleID;

			npcSpeaking = true;
			wasSpeaking = true;
			pendingSpeaking = true;
			pendingSince = Clock::now() - std::chrono::milliseconds(500);

			// Is there enough in this line to be worth a new setup?
			//
			// A zero count means unmeasured (no subtitle), not short, so it counts.
			// Intensity overrides length: "No!" at full force is short but deserves the
			// cut, and Choose() gives those lines the tightest setup. Most lines sit at
			// the default intensity of 50, so this only admits lines the writer marked.
			const bool worthCutting =
				!holdOnShortLines ||
				a_words == 0 ||
				a_words >= shortLineWords ||
				a_intensity >= kCloseUpIntensity;

			// Suppressed on the turn edge too, otherwise short one-line replies (the most
			// common short lines) would still cut. This doesn't affect keeping the camera
			// on the speaker; wrongSubject in the tick handles that.
			if (!worthCutting) {
				Log::Info(Log::Category::kContinuity,
					"Line of {} word(s) is under the {}-word floor; holding {}."sv,
					a_words, shortLineWords, Name(currentShot));
			}

			if (!alreadySpeaking) {
				// The early handoff already covered this turn. A later cadence cut keeps the
				// normal shot floor.
				if (worthCutting && !alreadyCoveredReply) {
					turnSinceCut = true;
				}
				replyStartedAt = Clock::now();
				turnBeganAt = replyStartedAt;
				linesThisTurn = 0;
			}

			if (replyBoundary.OnLine(alreadySpeaking)) {
				ResetReplyCounters();
			}
			// Every line, short ones included, ends a reaction. The answer to a speech
			// check is never one: the beat holds on them.
			UpdateReaction(worthCutting && !persuasion.Active(), a_intensity);

			cueIntensity = a_intensity;
			lastCueWords = a_words;
			Scene::Performance::OnLine(a_speaker, a_emotion, a_intensity, a_text);

			if (worthCutting) {
				cueSinceCut = true;
			}

			// Only eligible lines count toward the cadence, so with Ignore Short Lines on
			// a run of "Yes." and "Hmm." doesn't move the camera toward its next angle.
			// linesThisTurn still counts everything.
			++linesThisTurn;
			if (worthCutting) {
				++linesSinceCut;
			}
			rngState ^= ++cueCount * 0x85EBCA6Bu;
		}

		[[nodiscard]] std::optional<ShotType> Choose()
		{

			// The answer to a speech check gets their close-up. Checked before intensity
			// because the beat holds for the whole reply and pushes in.
			if (persuasion.CutOwed() && npcSpeaking && !sceneMode && framing == Framing::kAuto) {
				if (const auto shot = BeatShot()) {
					return shot;
				}
			}

			// In scene mode the second NPC's side of the table is the player's side, so
			// their raised lines get the player close-ups. The override below only looks
			// at the first NPC.
			if (sceneMode && !npcSpeaking && cueIntensity >= kCloseUpIntensity &&
				currentShot != ShotType::kClosePlayer && currentShot != ShotType::kExtremeClosePlayer) {
				const bool extreme = NextRandom() % 3u == 0u;
				if (extreme && Drawable(ShotType::kExtremeClosePlayer)) {
					return ShotType::kExtremeClosePlayer;
				}
				if (Drawable(ShotType::kClosePlayer)) {
					return ShotType::kClosePlayer;
				}
				if (Drawable(ShotType::kExtremeClosePlayer)) {
					return ShotType::kExtremeClosePlayer;
				}
			}

			// A raised intensity (only a few percent of lines) is worth overriding the
			// rotation for.
			if (npcSpeaking && cueIntensity >= kCloseUpIntensity &&
				currentShot != ShotType::kCloseUp && currentShot != ShotType::kExtremeClose) {
				// The extreme close-up gets a third of those, so it stays rare. Both are
				// checked against the enable flags; if both are off this falls through to
				// normal coverage.
				const bool extreme = NextRandom() % 3u == 0u;
				if (extreme && Drawable(ShotType::kExtremeClose)) {
					return ShotType::kExtremeClose;
				}
				if (Drawable(ShotType::kCloseUp)) {
					return ShotType::kCloseUp;
				}
				if (Drawable(ShotType::kExtremeClose)) {
					return ShotType::kExtremeClose;
				}
			}

			// A turn change, a new line or a stale shot all mean the same thing: find a
			// different angle on whoever is talking.
			return Coverage();
		}

		// One Solve per shot type per cut. A cut tries up to four weighted draws and
		// then a ladder over the same set, so types repeat, and each Solve is a set of
		// raycasts. Safe because `subjects` doesn't change during one cut decision;
		// the cache is local to it.
		struct ScoreCache
		{
			std::array<float, static_cast<std::size_t>(ShotType::kCount)> quality{};
			std::array<bool, static_cast<std::size_t>(ShotType::kCount)>  known{};
		};

		// How well this setup places here, or -1 if it can't be placed. Negative so
		// "can't place" always loses to "placed badly".
		[[nodiscard]] float Placement(ShotType a_type, const Subjects& a_subjects, ScoreCache& a_cache)
		{
			const auto index = static_cast<std::size_t>(a_type);
			if (!Drawable(a_type) || index >= a_cache.known.size()) {
				return -1.0f;
			}

			if (!a_cache.known[index]) {
				const auto pose = Solve(a_type, a_subjects);
				a_cache.quality[index] = pose.valid ? pose.quality : -1.0f;
				a_cache.known[index] = true;
			}

			return a_cache.quality[index];
		}

		// Good enough to stop looking. All four draws are compared so a shot jammed
		// against a wall doesn't win just by placing first, but a draw that got about
		// what it asked for ends the search right away.
		constexpr float kGoodEnough = 0.82f;

		// Is this actor audibly speaking right now? Read from the actor's own sound
		// handles rather than MenuTopicManager, because under a player-voice mod the
		// topic isn't committed until the player's line has finished.
		//
		// BSSoundHandle::state is an assumed state and some voices never update it, so
		// a handle can sit in kPlaying long after the line ends. Two safeguards: once
		// the NPC answers, the handle is retired and never counts as speech again; and
		// anything older than kMaxVoiceSeconds is ignored. One set of fields, because
		// the only caller asks about the player.
		//
		// The ceiling only has to catch a line nobody replies to, so it sits well past
		// any real line (long player lines run past six seconds).
		constexpr float kMaxVoiceSeconds = 20.0f;

		[[nodiscard]] bool VoicePlaying(RE::Actor* a_actor)
		{
			// DBReV answers this directly, for the conversation window rather than just
			// the audio: from the start of the line until the dialogue advances, post-line
			// delay included. LipSync uses the audio window instead (see
			// Compat::DBReV::Line). The handle scan below is the DBVO path.
			//
			// EverSpoke rather than Present: DBReV being loaded doesn't mean it's voicing
			// this player.
			if (Compat::DBReV::Present() && Compat::DBReV::EverSpoke()) {
				return Compat::DBReV::Speaking();
			}

			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			auto* high = process ? process->high : nullptr;
			if (!high) {
				return false;
			}

			std::uint32_t playing = RE::BSSoundHandle::kInvalidID;
			for (auto& handle : high->soundHandles) {
				if (handle.soundID != RE::BSSoundHandle::kInvalidID &&
					handle.state.get() == RE::BSSoundHandle::AssumedState::kPlaying) {
					playing = handle.soundID;
					break;
				}
			}

			if (playing == RE::BSSoundHandle::kInvalidID) {
				voiceHandleID = RE::BSSoundHandle::kInvalidID;
				return false;
			}

			// Already answered: the NPC replying means the line finished.
			if (playing == retiredVoiceID) {
				return false;
			}

			// A different ID is a new line.
			if (playing != voiceHandleID) {
				voiceHandleID = playing;
				voiceHandleSince = Clock::now();
				return true;
			}

			return SecondsSince(voiceHandleSince) < kMaxVoiceSeconds;
		}

		// The rendered aspect ratio. Skyrim's field of view is horizontal, so framing
		// by height needs this. Read from the swap chain because ultrawide displays
		// change it a lot.
		[[nodiscard]] float ScreenAspect()
		{
			auto* manager = RE::BSGraphics::Renderer::GetSingleton();
			if (!manager) {
				return 1.78f;
			}

			auto* swapChain = reinterpret_cast<IDXGISwapChain*>(manager->GetRuntimeData().renderWindows[0].swapChain);
			if (!swapChain) {
				return 1.78f;
			}

			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(swapChain->GetDesc(&desc)) || desc.BufferDesc.Height == 0) {
				return 1.78f;
			}

			return std::clamp(
				static_cast<float>(desc.BufferDesc.Width) / static_cast<float>(desc.BufferDesc.Height),
				1.0f, 3.0f);
		}

		// Which side of the eyeline to stage from. The side with more room wins, which
		// also keeps the camera out of walls; near-equal room is a coin flip so
		// conversations vary. (The camera's starting position can't decide it: at the
		// start of a conversation it sits almost exactly on the eyeline.)
		void ChooseSide(const RE::NiPoint3& a_player, const RE::NiPoint3& a_npc)
		{
			bool       ok = false;
			const auto axis = Normalized({ a_npc.x - a_player.x, a_npc.y - a_player.y, a_npc.z - a_player.z }, ok);
			if (!ok) {
				side = 1.0f;
				return;
			}

			const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(axis, worldUp), ok);
			if (!ok) {
				side = 1.0f;
				return;
			}

			const RE::NiPoint3 midpoint{
				(a_player.x + a_npc.x) * 0.5f,
				(a_player.y + a_npc.y) * 0.5f,
				(a_player.z + a_npc.z) * 0.5f
			};
			const RE::NiPoint3 leftward{ -right.x, -right.y, -right.z };

			constexpr float kProbe = 260.0f;
			const float     roomRight = Clearance(midpoint, right, kProbe);
			const float     roomLeft = Clearance(midpoint, leftward, kProbe);

			// Re-deciding (after someone sits or stands) needs a much bigger margin than
			// the first decision. Crossing the eyeline makes the two people appear to swap
			// places, so it should only happen for a clear win, and never on a coin toss.
			constexpr float kMeaningful = 40.0f;
			constexpr float kToFlip = 190.0f;

			const float wasSide = side;
			const float margin = sideCommitted ? kToFlip : kMeaningful;

			if (roomRight > roomLeft + margin) {
				side = 1.0f;
			} else if (roomLeft > roomRight + margin) {
				side = -1.0f;
			} else if (!sideCommitted) {
				side = (NextRandom() & 1u) ? 1.0f : -1.0f;
			}
			// Otherwise keep the side already on screen.

			// A held side isn't worth logging; scenes re-stage on every line.
			if (!sideCommitted || side != wasSide) {
				Log::Info(Log::Category::kStaging,
					"Side {}: {} (room left {:.0f}u, right {:.0f}u)."sv,
					!sideCommitted ? "chosen"sv : "FLIPPED"sv,
					side < 0.0f ? "left"sv : "right"sv, roomLeft, roomRight);
			}

			sideCommitted = true;
		}

		// Apply the latest ini/menu interface settings. Runs every frame but only does
		// work after a change. Runs before the staging check in Tick because Open()
		// calls Suppress() right away, which reads hideSpeakerName.
		void SyncInterfaceSettings()
		{
			if (!interfaceDirty) {
				return;
			}
			interfaceDirty = false;

			// Puts the name back immediately when the option is switched off mid
			// conversation.
			Scene::Interface::SetHideSpeakerName(wantHideSpeakerName);
		}

		// Screen furniture, driven from the always-on tick (the camera hook doesn't
		// run outside third person).
		//
		// The topic list follows the dialogue menu's own state machine. eMenuState
		// goes greeting -> topicList -> topicClicked -> transitioning -> topicList,
		// and topicList means the options are live and up to date. Reading it directly
		// avoids stale rows, flashes during the greeting and gaps under player-voice
		// mods, without guessing at whose turn it is. The four timing settings are
		// just delays and durations applied to that target.
		void DriveInterface()
		{

			// Re-assert the HUD hide every frame. The engine and HUD mods rewrite _visible
			// on their own schedule, and menus that open mid-conversation weren't there
			// when the first hide ran. Suppress() is idempotent and can fail if the HUD
			// movie isn't available yet, so calling it here also retries.
			Scene::Interface::Suppress();
			Scene::Interface::Enforce();

			// A scene has no topic list.
			if (sceneMode) {
				return;
			}

			// Goes through SetChoiceAlpha even when the fade is off, because the
			// speaker-name hide is applied there.
			const auto moviePhase = Scene::Interface::ReadDialoguePhase();

			if (moviePhase.phase != lastMoviePhase) {
				lastMoviePhase = moviePhase.phase;
				Log::Info(Log::Category::kStaging,
					"Menu phase: {}"sv, Scene::Interface::Name(moviePhase.phase));
			}

			// Fail open: if the menu doesn't publish eMenuState (a replacer movie), the
			// list stays at full opacity.
			const bool listLive = moviePhase.TopicsLive();

			// The list isn't hidden until the menu has reported it live once (see
			// listWasLive). An unreadable phase leaves the list alone but doesn't count as
			// the greeting having ended; only a known topic-list state arms the fade.
			if (moviePhase.CanArmFade()) {
				listWasLive = true;
			}

			const bool fading = fadeTopicList && listWasLive;

			// The target and when it last changed. Everything after this eases between two
			// values.
			const bool wantList = !fading || listLive;
			if (wantList != listWanted) {
				listWanted = wantList;
				listEdgeAt = Clock::now();
				choiceEaseFrom = choiceAlpha;
				voiceFadeTimed = false;
				voiceServedDelay = false;
			}

			// Fade After PC Line: the menu takes the list down at the click, but the
			// player's voiced line is still playing. Hold what's on screen and have it
			// gone by the time the line ends (timed to the line when its length is known).
			// The clock is pinned rather than the target forced, so the menu phase still
			// decides the target.
			//
			// The end of the line comes from DBReV's line events when ReVoiced is voicing
			// the player (a skipped line releases immediately), otherwise from the
			// player's sound handles (see VoicePlaying). Both are bounded by
			// kMaxPlayerLineHold from the start of the hold, so a missing end signal can't
			// leave the list stuck on screen.
			const bool wantHold = !wantList && fadeAfterPlayerLine;
			bool       holdForVoice = false;

			if (wantHold && VoicePlaying(RE::PlayerCharacter::GetSingleton())) {
				if (!playerLineHeld) {
					playerLineHeld = true;
					playerLineHeldSince = Clock::now();
				}

				holdForVoice = SecondsSince(playerLineHeldSince) < kMaxPlayerLineHold;

				if (!holdForVoice && playerLineHoldExpired.Take()) {
					Log::Warn(Log::Category::kStaging,
						"Your line has been reported as playing for {:.0f}s with no end; "
						"releasing the topic list rather than holding it any longer."sv,
						kMaxPlayerLineHold);
				}
			} else {
				// Released when the voice stops and whenever the list is wanted back. Both
				// reset the clock, so the next line starts its own hold and a skipped line
				// behaves like a finished one.
				playerLineHeld = false;
			}

			// Fade timed to end with the line, when the audio length is known (from DBReV
			// or the .fuz). Otherwise hold to the end and fade from there without another
			// delay.
			if (holdForVoice) {
				const auto  serial = Scene::LipSync::PlayerLineSerial();
				const float length = Scene::LipSync::PlayerLineDuration();
				if (serial != voiceFadeSerial) {
					voiceFadeSerial = serial;
					voiceFadeTimed = false;
				}
				if (Scene::LipSync::PlayerSpeaking() && length > 0.0f) {
					const float span = std::max(choiceFadeOut, 0.01f);
					const float remaining = std::max(length - Scene::LipSync::PlayerLineElapsed(), 0.0f);
					if (remaining < span) {
						if (!voiceFadeTimed) {
							voiceFadeTimed = true;
							voiceFadeFrom = choiceAlpha;
						}
						choiceAlpha = std::min(choiceAlpha, voiceFadeFrom * (remaining / span));
					}
				} else if (voiceFadeTimed) {
					// The audio has ended but the voice mod's post-line delay hasn't; the fade
					// already finished.
					choiceAlpha = 0.0f;
				}
			}

			if (holdForVoice) {
				// Re-based every held frame. ReVoiced reports a line about 0.25 s after the
				// click, so if the fade had already started, resume from what's on screen
				// rather than jumping back to full.
				listEdgeAt = Clock::now();
				choiceEaseFrom = choiceAlpha;
				voiceServedDelay = true;
			} else {
				// Delay (iListReturnDelay / iChoiceFadeDelay), then ease (kChoiceFadeSeconds /
				// iChoiceFadeTime). Coming in can ease slowly; going out should be quick.
				const float delay = wantList ? listReturnDelay :
					(voiceServedDelay ? 0.0f : choiceFadeDelay);
				const float span = std::max(wantList ? kChoiceFadeSeconds : choiceFadeOut, 0.01f);
				const float since = SecondsSince(listEdgeAt) - delay;

				if (since > 0.0f) {
					const float t = std::clamp(since / span, 0.0f, 1.0f);
					const float target = wantList ? 100.0f : 0.0f;
					choiceAlpha = choiceEaseFrom + (target - choiceEaseFrom) * t;
				}
			}

			// No staleness timer here on purpose: a stuck menu and a long monologue look
			// the same to a timer. If the list ever gets stuck, the phase log shows which
			// phase it stopped in.
			Scene::Interface::SetChoiceAlpha(choiceAlpha);
		}

		// Dialogue and anatomy updates continue while the camera is in first person.
		[[nodiscard]] std::optional<Subjects> SampleSubjects(float a_delta)
		{
			// "player" below is the other half of every shot: the player, or the second
			// NPC in scene mode. Only the size reference further down needs the real
			// player.
			auto otherPtr = OtherParty();
			auto* player = otherPtr.get();
			auto speakerPtr = subject.get();
			if (!player || !speakerPtr) {
				return std::nullopt;
			}
			const auto npcNow = PostureOf(speakerPtr.get());
			const auto playerNow = PostureOf(player);
			bool       postureChanged = false;
			if (!posturePrimed) {
				posturePrimed = true;
			} else if (npcNow != npcPosture || playerNow != playerPosture) {
				postureChanged = true;
				Log::Info(Log::Category::kStaging, "Posture changed ({} -> {}); re-measuring and re-staging."sv,
					PostureName(npcNow == npcPosture ? playerPosture : npcPosture),
					PostureName(npcNow == npcPosture ? playerNow : npcNow));
			}
			npcPosture = npcNow;
			playerPosture = playerNow;

			if (postureChanged) {
				protectedPose = {};
				protectedSearch = {};
				visibilityFailedObservation = false;
				// Snap rather than ease: easing from a bed up to a standing head would drag
				// the camera through the mattress. Standing up is a cut, not a move.
				playerAnchor.primed = false;
				npcAnchor.primed = false;
				// Same for the face offset, which was taken off the old posture.
				playerFaceTrack.primed = false;
				npcFaceTrack.primed = false;

				// Re-run ChooseSide and the clearance probe; the room around someone lying in
				// an alcove isn't the room around them standing beside it.
				haveShot = false;

				// The held values are relative to things that are about to change (the side
				// and the open direction), so reapplying them after a re-probe could throw the
				// camera across the room. This is treated as a cut, so the shot picks its
				// angle again.
				heldSweep = kUnheld;
				heldStandoff = kUnheld;
				heldRoom = kUnheld;
				heldFaceYaw = kUnheld;
				heldFacePitch = kUnheld;

				// The held face offset in a scene was taken off the old posture too.
				faceOffsetShot = {};
			}

			const bool firstSample = !playerAnchor.primed;

			// Measured when the anchors are primed and again on a posture change.
			if (firstSample || !npcBody.measured || !playerBody.measured) {
				npcBody = Measure(speakerPtr.get());
				playerBody = Measure(player);

				// The player is the size reference, so an ordinary conversation comes out at
				// exactly 1.0 and the NPC is sized relative to the player. Still the player in
				// scene mode, so two guards are framed as people and a giant next to a farmer
				// as a giant.
				float reference = playerBody.measured ? playerBody.radius : 0.0f;
				if (sceneMode) {
					if (auto* ruler = RE::PlayerCharacter::GetSingleton()) {
						const auto measured = Measure(ruler);
						if (measured.measured) {
							reference = measured.radius;
						}
					}
				}
				Fit(playerBody, reference);
				Fit(npcBody, reference);
			}

			const auto npcTarget = StablePoint(speakerPtr.get(), npcBody);
			const auto playerTarget = StablePoint(player, playerBody);
			if (!npcTarget || !playerTarget) {
				return std::nullopt;
			}

			const float delta = std::clamp(a_delta, 0.0f, 0.1f);

			Follow(playerAnchor, *playerTarget, delta);
			Follow(npcAnchor, *npcTarget, delta);
			TrackFace(npcFaceTrack, speakerPtr.get(), npcBody, *npcTarget, *playerTarget, delta);
			TrackFace(playerFaceTrack, player, playerBody, *playerTarget, *npcTarget, delta);

			// Player faces and eyes; not in scene mode.
			if (!sceneMode) {
				Scene::Performance::Update(delta, npcSpeaking);

				// Every frame: the gaze offset is meaningless if a package or combat check has
				// taken the head-track target back.
				Scene::Presence::Update(delta);
			}

			// Debounced. currentTopicInfo goes null briefly between two responses and
			// after a player-voice line before the NPC's cue lands; read raw, that looks
			// like the turn passing and the camera would cut away and straight back.
			//
			// Scene mode has no topic manager, so who has the floor comes from the cues
			// (ApplySceneCue).
			const bool rawSpeaking = sceneMode ? npcSpeaking : Dialogue::Session::GetSingleton().Speaking();
			if (rawSpeaking != pendingSpeaking) {
				pendingSpeaking = rawSpeaking;
				pendingSince = Clock::now();
			}

			const bool settled = SecondsSince(pendingSince) >= kTurnDebounceSeconds;
			if (settled && pendingSpeaking != wasSpeaking) {
				npcSpeaking = pendingSpeaking;
				wasSpeaking = npcSpeaking;

				// The one place a turn change is counted. Anything that expires after "one
				// exchange" uses turnSerial.
				++turnSerial;

				// Only the start of speech marks a turn (so a cut here can use the shorter
				// floor). The cut itself still needs the line count, a timer or coverage to
				// ask for it. OnCue normally sets this first; this branch covers a line that
				// starts without a cue.
				if (npcSpeaking) {
					turnSinceCut = true;
				} else {
					// A pending line cue expires with its line. Otherwise a line shorter than
					// iMinShotTime would leave cueSinceCut set and the cut would fire a second or
					// two after the NPC stopped talking.
					cueSinceCut = false;

					// The reply is over; drop any reaction state and the persuasion beat with it.
					reactionShots.Reset();
					EndBeat("their answer finished"sv);

					Log::Info(Log::Category::kContinuity,
						"Line ended; holding {} through the pause."sv,
						Name(currentShot));
				}

				turnBeganAt = Clock::now();
				if (npcSpeaking) {
					replyStartedAt = turnBeganAt;
				}
			}

			if (!haveShot) {
				ChooseSide(playerAnchor.position, npcAnchor.position);
				ProbeOpenDirection(playerAnchor.position, npcAnchor.position);
				screenAspect = ScreenAspect();
				// Logged only when it changes.
				static float aspectReported = 0.0f;
				if (std::abs(screenAspect - aspectReported) > 0.005f) {
					aspectReported = screenAspect;
					Log::Info(Log::Category::kStaging, "Framing against aspect {:.2f}."sv, screenAspect);
				}
				haveShot = true;
			}

			Subjects subjects{};
			subjects.playerHead = playerAnchor.position;
			subjects.npcHead = npcAnchor.position;
			// Compose against the captured base lens, never a live read. SD writes
			// worldFOV every frame, so reading it back would feed each frame's narrower
			// lens into the next solve and the shot would creep tighter. Shots with their
			// own lens supply it; this is the fallback.
			subjects.fovDegrees = baseFov > 1.0f ? baseFov : 75.0f;
			subjects.aspect = screenAspect;
			subjects.side = side;
			subjects.openDirection = openDirection;
			subjects.openDistance = openDistance;
			subjects.npc = npcBody;
			subjects.player = playerBody;
			subjects.npcFace = FaceFor(npcFaceTrack);
			subjects.playerFace = FaceFor(playerFaceTrack);
			subjects.followFace = followFace;
			subjects.ceiling = ceilingRoom;
			subjects.enforceLine = enforceLine || true180;  // true180 implies enforceLine
			subjects.true180 = true180;
			subjects.avoidCrowds = avoidCrowds;

			// Two NPCs don't necessarily face each other. See Subjects::avoidBackOfHead.
			subjects.avoidBackOfHead = sceneMode;
			subjects.npcForward = Heading(speakerPtr.get());
			subjects.playerForward = Heading(player);

			// The two participants, so the crowd probe can tell them from bystanders.
			// Everyone else counts, followers included.
			subjects.npcId = speakerPtr->GetFormID();
			subjects.playerId = player->GetFormID();
			subjects.progress = 0.0f;  // candidates are judged at the moment they open
			subjects.delta = delta;
			subjects.protectSubject = protectSubject;
			subjects.cropFractionPerEdge = letterboxWanted ?
				static_cast<float>(std::clamp(tunables.letterboxHeight, 0, 300)) / 1000.0f : 0.0f;
			if (protectSubject) {
				subjects.npcSight = MeasureSightTarget(speakerPtr.get(), npcAnchor.position, npcBody.scale);
				subjects.playerSight = MeasureSightTarget(player, playerAnchor.position, playerBody.scale);
			}
			return subjects;
		}

		[[nodiscard]] double VisibilityTime()
		{
			return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
		}

		[[nodiscard]] float SeparationSquared(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const auto d = a - b;
			return d.x * d.x + d.y * d.y + d.z * d.z;
		}

		// Placement recovery uses the same named setups and settings as normal
		// selection; it can't invent an unlisted fallback shot.
		[[nodiscard]] std::optional<std::pair<ShotType, Pose>> EnabledFallback(Subjects subjects)
		{
			subjects.heldSweep = kUnheld;
			subjects.heldStandoff = kUnheld;
			subjects.heldRoom = kUnheld;
			subjects.heldFaceYaw = kUnheld;
			subjects.heldFacePitch = kUnheld;
			subjects.holdPlacement = false;
			subjects.progress = 0.0f;
			std::array<Pose, kAllShots.size()> poses{};
			const auto selected = BestAvailable(std::span{ kAllShots }, Drawable, [&](ShotType type) {
				auto& pose = poses[static_cast<std::size_t>(type)];
				pose = Solve(type, subjects);
				if (!pose.valid) {
					return -1.0f;
				}
				const bool preferred = FramingIsRoom() ? IsNeutral(type) :
					(!IsNeutral(type) && FavoursNpc(type) == SubjectIsNpc());
				return pose.quality + (preferred ? 2.0f : (IsNeutral(type) ? 1.0f : 0.0f));
			});
			if (!selected) {
				return std::nullopt;
			}
			return std::pair{ *selected, poses[static_cast<std::size_t>(*selected)] };
		}

		void UseNativeView()
		{
			if (!nativeView) {
				RestoreFieldOfView();
				RestoreCameraRest();
				nativeView = true;
			}
		}

		[[nodiscard]] bool Visible(const Pose& pose)
		{
			return pose.valid && pose.visibility.state == SightState::kClear &&
				pose.lensClearance == SightState::kClear;
		}

		[[nodiscard]] bool ProtectedSubject(ShotType type)
		{
			if (!Drawable(type)) {
				return false;
			}
			if (FramingIsRoom()) {
				return IsNeutral(type);
			}
			if (IsNeutral(type)) {
				return framing == Framing::kAuto && SubjectIsNpc();
			}
			return FavoursNpc(type) == SubjectIsNpc();
		}

		void StartProtectedSearch(bool recovery)
		{
			protectedSearch = {};
			protectedSearch.active = true;
			protectedSearch.recovery = recovery;
			protectedSearch.turn = turnSerial;
			protectedSearch.framingAtStart = framing;
			std::array<bool, static_cast<std::size_t>(ShotType::kCount)> added{};
			const auto add = [&](ShotType type) {
				const auto i = static_cast<std::size_t>(type);
				if (i < added.size() && !added[i] && ProtectedSubject(type) &&
					(recovery || type != currentShot)) {
					added[i] = true;
					protectedSearch.order[protectedSearch.count++] = type;
				}
			};
			if (recovery) {
				add(currentShot);
			}
			for (int i = 0; i < 4; ++i) {
				if (const auto choice = Choose()) {
					add(*choice);
				}
			}
			// A weighted permutation tries every remaining allowed setup once. Room
			// heuristics can't prove a narrow, face-visible view won't work.
			for (;;) {
				std::uint32_t total = 0;
				for (std::size_t i = 0; i < added.size(); ++i) {
					const auto type = static_cast<ShotType>(i);
					if (!added[i] && ProtectedSubject(type) && (recovery || type != currentShot)) {
						total += static_cast<std::uint32_t>(std::max(Shot::Weight(type), 1));
					}
				}
				if (total == 0) {
					break;
				}
				auto draw = NextRandom() % total;
				for (std::size_t i = 0; i < added.size(); ++i) {
					const auto type = static_cast<ShotType>(i);
					if (added[i] || !ProtectedSubject(type) || (!recovery && type == currentShot)) {
						continue;
					}
					const auto weight = static_cast<std::uint32_t>(std::max(Shot::Weight(type), 1));
					if (draw < weight) {
						add(type);
						break;
					}
					draw -= weight;
				}
			}
		}

		void AttachSight(Subjects& subjects, SightContext& context)
		{
			context = BuildSightContext();
			subjects.sightContext = &context;
			subjects.protectSubject = true;
			subjects.checkVisibility = true;
		}

		// At most two full angle sweeps per update. Candidates carried over from an
		// earlier update are re-checked against the current subjects first.
		[[nodiscard]] std::optional<std::pair<ShotType, Pose>> SearchProtected(Subjects subjects)
		{
			if (!protectedSearch.active) {
				return std::nullopt;
			}
			if (protectedSearch.turn != turnSerial || protectedSearch.framingAtStart != framing) {
				StartProtectedSearch(protectedSearch.recovery);
			}
			subjects.heldSweep = kUnheld;
			subjects.heldStandoff = kUnheld;
			subjects.heldRoom = kUnheld;
			subjects.heldFaceYaw = kUnheld;
			subjects.heldFacePitch = kUnheld;
			subjects.holdPlacement = false;
			subjects.progress = 0.0f;
			subjects.requireFullFace = viewMode == ViewMode::kFirstPersonFallback;
			if (protectedSearch.best.valid) {
				CheckVisibility(protectedSearch.bestType, protectedSearch.best, subjects);
				if (!Visible(protectedSearch.best) || !ProtectedSubject(protectedSearch.bestType)) {
					protectedSearch.best = {};
				}
			}
			for (int budget = 0; budget < 2 && protectedSearch.cursor < protectedSearch.count; ++budget) {
				const auto type = protectedSearch.order[protectedSearch.cursor++];
				if (!ProtectedSubject(type)) {
					continue;
				}
				const auto pose = Solve(type, subjects);
				const bool recoveryClear = viewMode != ViewMode::kFirstPersonFallback || pose.visibility.face >= 0.99f;
				if (Visible(pose) && recoveryClear && (!protectedSearch.best.valid || pose.quality > protectedSearch.best.quality)) {
					protectedSearch.best = pose;
					protectedSearch.bestType = type;
				}
				if (protectedSearch.best.valid && protectedSearch.best.quality >= kGoodEnough) {
					break;
				}
			}
			const bool finished = protectedSearch.cursor >= protectedSearch.count;
			if (protectedSearch.best.valid && (finished || protectedSearch.cursor >= 6 ||
				protectedSearch.best.quality >= kGoodEnough)) {
				protectedSearch.active = false;
				return std::pair{ protectedSearch.bestType, protectedSearch.best };
			}
			if (finished) {
				protectedSearch.active = false;
				protectedRetryAt = Clock::now();
			}
			return std::nullopt;
		}

		bool CommitProtected(ShotType type, const Pose& pose)
		{
			if (!ProtectedSubject(type) || !Visible(pose)) {
				return false;
			}
			Log::Info(Log::Category::kContinuity,
				"Visible shot: {} -> {} (face {:.0f}%, quality {:.2f}, adjustment {:+.1f}deg)."sv,
				Name(currentShot), Name(type), pose.visibility.face * 100.0f, pose.quality, pose.sweep);
			previousShot = currentShot;
			currentShot = type;
			shotSince = Clock::now();
			heldSince = shotSince;
			heldSweep = pose.sweep;
			heldStandoff = pose.standoff;
			heldRoom = pose.room;
			heldFaceYaw = pose.faceYaw;
			heldFacePitch = pose.facePitch;
			protectedPose = pose;
			protectedShot = type;
			visibilityCheckedAt = {};
			visibilityRecovery.Reset();
			visibilityFailedObservation = false;
			protectedSearch = {};
			cueSinceCut = false;
			turnSinceCut = false;
			forcedCut = false;
			linesSinceCut = 0;
			cutEveryTarget = RollCutEvery();
			return true;
		}

		[[nodiscard]] std::optional<Pose> ProtectedFrame(Subjects subjects, bool ordinaryCut)
		{
			if (viewMode != ViewMode::kCinematic || fallbackRequested) {
				return std::nullopt;
			}
			if (!Drawable(currentShot)) {
				protectedPose = {};
			}
			const bool hadPose = ReusableShot(protectedPose.valid, protectedShot, currentShot, Drawable);
			Pose pose{};
			if (hadPose) {
				subjects.heldSweep = heldSweep;
				subjects.heldStandoff = heldStandoff;
				subjects.heldRoom = heldRoom;
				subjects.heldFaceYaw = heldFaceYaw;
				subjects.heldFacePitch = heldFacePitch;
				subjects.holdPlacement = heldRoom > kUnheld;
				subjects.progress = std::clamp(SecondsSince(shotSince) /
					std::max(static_cast<float>(Shot::MoveTime(currentShot)) / 100.0f, 0.1f), 0.0f, 1.0f);
				subjects.checkVisibility = false;
				pose = Solve(currentShot, subjects);
			}
			const bool moved = SeparationSquared(subjects.npcSight.head, checkedNpc) > 64.0f ||
				SeparationSquared(subjects.playerSight.head, checkedPlayer) > 64.0f ||
				SeparationSquared(pose.position, checkedCamera) > 64.0f || std::abs(pose.lens - checkedLens) > 1.0f;
			const bool check = !hadPose || !pose.valid || moved || SecondsSince(visibilityCheckedAt) >= 0.10f;
			SightContext context{};
			if (check || ordinaryCut || protectedSearch.active) {
				AttachSight(subjects, context);
			}
			bool recovery = !hadPose;
			if (check && hadPose) {
				CheckVisibility(currentShot, pose, subjects);
				visibilityCheckedAt = Clock::now();
				checkedNpc = subjects.npcSight.head;
				checkedPlayer = subjects.playerSight.head;
				checkedCamera = pose.position;
				checkedLens = pose.lens;
				const bool clear = Visible(pose);
				visibilityFailedObservation = !clear;
				recovery = visibilityRecovery.Observe(VisibilityTime(), clear,
					pose.visibility.severe || pose.lensClearance == SightState::kBlocked,
					pose.visibility.state == SightState::kUnknown, hadPose);
				if (clear) {
					protectedPose = pose;
				} else {
					// A short grace period keeps a brief crossing from causing a cut. This is the
					// last verified absolute pose, not an unchecked offset.
					pose = protectedPose;
				}
			} else if (hadPose && pose.valid) {
				// Reuse only the observation; the next check follows the moved lens.
				pose.visibility = protectedPose.visibility;
				pose.lensClearance = protectedPose.lensClearance;
				if (visibilityFailedObservation) {
					pose = protectedPose;
				}
			}
			if (recovery && protectedSearch.active && !protectedSearch.recovery) {
				StartProtectedSearch(true);
			}
			if (!protectedSearch.active && (recovery || ordinaryCut) &&
				(SecondsSince(protectedRetryAt) >= 0.5f || ordinaryCut || !hadPose)) {
				StartProtectedSearch(recovery);
				forcedCut = false;
				cueSinceCut = false;
				turnSinceCut = false;
			}
			if (protectedSearch.active) {
				if (!subjects.sightContext) {
					AttachSight(subjects, context);
				}
				if (const auto candidate = SearchProtected(subjects)) {
					if (CommitProtected(candidate->first, candidate->second)) {
						return candidate->second;
					}
				}
			}
			if (recovery && firstPersonFallback) {
				fallbackRequested = true;
				return std::nullopt;
			}
			return hadPose ? std::optional{ pose.valid ? pose : protectedPose } : std::nullopt;
		}

		void TickProtected(const std::optional<Subjects>& subjects)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || Dialogue::MenuWatch::ScreenTaken()) {
				return;
			}
			if (fallbackRequested) {
				fallbackRequested = false;
				if (protectSubject && firstPersonFallback && camera->IsInThirdPerson() &&
					Compat::SmoothCam::Holding()) {
					RestoreFieldOfView();
					RestoreCameraRest();
					viewMode = ViewMode::kFirstPersonFallback;
					fallbackThirdPersonOwed = !returnToFirstPerson;
					visibilityRecovery.EnterFallback(VisibilityTime());
					fallbackTurn = turnSerial;
					fallbackCue = cueCount;
					StartProtectedSearch(true);
					camera->ForceFirstPerson();
					Compat::SmoothCam::Release();
					Log::Info(Log::Category::kCamera,
						"Subject visibility lost; first-person coverage while dialogue continues."sv);
				}
			}
			if (viewMode != ViewMode::kFirstPersonFallback) {
				return;
			}
			// A state change by the player or engine owns the view; don't force it every
			// tick.
			if (!camera->IsInFirstPerson()) {
				(void)visibilityRecovery.Ready(VisibilityTime(), false, false);
				return;
			}
			if (!protectSubject || !firstPersonFallback) {
				if (Compat::SmoothCam::Acquire()) {
					viewMode = ViewMode::kCinematic;
					fallbackThirdPersonOwed = false;
					camera->ForceThirdPerson();
					protectedPose = {};
					protectedSearch = {};
				}
				return;
			}
			if (!subjects) {
				(void)visibilityRecovery.Ready(VisibilityTime(), false, false);
				return;
			}
			if (SecondsSince(visibilityCheckedAt) < 0.10f) {
				return;
			}
			if (SecondsSince(visibilityCheckedAt) > 0.30f) {
				(void)visibilityRecovery.Ready(VisibilityTime(), false, false);
			}
			visibilityCheckedAt = Clock::now();
			auto fresh = *subjects;
			SightContext context{};
			AttachSight(fresh, context);
			if (!protectedSearch.active && !protectedSearch.best.valid && SecondsSince(protectedRetryAt) >= 0.5f) {
				StartProtectedSearch(true);
			}
			if (protectedSearch.active) {
				(void)SearchProtected(fresh);
			}
			if (protectedSearch.best.valid) {
				CheckVisibility(protectedSearch.bestType, protectedSearch.best, fresh);
				if (!Visible(protectedSearch.best) || protectedSearch.best.visibility.face < 0.99f ||
					!ProtectedSubject(protectedSearch.bestType)) {
					protectedSearch.best = {};
				}
			}
			const bool clear = Visible(protectedSearch.best) && protectedSearch.best.visibility.face >= 0.99f;
			const bool boundary = !Dialogue::Session::GetSingleton().Speaking() || forcedCut ||
				turnSerial != fallbackTurn || cueCount != fallbackCue;
			if (visibilityRecovery.Ready(VisibilityTime(), clear, boundary) && Compat::SmoothCam::Acquire()) {
				const auto selected = protectedSearch.bestType;
				const auto pose = protectedSearch.best;
				if (!CommitProtected(selected, pose)) {
					Compat::SmoothCam::Release();
					return;
				}
				viewMode = ViewMode::kCinematic;
				fallbackThirdPersonOwed = false;
				camera->ForceThirdPerson();
				Log::Info(Log::Category::kCamera, "Subject clear; cinematic coverage resumed."sv);
			}
			fallbackTurn = turnSerial;
			fallbackCue = cueCount;
		}

	}

	void Director::Open(RE::Actor* a_speaker, bool a_restaging, bool a_provisional)
	{
		if (!a_speaker) {
			return;
		}
		const auto openingStarted = Clock::now();
		const Config::ReadScope settings;

		// Is this a resume, or a new conversation that happens to follow one? Decided
		// once per suspension, and the suspension is used up either way. Matched on
		// both the actor and the session serial, since two conversations with the same
		// person share a form ID.
		const auto openSerial = Dialogue::Session::GetSingleton().ConversationSerial();

		// Scenes never resume; they have no menu to come back from.
		const bool resuming = !sceneMode && suspendedForMenu &&
			suspendedPartner == a_speaker->GetFormID() &&
			suspendedSerial == openSerial;

		const ShotType resumeShot = suspendedShot;

		// Taken from the suspension whether or not this is a resume: if a different
		// conversation comes back, the player is still in third person because SD put
		// them there, and the new Open() can't tell.
		const bool suspensionOwesFirstPerson = suspendedForMenu && suspendedFirstPersonOwed;
		const bool resumeFallback = resuming && suspendedInFallback;
		const bool suspensionOwesThirdPerson = suspendedForMenu && suspendedThirdPersonOwed;

		if (suspendedForMenu && !resuming) {
			Log::Info(Log::Category::kCamera,
				"Cinematic resume skipped: [{:08X}] was suspended, but this is a new "
				"conversation with [{:08X}]."sv,
				suspendedPartner, a_speaker->GetFormID());
		}

		// The first-person debt is only spent once the camera is actually acquired.
		// SmoothCam can refuse (Improved Camera asks for the same camera), and
		// clearing it early would leave a first-person player stuck in third. A
		// suspension that belongs to a different conversation is dropped here.
		if (suspendedForMenu && !resuming) {
			suspendedForMenu = false;
			suspendedPartner = 0;
			suspendedSerial = 0;
			suspendedFirstPersonOwed = false;
		}

		// Already staging means the player went straight into a second conversation.
		// Session restarts within a single frame on a partner change, so Runtime never
		// sees it end and calls Open() for the new partner. Close and reopen rather
		// than patching the state in place, so the reset below stays the one
		// definition of a fresh conversation. The first-person debt is carried over,
		// including one recorded by a suspension.
		//
		// The session serial identifies which conversation this is; read once at the
		// top because the resume check needs it too.
		const auto sessionSerial = openSerial;

		bool firstPersonOwed = suspensionOwesFirstPerson;
		if (staging) {
			// A repeat call for the conversation already on screen. Same actor isn't
			// enough: leaving a conversation and walking straight back into one is a new
			// conversation with the same person.
			if (subject.get().get() == a_speaker && sessionSerial == stagedSerial) {
				return;
			}

			// The provisional staging becomes the real one: the live speaker arrived and
			// it's the same person. Adopting it avoids a visible restage.
			if (provisionalStage && !sceneMode && subject.get().get() == a_speaker) {
				provisionalStage = false;
				stagedSerial = sessionSerial;
				Log::Info(Log::Category::kCamera,
					"Conversation {} with {} picked up by the staging already on screen."sv,
					sessionSerial, a_speaker->GetName() ? a_speaker->GetName() : "<unnamed>");
				return;
			}

			// Carried across the close and reopen, otherwise the player would get one
			// frame of first person in between.
			firstPersonOwed = firstPersonOwed || returnToFirstPerson;
			returnToFirstPerson = false;
			Close();
		}

		// 0 for scene mode, so the player's own conversation starting over it never
		// matches the repeat-call check above.
		stagedSerial = sceneMode ? 0 : sessionSerial;

		if (!resumeFallback && !Compat::SmoothCam::Acquire()) {
			// If SmoothCam refuses, the suspension stays armed so the first-person debt
			// isn't lost. Runtime calls AbandonSuspension when the conversation ends,
			// which pays it.
			if (resuming) {
				Log::Warn(Log::Category::kCamera,
					"Resume declined: the camera is held by another plugin. The suspension for "
					"[{:08X}] stays armed so the view is still handed back when this "
					"conversation ends."sv,
					suspendedPartner);
			}
			return;
		}

		const bool keepThirdPerson = suspensionOwesThirdPerson || restoreThirdPersonPending;
		if (!resumeFallback) {
			// This conversation now owns the final restore, so a deferred debt from the
			// previous one must not change its view later.
			restoreThirdPersonPending = false;
			handBackPending = false;
		}

		// The suspension has been honoured; anything the resume needed was copied into
		// locals above.
		suspendedForMenu = false;
		suspendedPartner = 0;
		suspendedSerial = 0;
		suspendedFirstPersonOwed = false;

		subject = a_speaker->GetHandle();
		viewMode = resumeFallback ? ViewMode::kFirstPersonFallback : ViewMode::kCinematic;
		fallbackThirdPersonOwed = resumeFallback && suspensionOwesThirdPerson;
		fallbackRequested = false;
		protectedPose = {};
		protectedSearch = {};
		frameSubjects.reset();
		visibilityCheckedAt = {};
		protectedRetryAt = {};
		visibilityRecovery.Reset();
		visibilityFailedObservation = false;
		suspendedInFallback = false;
		suspendedThirdPersonOwed = false;
		stagingSince = Clock::now();
		shotSince = stagingSince;
		currentShot = ShotType::kTwoShot;
		previousShot = ShotType::kTwoShot;
		haveShot = false;

		// The eyeline belongs to the conversation, so a new one picks its side freely.
		sideCommitted = false;
		ceilingRoom = 0.0f;
		roomSpace = Space::kRoom;

		// Hotkey overrides only last for one conversation. Lasting preferences live in
		// the settings.
		framing = Framing::kAuto;
		forcedCut = false;
		requestCut.store(false, std::memory_order_relaxed);
		requestFraming.store(false, std::memory_order_relaxed);

		// Reset together, or the first override of a new conversation would expire
		// immediately against a stale serial.
		turnSerial = 0;
		framingUntilTurn = 0;
		playerVoiceHandoff.Reset(Scene::LipSync::PlayerLineSerial());
		reactionShots.Reset();
		replyBoundary.Reset();
		lastPickPhase = Scene::Interface::MenuPhase::kUnknown;
		lastPlayerLineSerial = Scene::LipSync::PlayerLineSerial();

		persuasion.End();
		beatInfo = nullptr;
		pickedCheck = SpeechCheck::kNone;
		beatShot = ShotType::kCount;

		npcSpeaking = false;
		wasSpeaking = false;
		cueIntensity = 50;
		lastLookApplied = -1;
		releasePending = false;
		cueSinceCut = false;
		turnSinceCut = false;

		// The hold on the spent topic list is per conversation. A line left open by a
		// menu close, a load or a voice mod that never reported its end mustn't carry
		// over.
		playerLineHeld = false;
		playerLineHeldSince = stagingSince;
		playerLineHoldExpired.Reset();
		voiceFadeTimed = false;
		voiceServedDelay = false;
		voiceFadeSerial = Scene::LipSync::PlayerLineSerial();
		gestureCueSerial = voiceFadeSerial;

		// Otherwise the debounce thinks a turn is already in flight and fires a
		// spurious change on the first frame.
		pendingSpeaking = false;
		pendingSince = stagingSince;
		turnBeganAt = stagingSince;
		linesThisTurn = 0;

		// Per conversation: each new conversation has to see its list go live before
		// anything may hide it.
		listWasLive = false;
		lastMoviePhase = Scene::Interface::MenuPhase::kUnknown;
		listWanted = true;
		listEdgeAt = stagingSince;
		choiceAlpha = 100.0f;
		choiceEaseFrom = 100.0f;

		// Handle IDs get reused, so a handle retired by the last conversation mustn't
		// silence the first line of this one.
		voiceHandleID = RE::BSSoundHandle::kInvalidID;
		retiredVoiceID = RE::BSSoundHandle::kInvalidID;

		lastNpcPose = {};
		lastPlayerPose = {};
		enabledRetryAt = {};
		nativeView = false;

		playerAnchor = {};
		npcAnchor = {};
		playerFaceTrack = {};
		npcFaceTrack = {};

		// Measurements from the previous conversation shouldn't frame the next person
		// (a dragon's sizing applied to a chicken), and a stale posture would look
		// like a change on the first frame.
		playerBody = {};
		npcBody = {};
		posturePrimed = false;

		haveFrameTime = false;
		rngState ^= a_speaker->GetFormID() * 2654435761u;

		// Staging can't work from inside the player's head, so first person has to go
		// for the conversation. Hand it back at the end, but only if Open() is what
		// took it away; another camera mod that forced third person handles its own
		// restore.
		returnToFirstPerson = false;
		if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInFirstPerson()) {
			returnToFirstPerson = !keepThirdPerson && Config::Bool("Direction", "bRestoreFirstPerson", true);
			if (!resumeFallback) {
				camera->ForceThirdPerson();
			}
		}

		// Carried over a partner change: the camera is already in third person because
		// the previous conversation held it there.
		if (firstPersonOwed) {
			returnToFirstPerson = true;
		}

		// The lens this conversation composes against. If no idle frame has been
		// sampled yet, use the camera's value unless it's below the floor (which would
		// be SD's own stuck lens), in which case use 75.
		if (restingFov < 1.0f) {
			auto*       cam = RE::PlayerCamera::GetSingleton();
			const float reading = cam ? cam->GetRuntimeData2().worldFOV : 0.0f;
			if (reading >= kSaneMinFov) {
				restingFov = reading;
			} else {
				restingFov = 75.0f;
				Log::Warn(Log::Category::kCamera,
					"No idle field-of-view sample yet and the camera reads {:.1f}, under the {:.0f} "
					"floor. Using 75 for this conversation."sv,
					reading, kSaneMinFov);
			}
		}
		baseFov = restingFov;

		staging = true;

		// letterboxWanted rather than true, since the bars are a setting.
		Render::Letterbox::SetVisible(letterboxWanted);

		// NPCs at a workbench put the work down a few seconds into talking (see
		// StopWork). Not in scene mode.
		if (!sceneMode) {
			Scene::StopWork::Begin(a_speaker);
		}

		// Head tracking on the player writes a behaviour graph variable and a dialogue
		// head-track target. Off means fully untouched. Presence::Release always runs
		// because it does nothing unless Engage did.
		if (sceneMode) {
			// The player is only watching in scene mode; nothing of theirs is touched.
		} else if (Config::Bool("Performance", "bHeadtracking", true)) {
			Scene::Presence::Engage(a_speaker);
		} else {
			Log::Info(Log::Category::kStaging,
				"Headtracking disabled by ini; the player's animation graph is not touched."sv);
		}
		// bHoldPlayerFace is retired: the player's head keeps whatever draw flag the
		// game gives it.
		Scene::Performance::HoldPlayerFace(false);
		// Configure profiles once, before Engage uses them. A resume keeps its
		// existing settings.
		if (!resuming && !a_restaging) {
			ReadTuning();
		} else {
			Scene::Performance::Configure(Config::Bool("Performance", "bExpressions", true), false);
		}
		// The player's face and mouth. NPC pairs keep the engine's own.
		if (!sceneMode) {
			Scene::Performance::Engage(a_speaker);
			Scene::FaceGen::Begin(a_speaker);
			Scene::LipSync::Engage();
		}

		// After ReadTuning, since Suppress() reads bHideSpeakerName. In scene mode the
		// lines are HUD subtitles, so the HUD hide keeps those.
		Scene::Interface::SetKeepSubtitles(sceneMode);
		Scene::Interface::Suppress();

		// Four lighting settings and a per-angle nudge. See LightRig.h.
		Scene::KeyLight::Configure(
			Config::Bool("Lighting", "bLights", false),
			Config::Int("Lighting", "iBrightness", 100),
			Config::Int("Lighting", "iRed", 255),
			Config::Int("Lighting", "iGreen", 246),
			Config::Int("Lighting", "iBlue", 234),
			Config::Bool("Lighting", "bShadows", false),
			Config::Int("Lighting", "iFadeTime", 18));

		lightPerShot = Config::Bool("Lighting", "bPerShot", false);
		lightOffsetX = Config::Int("Lighting", "iOffsetX", 0);
		lightOffsetY = Config::Int("Lighting", "iOffsetY", 0);
		lightOffsetZ = Config::Int("Lighting", "iOffsetZ", 0);

		// Reset so the first cut of a new conversation always applies its look.
		lastLookApplied = -1;

		Scene::KeyLight::Engage();

		// With per-angle lighting off (the default) the look is set once here and
		// never touched again.
		if (!lightPerShot) {
			const int look = Scene::FindLook(Config::String("Lighting", "sLook", "natural"));
			Scene::KeyLight::SetLook(look >= 0 ? look : Scene::DefaultLook());
			Scene::KeyLight::SetOffset(lightOffsetX, lightOffsetY, lightOffsetZ);
			lastLookApplied = look;
		}

		Scene::Focus::Configure(
			Config::Bool("Lighting", "bDepthOfField", false),
			Config::Int("Lighting", "iBlur", 55) / 100.0f);
		Scene::Focus::Engage();

		// The player beat is read in ReadTuning with the other settings.
		Log::Info(Log::Category::kCamera, "Staging opened on {}{}{}."sv,
			a_speaker->GetName() ? a_speaker->GetName() : "<unnamed>",
			sceneMode ? " (filming their conversation)"sv : ""sv,
			poseMode == 0 ? ""sv : " [iPoseMode NOT 0 - diagnostic camera write in force]"sv);

		// Logged per conversation, mode 0 included, since the mode can change between
		// conversations.
		Log::Info(Log::Category::kCamera, "iPoseMode={} ({}){}"sv,
			poseMode, PoseModeName(poseMode),
			poseMode == 1 || poseMode == 2 ?
				" - CAMERA WILL NOT MOVE; a lipsync result here is not a fix."sv :
				""sv);

		// Replay the greeting line that arrived before staging. Last in Open() because
		// ApplyCue reads settings that ReadTuning just refreshed. The age check drops
		// leftovers; the speaker check drops lines from anyone else (a guard muttering
		// nearby). Not in scene mode, where the line that started it is already
		// counted.
		if (pendingCue.valid && sceneMode) {
			pendingCue = {};
		}
		if (pendingCue.valid) {
			const auto stashed = pendingCue.speaker.get();
			const bool fresh = SecondsSince(pendingCue.at) <= kGreetingGrace;
			const bool sameSpeaker = stashed && stashed.get() == a_speaker;

			if (fresh && sameSpeaker) {
				Log::Info(Log::Category::kDialogue,
					"Greeting of {} word(s) arrived {:.0f}ms before staging; applying it now."sv,
					pendingCue.words, SecondsSince(pendingCue.at) * 1000.0f);
				ApplyCue(a_speaker, pendingCue.words, pendingCue.intensity, pendingCue.emotion, pendingCue.text);
			}

			pendingCue = {};
		}

		// Open on whoever is speaking, or on the player if nobody is.
		//
		// A resume asks Session instead of waiting for a cue: LineWatch doesn't
		// re-announce a response that's still active, so a line that was running when
		// the menu opened produces no cue when it closes. Session::Speaking() is the
		// live answer (the line may have finished while the menu was up).
		if (resuming) {
			npcSpeaking = Dialogue::Session::GetSingleton().Speaking();
		}

		// A scene opens on the NPC it was staged on.
		if (sceneMode) {
			npcSpeaking = sceneSpeakerIsA;
			wasSpeaking = npcSpeaking;
			pendingSpeaking = npcSpeaking;
		}

		// After the cue replay, which sets npcSpeaking.
		{
			// Weighted like every other draw. Falls back to Canonical() when nothing in
			// the opener set is drawable.
			const auto pool = npcSpeaking ?
				std::span<const ShotType>{ kOpeners } :
				std::span<const ShotType>{ kPlayerOpeners };

			const auto opening = WeightedPick(pool, Drawable);
			const auto opener = (opening ? opening : Canonical()).value_or(ShotType::kCount);

			currentShot = opener;
			previousShot = opener;
			shotSince = Clock::now();

			// The opening shot is the first cut, so the greeting's own turn edge doesn't
			// owe another one.
			turnSinceCut = false;
			cueSinceCut = false;
			linesSinceCut = 0;

			if (opener != ShotType::kCount) {
				Log::Info(Log::Category::kContinuity,
					"Opening on {}: {}."sv, npcSpeaking ? "the speaker"sv : "you (nobody speaking)"sv, Name(opener));
			} else {
				Log::Info(Log::Category::kContinuity, "No enabled opening shot; using camera fallback."sv);
			}
		}

		// A resume puts back the angle that was on screen, so returning from a
		// merchant doesn't look like a new conversation starting. Only when that angle
		// still favours the right person: otherwise wrongSubject drops the floor to
		// the turn floor and coverage cuts away a quarter second later. Neutral setups
		// frame both people and are always fine to restore.
		if (resuming) {
			const bool staleSubject =
				!IsNeutral(resumeShot) && FavoursNpc(resumeShot) != npcSpeaking;

			if (!Drawable(resumeShot)) {
				Log::Info(Log::Category::kCamera, "Stored shot {} is disabled; selecting enabled coverage."sv, Name(resumeShot));
			} else if (staleSubject) {
				Log::Info(Log::Category::kCamera,
					"Cinematic resumed on {} [{:08X}]; kept the opening {} rather than {}, which "
					"frames {} and would have been cut off in {:.2f}s."sv,
					a_speaker->GetName() ? a_speaker->GetName() : "<unnamed>",
					a_speaker->GetFormID(), Name(currentShot), Name(resumeShot),
					FavoursNpc(resumeShot) ? "them"sv : "you"sv, minTurnSeconds);
			} else {
				currentShot = resumeShot;
				previousShot = resumeShot;
				shotSince = Clock::now();

				Log::Info(Log::Category::kCamera,
					"Cinematic resumed on {} [{:08X}], back on {}."sv,
					a_speaker->GetName() ? a_speaker->GetName() : "<unnamed>",
					a_speaker->GetFormID(), Name(currentShot));
			}
		}

		// Last, so it wins over the opening shot chosen above.
		if (openShotHold > 0 && Drawable(ShotType::kClosePlayer)) {
			currentShot = ShotType::kClosePlayer;
			previousShot = currentShot;
			shotSince = Clock::now();
			openShotUntil = Clock::now() + std::chrono::milliseconds(openShotHold);

			turnSinceCut = false;
			cueSinceCut = false;
			linesSinceCut = 0;

			Log::Warn(Log::Category::kContinuity,
				"iOpenShotHold={}ms - forcing a frontal close on the PLAYER to open. "
				"Diagnostic for the lipsync latch; 0 restores normal opening."sv,
				openShotHold);
		}
		if (resumeFallback) {
			visibilityRecovery.EnterFallback(VisibilityTime());
			fallbackTurn = turnSerial;
			fallbackCue = cueCount;
		}
		provisionalStage = a_provisional && !sceneMode;

		Log::Info(Log::Category::kCamera, "Conversation setup completed in {:.2f}ms ({})."sv,
			std::chrono::duration<float, std::milli>(Clock::now() - openingStarted).count(),
			resuming ? "resume"sv : a_provisional ? "reopened menu, before its live line"sv :
			a_restaging ? "recovery"sv : "new conversation"sv);
	}

	void Director::LoadSettings()
	{
		ReadTuning();
	}

	Tunables Director::GetTunables()
	{
		return tunables;
	}

	void Director::ApplyTunables(const Tunables& a_tunables)
	{
		tunables = a_tunables;

		const auto seconds = [](int a_hundredths) {
			return static_cast<float>(std::max(0, a_hundredths)) / 100.0f;
		};

		minShotSeconds = seconds(tunables.minShotTime);
		minTurnSeconds = seconds(tunables.minTurnTime);
		playerBeatSeconds = seconds(tunables.playerBeat);
		playerVoiceHoldSeconds = seconds(tunables.playerVoiceHold);
		reactionSettings = { tunables.reactionShots, tunables.reactionEvery, tunables.reactionChance };

		// A ceiling below the floor would make every shot instantly stale.
		maxShotSeconds = std::max(seconds(tunables.maxShotTime), minShotSeconds);

		cutEveryMin = static_cast<std::uint32_t>(std::clamp(tunables.cutEveryMin, 1, 20));
		cutEveryMax = static_cast<std::uint32_t>(std::clamp(tunables.cutEveryMax, 1, 20));
		perLineAngleChange = tunables.perLineAngleChange;
		holdOnShortLines = tunables.holdOnShortLines;
		shortLineWords = static_cast<std::uint32_t>(std::clamp(tunables.shortLineWords, 1, 40));
		timedCutsWhileSpeaking = tunables.timedCutsWhileSpeaking;
		timedCutsWhileChoosing = tunables.timedCutsWhileChoosing;
		directing = tunables.enabled;
		coverPlayerTurn = tunables.coverPlayerTurn;
		enforceLine = tunables.enforceLine;
		if (tunables.true180 != true180 && staging) {
			lineRuleChanged.store(true, std::memory_order_relaxed);
		}
		true180 = tunables.true180;
		followFace = tunables.followFace;
		playerGestureCue = tunables.playerGestureCue;
		Scene::StopWork::SetEnabled(tunables.stopWorkToTalk);
		avoidCrowds = tunables.avoidCrowds;

		// Not paired with a heldRoom reset. Turning this on keeps the current shot's
		// last real measurement; turning it off just resumes probing.
		holdPlacement = tunables.holdPlacement;
		const bool wantProtection = tunables.protectSubject && !holdPlacement;
		if (wantProtection != protectSubject) {
			protectionSettingsDirty.store(true, std::memory_order_relaxed);
		}
		protectSubject = wantProtection;
		firstPersonFallback = tunables.firstPersonFallback;
		if (tunables.protectSubject && holdPlacement) {
			Log::Warn(Log::Category::kCamera,
				"Keep Subject Visible is inactive: Ignore Obstructions Mid-Shot takes priority. Disable the hold to enable protection."sv);
		}
		tunables.protectSubject = wantProtection;

		fadeTopicList = tunables.fadeTopicList;
		fadeAfterPlayerLine = tunables.fadeAfterPlayerLine;

		// Plain stores, read on the tick. The beat ends itself if switched off
		// mid-answer; the subtitles move back from the tick, which owns the movies.
		persuasionBeatEnabled = tunables.persuasionBeat;
		subtitlesInBar = tunables.subtitlesInBar;
		sceneTriggerSettings = { tunables.sceneAuto,
			static_cast<float>(std::clamp(tunables.sceneWait, 0, 600)) / 100.0f };
		sceneRange = static_cast<float>(std::clamp(tunables.sceneRange, 150, 2000));

		// Recorded here and applied on the tick. ApplyTunables runs from the settings
		// panel on the Present hook, and all Scaleform calls belong to the tick.
		if (tunables.hideSpeakerName != wantHideSpeakerName) {
			wantHideSpeakerName = tunables.hideSpeakerName;
			interfaceDirty = true;
		}
		// Applied here as well as in ReadTuning so dragging the slider is felt in the
		// conversation that's already open.
		choiceFadeDelay = static_cast<float>(std::clamp(tunables.choiceFadeDelay, 0, 600)) / 100.0f;
		choiceFadeOut = static_cast<float>(std::clamp(tunables.choiceFadeTime, 5, 200)) / 100.0f;
		poseMode = std::clamp(tunables.poseMode, 0, 5);

		// Only re-roll if the current target is outside the new range; re-rolling on
		// every apply would restart the count while a slider is being dragged.
		if (cutEveryTarget < std::min(cutEveryMin, cutEveryMax) ||
			cutEveryTarget > std::max(cutEveryMin, cutEveryMax)) {
			cutEveryTarget = RollCutEvery();
		}

		Render::Letterbox::SetBarFraction(
			static_cast<float>(std::clamp(tunables.letterboxHeight, 0, 300)) / 1000.0f);

		// Live, and only while staged: ticking the box mid-conversation puts the bars
		// up, and outside a conversation nothing is shown.
		letterboxWanted = tunables.letterbox;
		if (staging) {
			Render::Letterbox::SetVisible(letterboxWanted);
		}

	}

	void Director::Close()
	{
		if (!staging) {
			return;
		}

		const bool ownedCamera = Compat::SmoothCam::Holding();
		if (viewMode == ViewMode::kFirstPersonFallback && fallbackThirdPersonOwed && !suspendingForMenu) {
			restoreThirdPersonPending = true;
			if (!TryHandBackView()) {
				handBackPending = true;
				handBackSince = Clock::now();
			}
		}
		viewMode = ViewMode::kCinematic;
		fallbackRequested = false;
		fallbackThirdPersonOwed = false;
		protectedPose = {};
		protectedSearch = {};
		frameSubjects.reset();
		visibilityRecovery.Reset();
		staging = false;
		playerVoiceHandoff.Reset(Scene::LipSync::PlayerLineSerial());
		reactionShots.Reset();
		replyBoundary.Reset();
		subject = {};
		haveShot = false;
		npcSpeaking = false;
		releasePending = false;

		persuasion.End();
		beatInfo = nullptr;
		pickedCheck = SpeechCheck::kNone;
		beatShot = ShotType::kCount;

		provisionalStage = false;

		// Whoever ended the scene has already logged why.
		if (sceneMode) {
			sceneEndedKey = sceneKey;
			sceneEndedAt = Clock::now();
			sceneTrigger.Restart();
		}
		sceneMode = false;
		sceneForm = 0;
		counterpart = {};
		sceneAutomatic = false;
		sceneWaitExempt = false;
		sceneAheadReported = 0;

		// Cleared rather than left at the last value: a suspend closes and a resume
		// reopens the same conversation (same serial), and Open() mustn't mistake that
		// for a repeat call.
		stagedSerial = 0;

		// The hold on the spent list belongs to the conversation.
		playerLineHeld = false;

		Render::Letterbox::SetVisible(false);
		Scene::StopWork::End();
		Scene::Presence::Release();
		Scene::Performance::Release();
		Scene::FaceGen::End();
		Scene::LipSync::Release();
		Scene::KeepPosed::Clear();
		spaceReported = -1;
		Scene::Subtitles::Release();
		Scene::Interface::Restore();
		Scene::Interface::SetKeepSubtitles(false);
		Scene::KeyLight::Release();
		Scene::Focus::Release();

		// Every camera write happens before SmoothCam is handed the camera back, so
		// there's no window where two plugins think they own it.
		//
		// The state change is guarded on still being in third person; the player may
		// have switched to first person themselves, or the engine may be running a
		// killcam, a mount or furniture camera.
		//
		// The lens is always restored at the end of a conversation, but not across a
		// suspension: snapping to the player's FOV right before an inventory opens
		// reads as a sudden zoom-out frozen behind the menu. If the conversation ends
		// while the menu is open, AbandonSuspension restores it.
		if (!suspendingForMenu && ownedCamera) {
			RestoreFieldOfView();
		}

		// Restore the resting aim and zoom (nine fields of ThirdPersonState, including
		// the persistent zoom), so the player isn't left in the engine's dialogue
		// camera aimed at the NPC. Skipped across a suspension: the conversation comes
		// straight back and the writes would just churn whatever else manages the
		// camera. Done before ForceFirstPerson so it lands while third person is still
		// active.
		if (!suspendingForMenu && ownedCamera) {
			RestoreCameraRest();
		}

		// Also skipped across a suspension (see OnScreenTaken).
		if (returnToFirstPerson) {
			returnToFirstPerson = false;
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInThirdPerson()) {
				camera->ForceFirstPerson();
				Log::Info(Log::Category::kCamera, "Returned the player to first person."sv);
			}
		}

		// Last: everything above puts things back before SmoothCam gets the camera.
		Compat::SmoothCam::Release();

		Log::Info(Log::Category::kCamera, "Staging closed{}."sv,
			suspendingForMenu ? " for a menu; the view is left where it is"sv : ""sv);
	}

	void Director::OnDialogueMenu(bool a_opening)
	{
		// Set before the staging check: the dialogue menu opens and the greeting
		// starts about 120 ms before Open() runs, and the click guard needs to know
		// during that window.
		dialogueMenuUp.store(a_opening, std::memory_order_relaxed);

		// Move the subtitle into the bar before the menu draws its first frame; the
		// tick would only get to it a frame or two later.
		if (a_opening && staging && !sceneMode && subtitlesInBar) {
			Render::Letterbox::SetBeneath(Render::Letterbox::Beneath::kDialogue);
			Scene::Subtitles::Place(Render::Letterbox::TargetFraction(), false, 0.0f);
		}

		// A scene doesn't end on a dialogue menu; Runtime hands the camera to the
		// player's own conversation. See SceneOver.
		if (!staging || sceneMode) {
			return;
		}

		if (a_opening) {
			releasePending = false;
			return;
		}

		// The player has left, or the menu is blinking between topics. Start the grace
		// timer rather than deciding now.
		releasePending = true;
		releaseSince = Clock::now();
	}

	// A menu took over the screen (training, barter, a book, inventory). These
	// pause the game, which stops the tick, so the decision has to be made here:
	// MenuOpenCloseEvent is the one callback that still arrives.
	//
	// The conversation is released in full with Close() so the camera, lens, bars,
	// HUD and topic list are all back before the menu draws, and re-staged in full
	// with Open() when the menu closes. Only three things are carried across:
	// which conversation it was, the angle on screen, and whether the player is
	// owed first person. If the conversation ended while the menu was up, Runtime
	// calls AbandonSuspension instead.
	void Director::OnScreenTaken(std::string_view a_menu)
	{
		if (!staging) {
			return;
		}

		// A scene isn't suspended; it just ends. Auto mode will pick it up again if
		// the player stays put.
		if (sceneMode) {
			Render::Letterbox::Retract();
			Log::Info(Log::Category::kCamera, "Stopped filming: '{}' took the screen."sv, a_menu);
			Close();
			return;
		}

		// Everything the resume needs, recorded before Close() clears it.
		const auto partner = subject.get();
		suspendedPartner = partner ? partner->GetFormID() : 0;
		suspendedSerial = Dialogue::Session::GetSingleton().ConversationSerial();
		suspendedShot = currentShot;
		suspendedFirstPersonOwed = returnToFirstPerson;
		suspendedInFallback = viewMode == ViewMode::kFirstPersonFallback;
		suspendedThirdPersonOwed = fallbackThirdPersonOwed;
		suspendedForMenu = suspendedPartner != 0;

		// Cleared before Close() so the release doesn't return the player to first
		// person in the middle of the conversation.
		returnToFirstPerson = false;

		// Remove the bars immediately; Close() eases them out, which would clip the
		// top of the menu.
		Render::Letterbox::Retract();

		// Named, because the next report of this should arrive already diagnosed.
		Log::Info(Log::Category::kCamera,
			"Cinematic suspended: '{}' took the screen{}."sv, a_menu,
			suspendedForMenu ? " (resume armed)"sv : " (no partner recorded; not resuming)"sv);

		// Tells Close() this is a suspension. A flag rather than a parameter because
		// the other callers never suspend; cleared here so an early return in Close()
		// can't leave it set.
		suspendingForMenu = true;
		Close();
		suspendingForMenu = false;

		// Ask Runtime to stage again when the screen comes back. Runtime opens once
		// per partner and won't reopen while the key matches, so clearing the key is
		// what makes the resume happen. Runtime's Stranded() deliberately doesn't
		// cover this path.
		Runtime::RearmConversation();
	}

	void Director::OnScreenReleased()
	{
		// Not gated on staging: the reading it protects is taken while nothing is
		// staged.
		screenReleasedAt = Clock::now();
	}

	bool Director::Suspended() noexcept
	{
		return suspendedForMenu;
	}

	RE::FormID Director::SuspendedFor() noexcept
	{
		return suspendedPartner;
	}

	void Director::AbandonSuspension(bool a_handBackView)
	{
		// A load also cancels a deferred restore when nothing was suspended; nothing
		// owed in the old save carries into the new one.
		if (!a_handBackView) {
			restoreThirdPersonPending = false;
			handBackPending = false;
		}
		if (!suspendedForMenu) {
			return;
		}

		Log::Info(Log::Category::kCamera,
			"Cinematic resume skipped: the conversation with [{:08X}] ended while the "
			"menu was open."sv,
			suspendedPartner);

		const bool owedFirstPerson = suspendedFirstPersonOwed;
		const bool owedThirdPerson = suspendedThirdPersonOwed;

		suspendedForMenu = false;
		suspendedPartner = 0;
		suspendedSerial = 0;
		suspendedFirstPersonOwed = false;
		suspendedInFallback = false;
		suspendedThirdPersonOwed = false;

		if (!a_handBackView) {
			restoreThirdPersonPending = false;
			// A load drops everything and writes nothing, including any pending hand-back,
			// whose saved values belong to the world being unloaded.
			handBackPending = false;
			return;
		}

		// The part of Close() a suspension skipped. The conversation ended behind the
		// menu, so the resting aim, zoom and lens are owed now.
		//
		// SmoothCam has owned the camera since the suspension, so take it back, write,
		// and release it again, the same order Close() uses. If the camera is refused,
		// the write is deferred rather than dropped: handBackPending stays set and
		// Tick retries it on an idle frame.
		restoreThirdPersonPending = restoreThirdPersonPending || owedThirdPerson;
		if (!TryHandBackView()) {
			handBackPending = true;
			handBackSince = Clock::now();

			Log::Warn(Log::Category::kCamera,
				"Conversation ended behind a menu, but the camera is held by another plugin. "
				"Nothing written; the hand-back is queued and will be retried."sv);
		}

		// Outside the ownership block: returning the player to first person is a view
		// change, not a camera transform, and doesn't need SmoothCam's permission.
		if (owedFirstPerson) {
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->IsInThirdPerson()) {
				camera->ForceFirstPerson();
				Log::Info(Log::Category::kCamera,
					"Returned the player to first person after the menu."sv);
			}
		}
	}

	void Director::OnCue(RE::Actor* a_speaker, RE::DialogueResponse* a_response)
	{
		if (!a_response) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (a_speaker && player && a_speaker == static_cast<RE::Actor*>(player)) {
			return;
		}

		// A scene is driven by its own two people; a third voice nearby is logged and
		// ignored, like a stranger in the player's own conversation below.
		if (staging && sceneMode) {
			const char* text = a_response->text.c_str();
			SceneLine(a_speaker, WordCount(text), a_response->percent, text ? std::string_view{ text } : std::string_view{});
			return;
		}

		// Only the conversation partner drives the conversation. LineWatch hooks every
		// Character, so a guard across the market or a follower's idle line would
		// otherwise retire the player's voice handle, change the intensity, trigger
		// cuts, and fade the topic list while the player is still reading. Logged
		// rather than dropped silently. A partner change shows up as a new
		// conversation from Session, not as a stray cue.
		if (staging) {
			auto partner = subject.get();
			if (!partner || partner.get() != a_speaker) {
				const char* who = a_speaker ? a_speaker->GetName() : nullptr;
				Log::Info(Log::Category::kDialogue,
					"Ignored a line from {} — not the partner this conversation is staged on."sv,
					(who && *who) ? who : "<unnamed>");
				return;
			}
		}

		const std::uint32_t words = WordCount(a_response->text.c_str());
		const auto          intensity = a_response->percent;
		const auto          emotion =
			static_cast<std::uint32_t>(a_response->animFaceArchType.underlying());

		// The greeting's cue lands about 120 ms before Open() runs. Stashed here and
		// applied by Open() once staging exists and the settings have been read;
		// dropping it would lose the first line of every conversation.
		if (!staging) {
			pendingCue.speaker = a_speaker ? a_speaker->GetHandle() : RE::ActorHandle{};
			pendingCue.words = words;
			pendingCue.intensity = intensity;
			pendingCue.emotion = emotion;
			pendingCue.text = a_response->text.c_str();
			pendingCue.at = Clock::now();
			pendingCue.valid = true;
			return;
		}

		ApplyCue(a_speaker, words, intensity, emotion, a_response->text.c_str());
	}

	void Director::Tick(float a_delta)
	{
		// Reconcile MenuWatch's list of screen-owning menus every frame. It's built
		// from open/close events, and one missed close would otherwise block staging
		// for good. Runs before the staging check because Runtime reads it either way.
		Dialogue::MenuWatch::Reconcile();

		// Runs before the staging check and outside the session check, so the probe
		// also reports with bEnabled=0 (the control configuration) and before a
		// conversation opens. Outside a conversation the NPC half just doesn't report.
		{
			auto& session = Dialogue::Session::GetSingleton();
			auto  partner = session.Active() ? session.Partner().get() : RE::NiPointer<RE::Actor>{};
			Scene::Performance::Probe(partner.get(), a_delta);
		}

		Scene::FaceGen::Tick(a_delta);

		// Before the morph pass: Tick runs in PlayerCharacter::Update, ahead of the
		// traversal that consumes the channel, so FaceGen sees this frame's value.
		Scene::LipSync::Update(a_delta);

		// The player's expression runs here too, not in Performance::Update, which
		// returns early when an anchor fails. It also has to keep running after
		// Release so the ease-out finishes and the override flag gets handed back.
		Scene::Performance::UpdateFace(a_delta);

		// Before the staging check: Open() suppresses the HUD on the frame it stages,
		// so a settings change made just before a conversation has to land first.
		SyncInterfaceSettings();

		// Every frame, staged or not, so the bars are under the interface from their
		// first frame.
		Render::Letterbox::SetBeneath(BarsBeneath());

		// Every subtitled line, whoever says it. Feeds the film key, auto mode and
		// scene filming. Subtitles carry no emotion, so they count as intensity 50.
		for (const auto& line : Dialogue::SceneWatch::Poll()) {
			if (staging && sceneMode) {
				if (auto speaker = line.speaker.get()) {
					SceneLine(speaker.get(), WordCount(line.text.c_str()), 50, line.text);
				}
			}
		}

		// Hotkey requests, drained here because the camera hook stops firing outside
		// third person. Consumed only when staged, so a key pressed outside a
		// conversation clears itself instead of firing when the next one opens.
		{
			const bool wantCut = requestCut.exchange(false, std::memory_order_relaxed);
			const bool wantFraming = requestFraming.exchange(false, std::memory_order_relaxed);
			const bool lineRuleFlipped = lineRuleChanged.exchange(false, std::memory_order_relaxed);

			// The film key works staged or not, so it's handled before the branch below.
			// Starting a scene makes this a staged frame.
			if (requestScene.exchange(false, std::memory_order_relaxed)) {
				ToggleScene();
			}

			if (staging) {
				if (wantFraming) {
					framing = NextFraming(framing);

					// A framing change is a cut; otherwise the press appears to do nothing until
					// the shot's minimum hold runs out.
					forcedCut = true;

					// Expires at the end of the turn it was set in. Pressing again in the same
					// turn cycles the choice and re-arms it.
					framingUntilTurn = turnSerial;

					Log::Info(Log::Category::kContinuity,
						"Framing forced to {} by hotkey, for the rest of this turn."sv,
						FramingLabel(framing));

					char note[64]{};
					std::snprintf(note, sizeof(note), "Camera: %.*s",
						static_cast<int>(FramingLabel(framing).size()), FramingLabel(framing).data());
					Notify(note);
				}

				if (wantCut) {
					forcedCut = true;
				}

				// true180 changed mid-conversation. Only the player's shots move, so cut if
				// one is on screen. Clearing heldSince makes it re-pick its angle if the cut
				// finds nothing better.
				if (lineRuleFlipped) {
					lastPlayerPose = {};
					if (!FavoursNpc(currentShot)) {
						forcedCut = true;
						heldSince = {};
					}
				}

				// The override hands back when its turn ends. Checked here because the camera
				// hook doesn't run outside third person. No forced cut: auto already agrees
				// with the override for the rest of that turn, so the next real cut moves the
				// camera.
				if (framing != Framing::kAuto && turnSerial != framingUntilTurn) {
					Log::Info(Log::Category::kContinuity,
						"Framing hold on {} expired with the turn; back to automatic."sv,
						FramingLabel(framing));
					framing = Framing::kAuto;
				}
			}
		}

		// Sample the player's own camera while nothing is staged. The finer checks are
		// in SampleCameraRest.
		if (!staging) {
			// Retry a hand-back the camera was refused for (see AbandonSuspension). Only
			// when nothing is staged, no conversation is running and no menu owns the
			// screen. No early return, so the topic list release below still runs.
			if (handBackPending && SecondsSince(handBackSince) >= kHandBackRetrySeconds) {
				handBackSince = Clock::now();

				if (!Dialogue::Session::GetSingleton().Active() &&
					!Dialogue::MenuWatch::ScreenTaken() && TryHandBackView()) {
					handBackPending = false;
					Log::Info(Log::Category::kCamera,
						"Queued hand-back completed; the lens and the resting view are back."sv);
				}
			}

			SampleCameraRest();

			// Close() hands the topic list back, but that can fail when there's no
			// dialogue movie yet. A hidden list still commits on Accept, so retry it here.
			// Free when nothing is owed.
			Scene::Interface::ReleaseChoices();

			// Last, after the resting camera has been sampled; a scene staged here hands
			// back to exactly that.
			TickAutoScene(a_delta);
			return;
		}

		// A scene has its own end conditions. Checked before the conversation checks
		// below, which would end a scene immediately for not being the player's
		// conversation. The paused-game check still applies.
		if (sceneMode) {
			std::string_view why{};
			if (SceneOver(why)) {
				Log::Info(Log::Category::kCamera, "Stopped filming: {}."sv, why);
				Close();
				return;
			}
		}

		// Leaving a conversation hands the camera straight back instead of waiting for
		// the NPC to finish their line.
		if (releasePending && SecondsSince(releaseSince) >= kReleaseGraceSeconds) {
			Close();
			return;
		}

		// Something paused the game and took the screen (inventory, map, another mod's
		// camera session). Staging over it would fight for the camera.
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			Log::Info(Log::Category::kCamera, "Another menu took the screen; releasing."sv);
			Close();
			return;
		}

		// The player left even though the NPC is still talking: the topic manager's
		// speaker goes null and only lastSpeaker remains. Leaving mid-line closes the
		// menu and then reopens it for the trailing line, which is why the menu grace
		// alone isn't enough.
		//
		// But a null speaker also happens between two conversations with the same
		// person (commit a topic mid-line, or talk again right away), while the
		// dialogue menu stays up. So the menu must also be down. The game disables
		// movement while the dialogue menu is up, so a player looking at it can't have
		// walked away. Asked of the UI directly rather than the dialogueMenuUp latch,
		// which is built from events and could get stuck.
		if (!sceneMode && SecondsSince(stagingSince) > kExitCheckDelay && !DialogueMenuOpen()) {
			if (auto* manager = RE::MenuTopicManager::GetSingleton()) {
				const bool inConversation = static_cast<bool>(manager->speaker.get());
				if (!inConversation) {
					Log::Info(Log::Category::kCamera, "Player left the conversation; releasing."sv);
					Close();
					return;
				}
			}
		}

		// The subject is gone: unloaded, dead, or left with a cell change.
		if (!subject.get()) {
			Close();
			return;
		}

		// Combat ends it, unless the game is holding a conversation open anyway. The
		// player's combat flag stays up while anything hostile is still searching for
		// them, and the game will still open a follower's dialogue during that. If the
		// fight actually reaches them the menu closes and this releases as usual.
		if (auto* player = RE::PlayerCharacter::GetSingleton();
			player && player->IsInCombat() && (sceneMode || !DialogueMenuOpen())) {
			Log::Info(Log::Category::kCamera, "Combat started; releasing."sv);
			Close();
			return;
		}

		// The dialogue menu is gone and the session has ended. Backstop for exit paths
		// that don't fire a menu-close event.
		if (!sceneMode && !Dialogue::Session::GetSingleton().Active() && !releasePending) {
			Close();
		}

		// Last, behind a fresh staging check: everything above can call Close(), and
		// the interface mustn't be driven after that. Here rather than in the camera
		// hook because the screen has to stay clear for the whole conversation, not
		// just while the camera is in third person.
		if (staging) {
			if (protectionSettingsDirty.exchange(false, std::memory_order_relaxed)) {
				protectedPose = {};
				protectedSearch = {};
				visibilityFailedObservation = false;
				visibilityCheckedAt = {};
				protectedRetryAt = {};
				if (viewMode == ViewMode::kFirstPersonFallback) {
					visibilityRecovery.EnterFallback(VisibilityTime());
				} else {
					visibilityRecovery.Reset();
				}
			}
			frameSubjects = SampleSubjects(a_delta);

			// Player conversation only: the topic list, the player's voice, picks,
			// gestures, stop-work and the persuasion beat.
			if (!sceneMode) {
				const auto dialoguePhase = Scene::Interface::ReadDialoguePhase();
				const bool choosing = dialoguePhase.valid && !dialoguePhase.lineInFlight &&
					dialoguePhase.phase == Scene::Interface::MenuPhase::kTopicList &&
					!Compat::DBReV::Speaking();
				// Frame time, capped so a hitch can't skip a hold, and paused with the game.
				const float frameDelta = std::clamp(a_delta, 0.0f, 0.25f);

				const bool wasHandingOff = playerVoiceHandoff.Active();
				const bool wasHolding = playerVoiceHandoff.Holding();
				playerVoiceHandoff.SetDelay(playerVoiceHoldSeconds);
				playerVoiceHandoff.Update(Scene::LipSync::PlayerLineSerial(),
					Scene::LipSync::PlayerSpeaking(), npcSpeaking, choosing, frameDelta);
				if (!wasHolding && !wasHandingOff && playerVoiceHandoff.Holding()) {
					Log::Info(Log::Category::kContinuity,
						"Player voice ended; holding on you for {:.2f}s."sv, playerVoiceHoldSeconds);
				}
				if (!wasHandingOff && playerVoiceHandoff.Active()) {
					if (!wasHolding) {
						Log::Info(Log::Category::kContinuity,
							"Player voice ended; framing the NPC before the reply."sv);
					} else if (npcSpeaking) {
						Log::Info(Log::Category::kContinuity,
							"Their reply started; ending the hold on you early."sv);
					} else {
						Log::Info(Log::Category::kContinuity,
							"Hold on you finished; framing the NPC before the reply."sv);
					}
				}

				TrackTopicPicks(dialoguePhase, frameDelta);
				CuePlayerGesture();
				Scene::StopWork::Update(frameDelta, npcSpeaking);
				TickPersuasion();
			}

			if (protectSubject && !frameSubjects && viewMode == ViewMode::kCinematic) {
				fallbackRequested = firstPersonFallback;
			}
			TickProtected(frameSubjects);
			DriveInterface();
			PlaceSubtitles(a_delta);
			PublishPosed();
		}
	}

	void Director::OnThirdPersonUpdate(RE::ThirdPersonState* a_state)
	{
		if (!staging || !a_state || viewMode != ViewMode::kCinematic || fallbackRequested) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		auto* root = camera ? camera->cameraRoot.get() : nullptr;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  speakerPtr = subject.get();
		if (!root || !player || !speakerPtr) {
			return;
		}

		// Getting out of bed is a new setup, not a move. Getting up passes through
		// several states and each re-measures, so the height settles as the animation
		// plays instead of locking onto a half-risen pose.
		if (!frameSubjects) {
			return;
		}
		auto subjects = *frameSubjects;
		const float delta = subjects.delta;

		const float held = SecondsSince(shotSince);

		// Timed Angle Change, the only purely clock-driven cut. Separate settings for
		// each side of the exchange: a timed cut during a speech interrupts it, while
		// during a topic list nothing else would move the camera. Both off by default.
		// In scene mode someone always has the floor.
		const bool timedAllowed = (npcSpeaking || sceneMode) ? timedCutsWhileSpeaking : timedCutsWhileChoosing;

		// Once the persuasion beat has cut to their close-up, neither the clock nor
		// the line count moves the camera until the answer is over. Coverage and the
		// hotkeys still can.
		const bool beatHolding = persuasion.Holding();
		const bool beatDue = persuasion.CutOwed() && npcSpeaking && !sceneMode && framing == Framing::kAuto;

		const bool stale = !beatHolding && timedAllowed && held >= maxShotSeconds;

		// Per Line Angle Change: hold until this many eligible lines have gone by. A
		// turn change doesn't count as a reason on its own (otherwise every answer in
		// a back-and-forth would cut and the cadence setting would do nothing); it
		// only selects the shorter floor below.
		const bool motivated = !beatHolding && perLineAngleChange && cueSinceCut &&
			linesSinceCut >= cutEveryTarget;

		// Hold the player through the start of the reply, so the choice has a moment
		// to land. Only for unvoiced turns; voiced lines get iPlayerVoiceHold instead,
		// which ends as soon as the reply starts. Never in scene mode.
		const bool inPlayerBeat = !sceneMode && npcSpeaking && !playerVoiceHandoff.Active() &&
			(playerVoiceHandoff.Holding() ||
				std::chrono::duration<float>(Clock::now() - replyStartedAt).count() < playerBeatSeconds);

		// Coverage isn't subject to the shot timer: if the shot frames the wrong
		// person, fix it now. The minimum hold exists to stop accent cuts from
		// strobing, not to veto this.
		//
		// A neutral shot is never the wrong subject, which lets an environmental angle
		// hold through a topic list; once the NPC speaks, they're the subject.
		//
		// Coverage is a correction, not a pacing rule: it only fires when the shot
		// frames the wrong person, always lands on the other one, and doesn't spend
		// the line count.

		// Who this frame wants the camera on. Asked once; the tests below read it.
		const bool wantNpc = SubjectIsNpc();
		const bool wantRoom = FramingIsRoom();
		const bool forcedFraming = framing != Framing::kAuto;

		// A neutral shot left over the player's turn also counts as the wrong subject
		// when that turn is covered, so it moves on the turn floor instead of sitting
		// out the full shot floor. Not under a forced framing (pinned to the room,
		// that's the point). During a reaction a neutral is always wrong.
		const bool neutralStranded = (coverPlayerTurn || ReactionActive()) &&
			!forcedFraming && !wantNpc && IsNeutral(currentShot);

		// A forced framing makes "wrong subject" stricter, otherwise the key would
		// only affect the next cut and the current angle would stay up for its full
		// minimum.
		const bool wrongSubject = wantRoom ?
			!IsNeutral(currentShot) :
			(forcedFraming ?
					(IsNeutral(currentShot) || FavoursNpc(currentShot) != wantNpc) :
					(neutralStranded ||
						(!IsNeutral(currentShot) && FavoursNpc(currentShot) != wantNpc)));

		// A key press overrides the hold floor.
		const bool forceNow = forcedCut || !Drawable(currentShot);

		const float floor = (turnSinceCut || wrongSubject || beatDue) ? minTurnSeconds : minShotSeconds;

		Pose smartFrame{};
		if (protectSubject) {
			const bool ordinaryCut = (motivated || stale || wrongSubject || forceNow || beatDue) &&
				(held >= floor || forceNow) && (!inPlayerBeat || forceNow) &&
				(!HoldingOpenShot() || forceNow);
			const auto result = ProtectedFrame(subjects, ordinaryCut);

			// One attempt per answer. The protected search picks its own shot; the beat
			// only asks it to look.
			if (ordinaryCut && beatDue) {
				persuasion.Cut();
			}
			if (!result) {
				if (!fallbackRequested) {
					UseNativeView();
				}
				return;
			}
			smartFrame = *result;
		}

		// A cut needs both a reason and enough time on the current shot.
		if (!protectSubject && (motivated || stale || wrongSubject || forceNow || beatDue) &&
			(held >= floor || forceNow) && (!inPlayerBeat || forceNow)) {
			// Chosen only when a cut is actually happening, not every frame.
			ScoreCache cache{};

			ShotType next = currentShot;
			float    bestQuality = -1.0f;
			bool     found = false;

			for (int attempt = 0; attempt < 4; ++attempt) {
				const auto choice = Choose();
				if (!choice || !Drawable(*choice)) {
					continue;
				}
				const auto candidate = *choice;
				// A candidate that frames the wrong person is never acceptable, however good
				// its sightline. A neutral is fine during the NPC's line, but not on the
				// player's covered turn (or the camera would never cut to the player in a
				// wide-heavy setup), and not once the player has picked a subject by hand.
				const bool neutralAllowed = !forcedFraming && wantNpc;
				const bool correctSubject = wantRoom ?
					IsNeutral(candidate) :
					(FavoursNpc(candidate) == wantNpc || (IsNeutral(candidate) && neutralAllowed));

				if (candidate == currentShot || !correctSubject) {
					continue;
				}

				// Best of the draws, not the first that places. The weights already decided
				// which types were offered; this decides which one the room suits.
				const float quality = Placement(candidate, subjects, cache);
				if (quality > bestQuality) {
					bestQuality = quality;
					next = candidate;
					found = quality >= 0.0f;
				}

				if (bestQuality >= kGoodEnough) {
					break;
				}
			}

			// Nothing in the rotation fits. Try every enabled setup that frames the right
			// person, tightest first, since tight setups need the least room. Sorted first
			// so the raycasts go to the candidates most likely to place.
			if (!found) {
				std::array<ShotType, static_cast<std::size_t>(ShotType::kCount)> ladder{};
				std::size_t                                                      count = 0;

				for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(ShotType::kCount); ++i) {
					const auto candidate = static_cast<ShotType>(i);
					if (candidate == currentShot || !Drawable(candidate)) {
						continue;
					}
					if (wantRoom ? !IsNeutral(candidate) : FavoursNpc(candidate) != wantNpc) {
						continue;
					}
					ladder[count++] = candidate;
				}

				std::sort(ladder.begin(), ladder.begin() + static_cast<std::ptrdiff_t>(count),
					[](ShotType a_lhs, ShotType a_rhs) { return FillOf(a_lhs) > FillOf(a_rhs); });

				// Best of the first few that place, not just the first. Bounded, because the
				// ladder runs exactly when the room is defeating everything and each solve is
				// at its most expensive.
				constexpr int kLadderLooks = 6;

				int   examined = 0;
				float ladderBest = -1.0f;

				for (std::size_t i = 0; i < count; ++i) {
					const float quality = Placement(ladder[i], subjects, cache);
					if (quality < 0.0f) {
						continue;  // will not place here at all
					}

					if (quality > ladderBest) {
						ladderBest = quality;
						next = ladder[i];
						found = true;
					}

					if (ladderBest >= kGoodEnough || ++examined >= kLadderLooks) {
						break;
					}
				}

				bestQuality = ladderBest;
			}

			// The diagnostic opening hold outranks the cut policy until it expires.
			if (found && !forceNow && HoldingOpenShot()) {
				found = false;
			}

			// The request is spent whether or not a shot was found, so a press can't fire
			// again later on its own.
			forcedCut = false;

			// Same for the beat: one attempt per answer. It holds on whatever it lands on.
			const bool beatCut = beatDue && found && Drawable(next) && IsBeatShot(next);
			if (beatDue) {
				persuasion.Cut();
			}

			if (found && Drawable(next)) {
				// Lens, move and placement quality are logged because shot names alone don't
				// show whether the camera did anything different or got the shot it asked for.
				Log::Info(Log::Category::kContinuity,
					"Cut: {} ({}) -> {} ({}) [{:.0f}deg {} q{:.2f} {}] after {:.2f}s / {} line(s) ({}, {}, intensity {})."sv,
					Name(currentShot), SubjectName(currentShot),
					Name(next), SubjectName(next),
					LensOf(next), MoveName(next),
					bestQuality < 0.0f ? 0.0f : bestQuality, SpaceName(roomSpace),
					held, linesSinceCut,
					npcSpeaking ? "speaking"sv : "waiting"sv,
					// Reasons in the same order as the test above, so the log names the one that
					// actually fired.
					forceNow    ? "by hand"sv :
						beatCut      ? "persuasion"sv :
						wrongSubject ? "coverage"sv :
						motivated    ? "line count"sv :
						stale        ? "timer"sv :
									   "unknown"sv,
					cueIntensity);
				previousShot = currentShot;
				currentShot = next;
				shotSince = Clock::now();

				// The beat's shot pushes in for the length of the answer and keeps that move
				// for the rest of the shot.
				if (beatCut) {
					beatShot = next;
					beatShotSince = shotSince;
					beatPushSeconds = SpeechChecks::PushSeconds(lastCueWords);
					Log::Info(Log::Category::kContinuity,
						"Persuasion beat: {} for the answer, pushing in over {:.1f}s."sv,
						Name(next), beatPushSeconds);
				}

				// Only cuts that wanted a new angle reset the line count. A coverage cut is a
				// handover, not an angle change; letting it reset the tally meant the count
				// never reached its target and the cadence setting appeared to do nothing.
				// Re-rolled here, at the cut.
				if (motivated || stale || forceNow) {
					linesSinceCut = 0;
					cutEveryTarget = RollCutEvery();
				}
			}
			cueSinceCut = false;
			turnSinceCut = false;
		}

		// The move runs on this shot's own duration, not the staleness ceiling; at the
		// ceiling's 9 s most moves would barely start before the next cut.
		const float moveSeconds =
			std::max(static_cast<float>(Shot::MoveTime(currentShot)) / 100.0f, 0.1f);
		subjects.progress = std::clamp(SecondsSince(shotSince) / moveSeconds, 0.0f, 1.0f);

		// The angle and standoff this shot committed to, handed back to Solve. Set
		// here and nowhere earlier, so every candidate above still gets a full sweep;
		// only the shot being rendered holds.
		if (shotSince != heldSince) {
			heldSince = shotSince;
			heldSweep = kUnheld;
			heldStandoff = kUnheld;
			heldRoom = kUnheld;
			heldFaceYaw = kUnheld;
			heldFacePitch = kUnheld;
		}
		subjects.delta = delta;
		subjects.heldSweep = heldSweep;
		subjects.heldStandoff = heldStandoff;
		subjects.heldRoom = heldRoom;
		subjects.heldFaceYaw = heldFaceYaw;
		subjects.heldFacePitch = heldFacePitch;

		// Hold Placement only applies once there's something held. heldRoom is cleared
		// on a shot's first frame, so that frame always runs the full sweep, wall
		// margin, crowd test and refusal. Candidate scoring above builds fresh
		// Subjects and isn't affected.
		subjects.holdPlacement = holdPlacement && heldRoom > kUnheld;

		// In scene mode the face offset is taken on the shot's first frame and held
		// until the next cut. People in scenes walk, kneel and turn, and following the
		// head through all of that looks like the camera being dragged around. The
		// root is still followed, so someone walking off isn't left behind.
		if (sceneMode) {
			auto& face = FavoursNpc(currentShot) ? subjects.npcFace : subjects.playerFace;
			if (faceOffsetShot != shotSince) {
				faceOffsetShot = shotSince;
				heldFaceOffset = face.offset;
			}
			face.offset = heldFaceOffset;
		}

		// The beat's push replaces the setup's own move, timed to the answer. Keyed to
		// the cut that made it, so a later cut to the same setup behaves normally.
		if (beatShot != ShotType::kCount && currentShot == beatShot && shotSince == beatShotSince) {
			subjects.moveOverride = Move::kPushIn;
			subjects.moveOverrideStrength = kBeatPush;
			subjects.progress = std::clamp(SecondsSince(shotSince) / std::max(beatPushSeconds, 0.1f), 0.0f, 1.0f);
		}

		auto pose = protectSubject ? smartFrame : Solve(currentShot, subjects);

		if (!pose.valid && !protectSubject) {
			const auto& remembered = FavoursNpc(currentShot) ? lastNpcPose : lastPlayerPose;
			if (ReusableShot(remembered.pose.valid, remembered.type, currentShot, Drawable)) {
				// Reuse this shot's own composition and lens, moved with its subject. A
				// different shot's offset isn't a valid fallback.
				const auto anchor = FavoursNpc(currentShot) ? npcAnchor.position : playerAnchor.position;
				const RE::NiPoint3 shift{ anchor.x - remembered.anchor.x,
					anchor.y - remembered.anchor.y, anchor.z - remembered.anchor.z };
				pose = remembered.pose;
				pose.position += shift;
				pose.lookAt += shift;
			} else if (SecondsSince(enabledRetryAt) >= 0.5f) {
				enabledRetryAt = Clock::now();
				if (const auto fallback = EnabledFallback(subjects); fallback && Drawable(fallback->first)) {
					Log::Info(Log::Category::kContinuity, "Enabled fallback: {} -> {}."sv,
						Name(currentShot), Name(fallback->first));
					previousShot = currentShot;
					currentShot = fallback->first;
					pose = fallback->second;
					shotSince = Clock::now();
					heldSince = shotSince;
					heldSweep = kUnheld;
					heldStandoff = kUnheld;
					heldRoom = kUnheld;
					heldFaceYaw = kUnheld;
					heldFacePitch = kUnheld;
				}
			}
		}

		if (!pose.valid || !Drawable(currentShot) || !std::isfinite(pose.sweep) ||
			(pose.sweep > kUnheld && !ShotAngles::AllowedAdjustment(pose.sweep))) {
			UseNativeView();
			return;
		}

		const bool poseOnNpc = FavoursNpc(currentShot);
		auto& slot = poseOnNpc ? lastNpcPose : lastPlayerPose;
		slot.pose = pose;
		slot.anchor = poseOnNpc ? npcAnchor.position : playerAnchor.position;
		slot.type = currentShot;
		if (pose.sweep > kUnheld) {
			if (heldSweep <= kUnheld && std::abs(pose.sweep) >= 0.05f) {
				Log::Info(Log::Category::kContinuity, "Placed {}: adjustment {:+.1f}deg."sv,
					Name(currentShot), pose.sweep);
			}
			heldSweep = pose.sweep;
		}
		if (heldFaceYaw <= kUnheld && pose.faceYaw > kUnheld && pose.facePitch > kUnheld) {
			heldFaceYaw = pose.faceYaw;
			heldFacePitch = pose.facePitch;
			const auto& face = FavoursNpc(currentShot) ? npcFaceTrack : playerFaceTrack;
			// Only log a face that's away from its stable point or off-axis; otherwise
			// it's every ordinary shot.
			const bool offPoint = std::abs(face.offset.x) >= 2.0f || std::abs(face.offset.y) >= 2.0f ||
			                      std::abs(face.offset.z) >= 2.0f;
			if (offPoint || std::abs(pose.faceYaw) >= 3.0f || std::abs(pose.facePitch) >= 3.0f) {
				Log::Info(Log::Category::kContinuity,
					"Framing the face for {}: head {:+.0f},{:+.0f},{:+.0f} off its stable point, "
					"bearing {:+.0f}deg, height {:+.0f}deg."sv,
					Name(currentShot), face.offset.x, face.offset.y, face.offset.z,
					pose.faceYaw, pose.facePitch);
			}
		}
		if (pose.standoff > kUnheld) {
			heldStandoff = pose.standoff;
		}
		if (heldRoom <= kUnheld && pose.room > kUnheld) {
			heldRoom = pose.room;
		}

		if (pose.valid && Drawable(currentShot)) {
			nativeView = false;
			ApplyPose(root, pose, delta, a_state);

			// Lens written every frame, because the engine and other mods also write
			// worldFOV and would revert a value set once. A shot with no lens of its own
			// gets baseFov, the player's own value.
			camera->GetRuntimeData2().worldFOV = pose.lens > 1.0f ? pose.lens : baseFov;

			// Per-angle look, only if enabled. Resolved per frame because a shot can
			// outlive the line that started it; the guard keeps it from re-sending an
			// unchanged look, which would restart the cross-fade every frame.
			if (lightPerShot) {
				const int look = Shot::LightOf(currentShot) >= 0 ?
									 Shot::LightOf(currentShot) :
									 Scene::FindLook(AuthoredLight(currentShot));

				if (look != lastLookApplied) {
					Scene::KeyLight::SetLook(look);
					lastLookApplied = look;
				}

				// Added to the global nudge rather than replacing it.
				Scene::KeyLight::SetOffset(
					lightOffsetX + Shot::LightOffsetX(currentShot),
					lightOffsetY + Shot::LightOffsetY(currentShot),
					lightOffsetZ + Shot::LightOffsetZ(currentShot));
			}

			// The look follows the camera's side of the eyeline, so the key stays on the
			// same side of the frame across a reverse. Uses this shot's own side, which
			// differs from `side` for the player's shots under true180.
			Scene::KeyLight::SetSide(SideFor(currentShot, subjects));

			// Light whoever the shot is on, not whoever is speaking; on a reaction shot
			// that's the listener.
			Scene::KeyLight::Aim(pose.position, pose.lookAt, delta);
		}
	}

	std::string_view FramingLabel(Framing a_framing) noexcept
	{
		switch (a_framing) {
		case Framing::kThem: return "them"sv;
		case Framing::kYou:  return "you"sv;
		case Framing::kRoom: return "the room"sv;
		default:             return "automatic"sv;
		}
	}

	// These set an atomic and return. They're called from the input thread and the
	// work happens on the next Tick.
	void Director::RequestCut() noexcept
	{
		requestCut.store(true, std::memory_order_relaxed);
	}

	void Director::RequestScene() noexcept
	{
		requestScene.store(true, std::memory_order_relaxed);
	}

	bool Director::FilmingScene() noexcept
	{
		return staging && sceneMode;
	}

	void Director::RequestFraming() noexcept
	{
		requestFraming.store(true, std::memory_order_relaxed);
	}

	Framing Director::CurrentFraming() noexcept
	{
		return framing;
	}

	bool Director::Directing() noexcept
	{
		return directing;
	}

	bool Director::DialogueMenuUp() noexcept
	{
		return dialogueMenuUp.load(std::memory_order_relaxed);
	}

	bool Director::Staging() noexcept
	{
		return staging;
	}
}

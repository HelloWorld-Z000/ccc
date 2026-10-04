#include "SD/Runtime.h"

#include "SD/Camera/Director.h"
#include "SD/Camera/Presets.h"
#include "SD/Compat/Conflicts.h"
#include "SD/Compat/DBReV.h"
#include "SD/Compat/ImprovedCamera.h"
#include "SD/Compat/SmoothCam.h"
#include "SD/Compat/VoiceCommand.h"
#include "SD/Core/Config.h"
#include "SD/Core/Hotkeys.h"
#include "SD/Core/Logging.h"
#include "SD/Core/Tick.h"
#include "SD/Dialogue/LineWatch.h"
#include "SD/Dialogue/MenuWatch.h"
#include "SD/Dialogue/SceneWatch.h"
#include "SD/Dialogue/Session.h"
#include "SD/Menu/Settings.h"
#include "SD/Render/Letterbox.h"
#include "SD/Scene/FaceGen.h"
#include "SD/Scene/KeepPosed.h"
#include "SD/Scene/LightRig.h"
#include "SD/Scene/LipSync.h"
#include "SD/Scene/Performance.h"
#include "SD/Scene/RegionalFace.h"
#include "SD/Scene/Interface.h"

#include <chrono>

namespace SD::Runtime
{
	namespace
	{
		std::atomic_bool ready{ false };
		Log::OnceFlag    firstFrame;
		Log::OnceFlag    waitingForLiveSpeaker;

		std::uint64_t windowFrames{ 0 };
		float         windowElapsed{ 0.0f };
		float         windowPeakDelta{ 0.0f };

		constexpr float kHeartbeatSeconds = 60.0f;


		// The partner this conversation was opened for, cleared only when the session
		// really ends. One open per conversation; retrying is what made the camera
		// flicker in and out after an early exit.
		RE::FormID openedFor{ 0 };

		// And which conversation with them: the form ID alone can't tell two
		// conversations with the same person apart.
		std::uint32_t openedSerial{ 0 };

		using Clock = std::chrono::steady_clock;

		// When the last re-stage of an already-staged conversation was attempted.
		// Limited to once a second so a refused Open() doesn't turn into a per-frame
		// loop.
		Clock::time_point lastRestage{};

		constexpr float kRestageRetrySeconds = 1.0f;

		// A dialogue menu up with only the previous line's speaker recorded, and since
		// when. See where it's staged.
		bool              tailMenuSeen{ false };
		Clock::time_point tailMenuSince{};
		constexpr float   kTailStageSeconds = 0.35f;

		[[nodiscard]] float SecondsSince(Clock::time_point a_when)
		{
			return std::chrono::duration<float>(Clock::now() - a_when).count();
		}

		bool ReadFlag(const char* a_section, const char* a_key, int a_default)
		{
			return Config::Bool(a_section, a_key, a_default != 0);
		}

		// A reopened menu can sit in its greeting while lastSpeaker still holds the
		// previous line. Wait for Session to see a live speaker; the menu being up
		// isn't enough to recover the old staging.
		[[nodiscard]] bool Stranded()
		{
			if (Camera::Director::Staging() || Camera::Director::Suspended() ||
				!Dialogue::Session::GetSingleton().PlayerEngaged()) {
				return false;
			}

			if (SecondsSince(lastRestage) < kRestageRetrySeconds) {
				return false;
			}

			// Asked of the UI directly rather than Director::DialogueMenuUp(), which is an
			// event latch; a stuck latch would restage once a second forever.
			auto* ui = RE::UI::GetSingleton();
			return ui && !ui->GameIsPaused() && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
		}
	}

	std::uint64_t FrameCount() noexcept
	{
		return Core::Tick::Total();
	}

	void Initialize()
	{
		Log::LogLoadedModule();

		// Before any conversation, so the first line of the session already identifies
		// its voice pack. See Compat::VoiceCommand.
		Compat::VoiceCommand::Install();

		// Line detection first, independent of the frame source. UpdateInDialogue is
		// driven by the engine and carries its own payload.
		Dialogue::LineWatch::Install();
		Dialogue::MenuWatch::Register();

		// Dialogue input belongs to the game: there's no input handler here. The topic
		// list fade is visual only, and a click during a fade does what it does in
		// vanilla.
		Log::Info(Log::Category::kDialogue,
			"Dialogue input untouched: no handler installed, no press intercepted."sv);

		Config::ReportSource();
		Compat::ReportKnownConflicts();

		// Improved Camera isn't a conflict, but it shares a camera state with this
		// mod; Detect decides how. See Compat::ImprovedCamera.
		Compat::ImprovedCamera::Detect();

		Compat::SmoothCam::Request();

		// Before the menu registers, so the panel shows the file's values rather than
		// built-in defaults.
		Camera::Director::LoadSettings();

		// Read the preset folder once up front so the log says what's installed.
		Camera::RescanPresetFiles(true);

		// A preset named in the ini, for setups without SKSE Menu Framework. Applied
		// after the settings load and cleared afterwards so it only runs once.
		Camera::ApplyPendingPreset();

		// Re-read, because ApplyPendingPreset writes the ini rather than the live
		// settings.
		Camera::Director::LoadSettings();

		// Safe when the framework is missing; it checks for the DLL and returns.
		Menu::Settings::Register();

		// Always installed; bLetterbox is a live setting. A retracted letterbox costs
		// one atomic read per frame.
		Render::Letterbox::Install();

		// Idle until the Director names someone: one atomic load per actor animation
		// update.
		Scene::KeepPosed::Install();

		// After LoadSettings (it reads bindings through the same resolver) and before
		// the ready flag, so a key pressed on the first frame works.
		Core::Hotkeys::Install();

		// bEnabled is a live setting (Tunables::enabled), read by LoadSettings above
		// and toggled from the settings panel.
		Log::Info(Log::Category::kCamera, "Directed shots are {}."sv,
			Camera::Director::Directing() ? "ENABLED"sv : "disabled ([Direction] bEnabled=0)"sv);

		// The FaceGen hook is installed for the face probe, iForceViseme or
		// synthesized lip sync (which writes through it). It patches a vtable every
		// head goes through, so it's only installed when one of those needs it.
		Scene::LipSync::Configure(
			ReadFlag("Performance", "bSynthLipSync", 1),
			Config::Int("Performance", "iLipSyncStrength", 55));

		Scene::Performance::Configure(ReadFlag("Performance", "bExpressions", 1), false);
		Scene::RegionalFace::SetEnabled(ReadFlag("Performance", "bRegionalExpressions", 1));

		const int forcedViseme = Config::Int("Diagnostics", "iForceViseme", -1);
		if (ReadFlag("Diagnostics", "bLogFaceAnim", 0) || forcedViseme >= 0 ||
			Scene::LipSync::Enabled() ||
			Scene::Performance::ExpressionsEnabled() || Config::Int("Diagnostics", "iForceExpression", -1) >= 0) {
			Scene::FaceGen::Install();
		}
		Scene::FaceGen::SetForcedViseme(forcedViseme);

		if (!Scene::FaceGen::Installed()) {
			if (forcedViseme >= 0) {
				Log::Warn(Log::Category::kCore,
					"iForceViseme is set but the morph hook did not install, so nothing will be written."sv);
			}
			if (Scene::LipSync::Enabled()) {
				Log::Warn(Log::Category::kCore,
					"bSynthLipSync is on but the morph hook did not install, so the mouth will not be driven."sv);
			}
		}

		if (ReadFlag("Diagnostics", "bFrameSource", 1)) {
			Core::Tick::Install();
		} else {
			Log::Info(Log::Category::kCore,
				"Frame source disabled by ini. Line starts will still be observed; line ends will not."sv);
		}

		ready.store(true, std::memory_order_release);
		Log::Info(Log::Category::kCore, "Ready — observing dialogue, not yet directing it."sv);
	}

	void OnFrame(RE::PlayerCamera*, float a_delta)
	{
		if (!ready.load(std::memory_order_acquire)) {
			return;
		}

		if (firstFrame.Take()) {
			Log::Info(Log::Category::kCore, "First frame observed via '{}'."sv,
				Core::Tick::Name(Core::Tick::Primary()));
		}

		++windowFrames;
		windowElapsed += a_delta;
		windowPeakDelta = std::max(windowPeakDelta, a_delta);

		if (windowElapsed >= kHeartbeatSeconds) {
			const float rate = windowElapsed > 0.0f ? static_cast<float>(windowFrames) / windowElapsed : 0.0f;

			// Every frame source candidate is reported, not just the one that won.
			Log::Info(Log::Category::kCore,
				"Tick heartbeat: {:.1f}/s over {:.1f}s (peak delta {:.3f}s) | PlayerCamera={} PlayerCharacter={} ThirdPersonState={}"sv,
				rate, windowElapsed, windowPeakDelta,
				Core::Tick::Count(Core::Source::kPlayerCamera),
				Core::Tick::Count(Core::Source::kPlayerCharacter),
				Core::Tick::Count(Core::Source::kThirdPersonState));

			windowFrames = 0;
			windowElapsed = 0.0f;
			windowPeakDelta = 0.0f;
		}

		auto& session = Dialogue::Session::GetSingleton();
		session.OnFrame(a_delta);

		// Release conditions are checked here rather than in the camera hook, so they
		// still run when the camera state has changed under the staging.
		Camera::Director::Tick(a_delta);

		// Staging follows the conversation, not the line, so SmoothCam isn't
		// renegotiated several times in one exchange.
		if (Camera::Director::Directing()) {
			const bool       active = session.Active();
			auto             partner = session.Partner().get();
			const RE::FormID partnerID = partner ? partner->GetFormID() : 0;
			const auto       serial = session.ConversationSerial();

			// Has this conversation been staged yet?
			const bool fresh = (partnerID != openedFor || serial != openedSerial);

			// It has, and the director let go of it anyway. See Stranded.
			const bool restaging = active && !fresh && Stranded();

			// A reopened menu with only the previous speaker recorded is staged anyway
			// after a short beat. Re-entering a conversation while the NPC finishes a line
			// can leave speaker empty for several seconds, and waiting meant no camera in
			// an open dialogue menu. The staging is marked provisional, and the live
			// conversation with the same person is adopted by it when it arrives. The beat
			// skips menus that open and close in a fraction of a second.
			bool stageTail = false;
			{
				auto*      ui = RE::UI::GetSingleton();
				const bool tailOnly = active && partner && !session.PlayerEngaged() &&
					!Camera::Director::Staging() && !Camera::Director::Suspended() &&
					!Dialogue::MenuWatch::ScreenTaken() &&
					ui && !ui->GameIsPaused() && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME);
				if (!tailOnly) {
					tailMenuSeen = false;
				} else if (!tailMenuSeen) {
					tailMenuSeen = true;
					tailMenuSince = Clock::now();
					if (waitingForLiveSpeaker.Take()) {
						Log::Info(Log::Category::kCamera,
							"Dialogue menu reopened with only the previous speaker [{:08X}]; "
							"staging it in {:.2f}s unless the live line arrives first."sv,
							partnerID, kTailStageSeconds);
					}
				} else {
					stageTail = SecondsSince(tailMenuSince) >= kTailStageSeconds &&
						SecondsSince(lastRestage) >= kRestageRetrySeconds;
				}
			}
			if (!active || session.PlayerEngaged()) {
				waitingForLiveSpeaker.Reset();
			}

			if (!active) {
				if (openedFor != 0) {
					// The conversation that was opened is over. A scene the player started filming
					// since then isn't it.
					if (!Camera::Director::FilmingScene()) {
						Camera::Director::Close();
					}
					openedFor = 0;
					openedSerial = 0;
				}

				// A suspension whose conversation is gone (the trade was the last thing the
				// topic did, the NPC walked off, a save was loaded). Dropped here so a later
				// conversation with the same actor can't inherit its angle or first-person
				// debt.
				if (Camera::Director::Suspended()) {
					Camera::Director::AbandonSuspension();
				}
			} else if (partner && (fresh || restaging || stageTail) && !Dialogue::MenuWatch::ScreenTaken() &&
				(session.PlayerEngaged() || Camera::Director::Suspended() || stageTail)) {
				// Not while a pausing menu has the screen: staging into a frozen game would
				// leave its changes (letterbox, HUD hide, topic list) stuck with nothing
				// running to undo them. In practice this only lasts a frame or two.
				//
				// Reasons to open: a new conversation (partner or session serial changed), a
				// conversation the director released while it's still live with a speaker
				// (Stranded), or a reopened menu with only the previous speaker (stageTail,
				// staged provisionally). Refused acquisitions retry at most once a second.

				// A suspension isn't owed a return to a conversation the player has left (the
				// session can stay alive on the NPC's trailing line). Marked as staged so this
				// is decided once instead of every frame.
				if (restaging || stageTail) {
					lastRestage = Clock::now();
					Log::Info(Log::Category::kCamera,
						"Conversation {} with [{:08X}] is still on screen and nothing was staging it; "
						"taking it back{}."sv,
						serial, partnerID, stageTail ? " before its live line"sv : ""sv);
				}

				if (Camera::Director::Suspended() && !session.PlayerEngaged()) {
					Camera::Director::AbandonSuspension();
				} else {
					Camera::Director::Open(partner->As<RE::Actor>(), restaging || stageTail, stageTail);
				}
				tailMenuSeen = false;

				openedFor = partnerID;
				openedSerial = serial;
			}
		}
	}

	void RearmConversation()
	{
		openedFor = 0;
		openedSerial = 0;
	}

	void OnGameLoaded()
	{
		// The save may have been written mid-conversation, in which case the topic
		// manager state belongs to a session this build never opened.
		Dialogue::Session::GetSingleton().Abandon();
		firstFrame.Reset();
		Log::Info(Log::Category::kCore, "Game loaded; session state re-keyed. {} ticks so far."sv, FrameCount());
	}

	void AbandonForLoad()
	{
		// Hand the camera back synchronously; anything deferred would run after the
		// load.
		Camera::Director::Close();
		Scene::Performance::ResetForLoad();
		Scene::LipSync::Release();
		Dialogue::SceneWatch::Reset();
		openedFor = 0;
		openedSerial = 0;

		// A resume armed in the outgoing world has nothing to come back to, and its
		// form ID may be someone else in the new save. No view hand-back: the saved
		// aim and zoom belong to the old world, and the camera state object survives a
		// load.
		Camera::Director::AbandonSuspension(false);

		Dialogue::Session::GetSingleton().Abandon();

		// A line left open across a load would hold the topic list in the new save.
		Compat::DBReV::Reset();

		// Same for a line SpeakSound announced that nothing consumed. The learned pack
		// is kept on purpose; see VoiceCommand::Reset.
		Compat::VoiceCommand::Reset();
	}
}

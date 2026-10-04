#include "SD/Dialogue/MenuWatch.h"

#include "SD/Camera/Director.h"
#include "SD/Core/Logging.h"
#include "SD/Core/Tick.h"
#include "SD/Render/Letterbox.h"
#include "SD/Runtime.h"

namespace SD::Dialogue
{
	namespace
	{
		std::uint64_t lastReportedFrame{ 0 };

		bool IsDialogueMenu(const RE::BSFixedString& a_name)
		{
			const char* raw = a_name.c_str();
			return raw && std::string_view{ raw } == RE::DialogueMenu::MENU_NAME;
		}

		// The screen-owning menus currently open, by name. Names rather than a count
		// because the flags can only be checked on open; on close the menu may already
		// be gone from the map, so whether it was counted has to be remembered. The
		// stack is never deep, so a vector and a scan are fine.
		std::vector<std::string> pausingMenus;

		// Published to the render thread, which can't ask this itself. The letterbox
		// is drawn from Present, which keeps running while the game thread is stopped,
		// so it needs to know whether a menu owns the screen. (GameIsPaused also
		// counts the Console and SKSE Menu Framework's own panel; see
		// Letterbox::SetScreenTaken.) Called from every place that changes the list.
		// The last published value, so the falling edge can be detected.
		bool screenWasTaken{ false };

		void PublishScreenTaken()
		{
			const bool taken = !pausingMenus.empty();
			Render::Letterbox::SetScreenTaken(taken);

			// The falling edge goes to the Director, which waits a moment after a menu
			// closes before sampling the camera again. Its own tick can't see the close
			// (the frame source stops while a pausing menu is up). Done here so the
			// Reconcile sweep publishes the same edge.
			if (screenWasTaken && !taken) {
				Camera::Director::OnScreenReleased();
			}
			screenWasTaken = taken;
		}

		// The one exception. The console pauses the game like other menus, but it's an
		// overlay over a conversation that hasn't moved, whereas menus opened by a
		// dialogue topic take over the conversation. Anything added here needs the
		// same argument.
		constexpr std::array kOverlayMenus{
			"Console"sv,
			"Console Native UI Menu"sv,
		};

		// Whether the menu now opening owns the screen. Two tests: kPausesGame
		// (inventory, containers, barter, gifts, magic, favourites, training, books,
		// map, journal), and kInventoryItemMenu, which catches CraftingMenu, the one
		// item menu that doesn't pause the game. Asked as flags so replacers and
		// mod-added menus are covered. The flag bits are read directly from menuFlags
		// because several IMenu accessors in this CommonLibSSE test the wrong
		// enumerator (PausesGame is correct).
		bool TakesScreen(const RE::BSFixedString& a_name)
		{
			const char* raw = a_name.c_str();
			if (raw && std::find(kOverlayMenus.begin(), kOverlayMenus.end(),
							 std::string_view{ raw }) != kOverlayMenus.end()) {
				return false;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			auto menu = ui->GetMenu(a_name);
			if (!menu) {
				return false;
			}

			return menu->PausesGame() ||
				menu->menuFlags.any(RE::UI_MENU_FLAGS::kInventoryItemMenu);
		}
	}

	bool MenuWatch::ScreenTaken() noexcept
	{
		return !pausingMenus.empty();
	}

	void MenuWatch::Reconcile()
	{
		if (pausingMenus.empty()) {
			return;
		}

		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}

		// Asked per menu rather than from the pause counter. A record built from
		// paired events can be left holding a menu whose close was never delivered,
		// which would block staging for good; but clearing everything when the game
		// unpauses breaks CraftingMenu, which doesn't pause. So each recorded menu is
		// checked against the UI directly.
		std::erase_if(pausingMenus, [ui](const std::string& a_name) {
			if (ui->IsMenuOpen(a_name)) {
				return false;
			}
			Log::Info(Log::Category::kDialogue,
				"'{}' is recorded as owning the screen but is no longer open; clearing."sv,
				a_name);
			return true;
		});

		PublishScreenTaken();
	}

	MenuWatch* MenuWatch::GetSingleton()
	{
		static MenuWatch instance;
		return &instance;
	}

	void MenuWatch::Register()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			Log::Error(Log::Category::kDialogue, "UI singleton unavailable; menu watch not registered."sv);
			return;
		}

		ui->AddEventSink<RE::MenuOpenCloseEvent>(GetSingleton());
		Log::Info(Log::Category::kDialogue, "Menu watch registered (engine-driven, independent of the frame hook)."sv);
	}

	RE::BSEventNotifyControl MenuWatch::ProcessEvent(
		const RE::MenuOpenCloseEvent*               a_event,
		RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_source)
	{
		(void)a_source;

		if (!a_event) {
			return RE::BSEventNotifyControl::kContinue;
		}

		if (!IsDialogueMenu(a_event->menuName)) {
			// Every other menu. This is the one callback that still runs while the game is
			// paused, so it's where menus opened by a dialogue topic (training, barter,
			// gifts, books) are noticed.
			const char* raw = a_event->menuName.c_str();
			if (!raw || !*raw) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const std::string name{ raw };
			const auto        known = std::find(pausingMenus.begin(), pausingMenus.end(), name);

			if (a_event->opening) {
				// Guard against a repeat open, which would otherwise be recorded twice and
				// never cleared.
				if (known == pausingMenus.end() && TakesScreen(a_event->menuName)) {
					pausingMenus.push_back(name);
					PublishScreenTaken();
					Camera::Director::OnScreenTaken(name);
				}
			} else if (known != pausingMenus.end()) {
				// Nothing to tell the Director: it released when the screen was taken, and
				// Runtime restages on the first tick after the game resumes.
				pausingMenus.erase(known);
				PublishScreenTaken();
			}

			return RE::BSEventNotifyControl::kContinue;
		}

		Camera::Director::OnDialogueMenu(a_event->opening);

		const auto frames = Runtime::FrameCount();
		const auto since = frames - lastReportedFrame;
		lastReportedFrame = frames;

		// Report what the polled state says at the moment the engine says the menu
		// changed, so any disagreement shows up in the log here.
		std::string_view speakerState = "no manager"sv;
		std::string_view topicState = "no manager"sv;
		if (auto* manager = RE::MenuTopicManager::GetSingleton()) {
			const bool haveSpeaker = static_cast<bool>(manager->speaker.get());
			const bool haveLast = static_cast<bool>(manager->lastSpeaker.get());
			speakerState = haveSpeaker ? "speaker"sv : (haveLast ? "lastSpeaker only"sv : "none"sv);
			topicState = manager->currentTopicInfo ? "talking"sv : "silent"sv;
		}

		Log::Info(Log::Category::kDialogue,
			"Dialogue Menu {} | {} ticks total (+{} since last menu event) | manager: {}, {}"sv,
			a_event->opening ? "OPEN "sv : "CLOSE"sv, frames, since, speakerState, topicState);

		// Only meaningful when the frame source is installed; otherwise a zero delta
		// is expected.
		if (since == 0 && Core::Tick::Installed()) {
			Log::Error(Log::Category::kDialogue,
				"Frame source did not advance between menu events — PlayerCamera::Update is not a per-frame tick."sv);
		}

		return RE::BSEventNotifyControl::kContinue;
	}
}

#include "SD/Scene/Interface.h"
#include "SD/Scene/DialogueDisplayOverride.h"

#include "SD/Core/Logging.h"

namespace SD::Scene
{
	namespace
	{
		bool suppressed{ false };

		// What this mod hid, at the two levels it hides from. Two lists because each
		// is restored through its own parent (the base clip, or _root).
		std::vector<std::string> hidden;      // children of HUDMovieBaseInstance
		std::vector<std::string> hiddenRoot;  // children of _root that are not it

		Log::OnceFlag            inventoryReported;

		// Once per conversation: Suppress() is retried every frame until it works.
		Log::OnceFlag            hudUnavailableReported;

		// Same for the topic list's hand-back, which is retried from the tick.
		Log::OnceFlag            releaseFailedReported;

		// A path inside DialogueMenu that's looked for until it's found, then
		// remembered. See Resolve for why it's retried.
		struct PathProbe
		{
			std::string path;
			int         attempts{ 0 };
			bool        settled{ false };
		};

		// Two seconds at 60 fps: long enough for the menu to finish building.
		constexpr int kProbeAttempts = 120;

		// Children of HUDMovieBaseInstance that stay visible; everything else is
		// hidden. These two are transient notifications rather than furniture, and
		// hiding the movie means they're never drawn at all:
		//
		//   MessagesBlock           - the notification stack (items received, gold,
		//                             skill increases, "you cannot carry any more").
		//   QuestUpdateBaseInstance - quest starts and objectives, level ups, and
		//                             learning a shout word.
		//
		// The names come from the hudmenu.swf files on this profile; HUD replacers
		// keep them because the engine drives them by name (see
		// RE::HUDObject::HudComponents).
		//
		// HUDMenu's SubtitleTextHolder isn't here: it carries ambient lines from other
		// NPCs. The conversation's own subtitle is drawn by DialogueMenu.
		constexpr std::array kReleased{
			"MessagesBlock"sv,
			"QuestUpdateBaseInstance"sv,
		};

		// Fallback names, for a movie the depth walk below can't read. The first block
		// is vanilla's roster (identical in SkyHUD, Edge UI and Edge UI Explorer
		// Addon); the second covers HUDs of some other lineage.
		constexpr std::array kKnownChildren{
			"TutorialLockInstance"sv,
			"LocationLockBase"sv,
			"CompassShoutMeterHolder"sv,
			"Crosshair"sv,
			"StealthMeterInstance"sv,
			"SubtitleTextHolder"sv,
			"RolloverName_mc"sv,
			"RolloverInfo_mc"sv,
			"GrayBarInstance"sv,
			"ActivateButton"sv,
			"WeightTranslated"sv,
			"ValueTranslated"sv,
			"FloatingQuestMarkerInstance"sv,
			"FavorBackButtonBase"sv,
			"Health"sv,
			"ChargeMeters"sv,
			"ChargeMeterBaseAlt"sv,
			"Magica"sv,  // spelled this way in the swf; not a typo here
			"Stamina"sv,
			"BottomLeftLockInstance"sv,
			"BottomRightLockInstance"sv,
			"ArrowInfoInstance"sv,
			"TimeDisplay"sv,
			"EnemyHealth_mc"sv,
			"TopLeftRefInstance"sv,
			"BottomRightRefInstance"sv,
			"ConfigWarning"sv,

			"CompassShoutMeterHolder_mc"sv,
			"Health_mc"sv,
			"Magicka_mc"sv,
			"Stamina_mc"sv,
			"LevelMeter_mc"sv,
			"ShoutMeter_mc"sv,
			"WeaponChargeMeters_mc"sv,
			"RolloverText_mc"sv,
			"RolloverButtonHolder_mc"sv,
			"FavorRolloverText_mc"sv,
			"ActivateButtonHolder_mc"sv,
			"ActivateButtonArt_mc"sv,
			"CrosshairInstance"sv,
			"CrosshairAlert"sv,
			"SneakAnim"sv,
			"SneakAnimInstance"sv,
			"TopMeters_mc"sv,
			"BottomBar_mc"sv,
			"LeftMeters_mc"sv,
			"RightMeters_mc"sv,
		};

		// Except while filming other NPCs: their lines are the HUD subtitle, and
		// there's no dialogue menu to carry them. See SetKeepSubtitles.
		bool keepSubtitles{ false };

		[[nodiscard]] bool Released(std::string_view a_name)
		{
			if (keepSubtitles && a_name == "SubtitleTextHolder"sv) {
				return true;
			}
			return std::find(kReleased.begin(), kReleased.end(), a_name) != kReleased.end();
		}

		// Menus that stay up. Every other menu open during a conversation is an
		// overlay to hide. Compass and HUD mods (Compass Navigation Overhaul, True
		// HUD) draw their own menus rather than living inside HUDMenu, and the menu
		// table can be enumerated, so they're handled here.
		constexpr std::array kKeepMenus{
			"Dialogue Menu"sv,
			"HUD Menu"sv,
			"Console"sv,
			"Console Native UI Menu"sv,
			"Cursor Menu"sv,
			"Fader Menu"sv,
			"Loading Menu"sv,
			"Main Menu"sv,
			"LoadWaitSpinner"sv,
			"Top Menu"sv,
			"Overlay Menu"sv,
			"Overlay Interaction Menu"sv,
		};

		std::vector<std::string> hiddenMenus;

		// Spared menus already named in the log, so each is logged once.
		std::vector<std::string> sparedReported;

		// Logged once per session.
		Log::OnceFlag            depthCapReported;

		// Foreign menus with one part the player should still see. Every child of
		// `parent` except `keep` is faded instead of hiding the whole movie. TrueHUD
		// draws all its widgets in one menu; Recent Loot is alone in
		// TrueHUD_PartialVisibilityWidgets, which TrueHUD keeps up during dialogue.
		struct PartialKeep
		{
			std::string_view menu;
			const char*      parent;
			std::string_view keep;
		};

		constexpr std::array kPartialKeeps{
			PartialKeep{ "TrueHUD"sv, "_root.TrueHUD", "TrueHUD_PartialVisibilityWidgets"sv },
		};

		struct FadedChild
		{
			std::string menu;
			std::string path;
			double      alpha;  // restored on release
		};

		std::vector<FadedChild>  fadedChildren;
		std::vector<std::string> partialMenus;

		// The opacity below which the list is taken out of hit testing. _visible=false
		// also stops rendering, so a high threshold makes the fade snap; at 1.5% the
		// list is effectively gone already.
		constexpr float kListDrawnAlpha = 1.5f;

		PathProbe choiceProbe;

		// Frames between foreign-menu sweeps: about four a second, which is plenty for
		// catching a newly opened menu.
		constexpr int kSweepFrames = 15;
		int           sweepCountdown{ 0 };

		// Rows inside the topic list, for the highlighted entry's text. A separate
		// probe from choiceProbe, which only runs when the fade is on.
		PathProbe                rowsProbe;

		// Vanilla BSScrollingList names, also present in Edge UI's dialoguemenu.swf.
		constexpr std::array kListPaths{
			"_root.DialogueMenu_mc.TopicListHolder.List_mc"sv,
			"_root.DialogueMenu_mc.TopicList.List_mc"sv,
			"_root.TopicListHolder.List_mc"sv,
			"_root.DialogueMenu_mc.List_mc"sv,
		};

		// Last resort: hide the HUD's root. Only used when Suppress can't read the
		// movie (no HUDMovieBaseInstance, or no children found by walk or name). It
		// costs the notifications, but leaves a clean frame. Separate from `hidden`
		// because it's restored differently.
		bool hudHidden{ false };

		// How many sweeps the fallback keeps retrying for the children: two seconds,
		// like kProbeAttempts.
		constexpr int kRecoverySweeps = 8;
		int           recoverySweeps{ 0 };

		// The speaker's name, printed beside the highlighted topic. It's a sibling of
		// TopicListHolder, so fading the list leaves it behind. Edge UI and Dragonborn
		// Voice Over keep the vanilla `SpeakerName` member and `SetSpeakerName`, so
		// the same path works for them.
		PathProbe   nameProbe;
		bool        hideSpeakerName{ true };

		// Resolve a display object inside DialogueMenu by probing a list of paths.
		// CommonLibSSE's ObjectVisitor has nothing that drives it, so members can't be
		// listed and named candidates are the only option.
		[[nodiscard]] bool ResolvePath(
			RE::GFxMovieView*                 a_view,
			std::span<const std::string_view> a_candidates,
			std::string&                      a_out,
			std::string_view                  a_what,
			bool                              a_logMiss)
		{
			std::string tried;
			for (const auto& candidate : a_candidates) {
				const std::string path{ candidate };
				RE::GFxValue      probe;
				if (a_view->GetVariable(&probe, path.c_str()) && probe.IsDisplayObject()) {
					a_out = path;
					Log::Info(Log::Category::kStaging, "{} found at {}."sv, a_what, a_out);
					return true;
				}
				if (!tried.empty()) {
					tried += ", ";
				}
				tried += path;
			}

			if (a_logMiss) {
				Log::Warn(Log::Category::kStaging, "No {} found. Tried: {}"sv, a_what, tried);
			}
			return false;
		}

		// True once the path is resolved and usable; false while still looking and
		// once the attempts run out. A miss is retried for a while because the engine
		// builds TopicListHolder a few frames after DialogueMenu opens, and the first
		// staged frame can land in that gap. The warning is logged on the last attempt
		// only.
		[[nodiscard]] bool Resolve(
			PathProbe&                        a_probe,
			RE::GFxMovieView*                 a_view,
			std::span<const std::string_view> a_candidates,
			std::string_view                  a_what)
		{
			if (!a_probe.settled) {
				const bool lastChance = ++a_probe.attempts >= kProbeAttempts;
				if (ResolvePath(a_view, a_candidates, a_probe.path, a_what, lastChance)) {
					a_probe.settled = true;
				} else if (lastChance) {
					a_probe.settled = true;
					a_probe.path.clear();
				}
			}

			return !a_probe.path.empty();
		}

		// The menu clip, which owns eMenuState and bAllowProgress. One level above the
		// topic list.
		constexpr std::array kMenuPaths{
			"_root.DialogueMenu_mc"sv,
			"_root.DialogueMenu"sv,
		};

		PathProbe menuProbe;

		constexpr std::array kNamePaths{
			"_root.DialogueMenu_mc.SpeakerName"sv,
			"_root.DialogueMenu_mc.SpeakerNameText"sv,
			"_root.DialogueMenu_mc.speakerName"sv,
			"_root.DialogueMenu_mc.NameText"sv,
			"_root.SpeakerName"sv,
		};

		// Keeps the movie alive until its GFxValue is released. Compares both, since
		// the same path can name a new clip.
		struct DialogueNode
		{
			RE::GPtr<RE::GFxMovieView> movie;
			RE::GFxValue value;

			bool operator==(const DialogueNode& a_other) const
			{
				return movie.get() == a_other.movie.get() && value == a_other.value;
			}
			bool ReadAlpha(double& a_out) const
			{
				RE::GFxValue actual;
				if (!value.IsDisplayObject() || !value.GetMember("_alpha", &actual) || !actual.IsNumber()) {
					return false;
				}
				a_out = actual.GetNumber();
				return true;
			}
			bool ReadVisible(bool& a_out) const
			{
				RE::GFxValue actual;
				if (!value.IsDisplayObject() || !value.GetMember("_visible", &actual) || !actual.IsBool()) {
					return false;
				}
				a_out = actual.GetBool();
				return true;
			}
			bool WriteAlpha(double a_alpha)
			{
				return value.IsDisplayObject() && value.SetMember("_alpha", RE::GFxValue{ a_alpha });
			}
			bool WriteVisible(bool a_visible)
			{
				return value.IsDisplayObject() && value.SetMember("_visible", RE::GFxValue{ a_visible });
			}
		};

		DialogueDisplayOverride<DialogueNode> choiceOverride;
		DialogueDisplayOverride<DialogueNode> nameOverride;
		RE::GPtr<RE::GFxMovieView> probedMovie;

		[[nodiscard]] RE::GPtr<RE::GFxMovieView> DialogueView()
		{
			auto* ui = RE::UI::GetSingleton();
			auto view = ui ? ui->GetMovieView(RE::DialogueMenu::MENU_NAME) : nullptr;
			if (view.get() != probedMovie.get()) {
				// Restore only objects retained from the outgoing movie, even if the UI no
				// longer exposes it. No debt or failed probe carries over to a new movie.
				choiceOverride.Reset();
				nameOverride.Reset();
				choiceProbe = {};
				nameProbe = {};
				rowsProbe = {};
				menuProbe = {};
				releaseFailedReported.Reset();
				probedMovie = view;
			}
			return view;
		}

		void SetNameAlpha(float a_alpha)
		{
			auto view = DialogueView();
			if (!view || !Resolve(nameProbe, view.get(), kNamePaths, "speaker name"sv)) {
				return;
			}
			RE::GFxValue node;
			if (view->GetVariable(&node, nameProbe.path.c_str()) && node.IsDisplayObject()) {
				nameOverride.Bind({ view, node });
				nameOverride.SetAlpha(static_cast<double>(std::clamp(a_alpha, 0.0f, 100.0f)));
			}
		}

		[[nodiscard]] bool SetMovieVisible(RE::IMenu* a_menu, bool a_visible, bool a_onlyIfShown)
		{
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			if (!view) {
				return false;
			}

			RE::GFxValue root;
			if (!view->GetVariable(&root, "_root") || !root.IsDisplayObject()) {
				return false;
			}

			if (a_onlyIfShown) {
				RE::GFxValue visible;
				if (root.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
					return false;  // already hidden by someone else
				}
			}

			root.SetMember("_visible", RE::GFxValue{ a_visible });
			return true;
		}

		// Logs everything still on screen after the pass.
		void LogRemainingMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}

			std::string remaining;
			for (auto& entry : ui->menuMap) {
				const char* raw = entry.first.c_str();
				if (!raw || !*raw || !entry.second.menu || !entry.second.menu->uiMovie) {
					continue;
				}

				RE::GFxValue root;
				RE::GFxValue visible;
				if (!entry.second.menu->uiMovie->GetVariable(&root, "_root") || !root.IsDisplayObject()) {
					continue;
				}
				if (root.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
					continue;
				}

				if (!remaining.empty()) {
					remaining += ", ";
				}
				remaining += raw;
			}

			Log::Info(Log::Category::kStaging, "Menus still visible: {}"sv,
				remaining.empty() ? "<none>"s : remaining);
		}

		// Can the player interact with this menu? If so, don't hide it.
		//
		// The sweep exists to clear passive widgets (compass replacers, stat bars,
		// durability readouts). A menu that pauses the game, is modal, wants the
		// cursor or takes the menu control context is something the player is expected
		// to answer, like a follower framework's confirmation box; hiding one leaves
		// the player with no way to respond. Erring toward keeping a menu just leaves
		// a widget on screen.
		//
		// The flag bits are read directly from menuFlags: several of IMenu's accessors
		// in this CommonLibSSE test the wrong enumerator. PausesGame is correct.
		[[nodiscard]] bool PlayerFacing(RE::IMenu* a_menu)
		{
			using Flag = RE::UI_MENU_FLAGS;
			return a_menu && a_menu->menuFlags.any(
									 Flag::kPausesGame,
									 Flag::kModal,
									 Flag::kUsesCursor,
									 Flag::kUsesMenuContext,
									 Flag::kUpdateUsesCursor,
									 Flag::kAssignCursorToRenderer);
		}

		[[nodiscard]] bool EnumerateChildren(RE::GFxValue& a_parent, std::vector<std::string>& a_out);

		[[nodiscard]] const PartialKeep* FindPartialKeep(std::string_view a_menu)
		{
			const auto it = std::find_if(kPartialKeeps.begin(), kPartialKeeps.end(),
				[&](const PartialKeep& a_keep) { return a_keep.menu == a_menu; });
			return it != kPartialKeeps.end() ? &*it : nullptr;
		}

		// Fades every child of a_keep.parent except a_keep.keep. Uses _alpha, not
		// _visible, because TrueHUD sets _visible on these containers itself on every
		// menu change. Returns false if the parent can't be found, so the caller can
		// hide the whole movie instead.
		[[nodiscard]] bool FadeAllBut(RE::IMenu* a_menu, std::string_view a_menuName, const PartialKeep& a_keep)
		{
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			RE::GFxValue parent;
			if (!view || !view->GetVariable(&parent, a_keep.parent) || !parent.IsDisplayObject()) {
				return false;
			}

			std::vector<std::string> children;
			if (!EnumerateChildren(parent, children)) {
				return false;
			}

			for (const auto& child : children) {
				if (child == a_keep.keep) {
					continue;
				}

				auto path = std::string{ a_keep.parent } + "." + child;
				RE::GFxValue node;
				if (!view->GetVariable(&node, path.c_str()) || !node.IsDisplayObject()) {
					continue;
				}

				const bool known = std::any_of(fadedChildren.begin(), fadedChildren.end(),
					[&](const FadedChild& a_faded) { return a_faded.menu == a_menuName && a_faded.path == path; });
				if (!known) {
					RE::GFxValue alpha;
					const double saved = node.GetMember("_alpha", &alpha) && alpha.IsNumber() ? alpha.GetNumber() : 100.0;
					fadedChildren.push_back({ std::string{ a_menuName }, std::move(path), saved });
				}
				node.SetMember("_alpha", RE::GFxValue{ 0.0 });
			}
			return true;
		}

		void SetFadedAlpha(bool a_restore)
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			for (const auto& faded : fadedChildren) {
				auto         menu = ui->GetMenu(faded.menu);
				auto         view = menu ? menu->uiMovie : nullptr;
				RE::GFxValue node;
				if (view && view->GetVariable(&node, faded.path.c_str()) && node.IsDisplayObject()) {
					node.SetMember("_alpha", RE::GFxValue{ a_restore ? faded.alpha : 0.0 });
				}
			}
		}

		void SuppressForeignMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}

			std::string list;
			std::string spared;
			for (auto& entry : ui->menuMap) {
				const char* raw = entry.first.c_str();
				if (!raw || !*raw) {
					continue;
				}
				const std::string name{ raw };

				if (std::find(kKeepMenus.begin(), kKeepMenus.end(), std::string_view{ name }) != kKeepMenus.end()) {
					continue;
				}

				if (PlayerFacing(entry.second.menu.get())) {
					// Logged only when it's on screen, so dormant registrations don't clutter the
					// log. A widget that survives a conversation shows up here.
					RE::GFxValue root;
					RE::GFxValue visible;
					auto         view = entry.second.menu ? entry.second.menu->uiMovie : nullptr;
					if (view && view->GetVariable(&root, "_root") && root.IsDisplayObject() &&
						(!root.GetMember("_visible", &visible) || !visible.IsBool() || visible.GetBool()) &&
						std::find(sparedReported.begin(), sparedReported.end(), name) == sparedReported.end()) {
						sparedReported.push_back(name);
						if (!spared.empty()) {
							spared += ", ";
						}
						spared += name;
					}
					continue;
				}

				// Already held either way; ReassertForeignMenus keeps it.
				const bool heldWhole = std::find(hiddenMenus.begin(), hiddenMenus.end(), name) != hiddenMenus.end();
				const bool heldPartly = std::find(partialMenus.begin(), partialMenus.end(), name) != partialMenus.end();
				if (heldPartly) {
					continue;
				}
				if (const auto* keep = FindPartialKeep(name); keep && !heldWhole &&
					FadeAllBut(entry.second.menu.get(), name, *keep)) {
					partialMenus.push_back(name);
					Log::Info(Log::Category::kStaging,
						"Foreign menu {}: kept {} visible, faded the rest."sv, name, keep->keep);
					continue;
				}

				if (!SetMovieVisible(entry.second.menu.get(), false, true)) {
					continue;
				}

				// A menu its owner shows again between sweeps comes back through here, so
				// don't add it twice.
				if (std::find(hiddenMenus.begin(), hiddenMenus.end(), name) == hiddenMenus.end()) {
					hiddenMenus.push_back(name);
				}

				if (!list.empty()) {
					list += ", ";
				}
				list += name;
			}

			if (!spared.empty()) {
				Log::Info(Log::Category::kStaging,
					"Left alone as player-facing: {}"sv, spared);
			}

			if (!list.empty()) {
				Log::Info(Log::Category::kStaging, "Foreign menus hidden: {}"sv, list);
			}
		}

		// Re-apply the hide on menus already taken. The sweep above skips anything
		// already hidden (by design), so this is what holds them.
		void ReassertForeignMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}

			for (const auto& name : hiddenMenus) {
				if (auto menu = ui->GetMenu(name)) {
					static_cast<void>(SetMovieVisible(menu.get(), false, false));
				}
			}

			// Catches the menu being rebuilt mid-conversation, which resets alpha.
			SetFadedAlpha(false);
		}

		void RestoreForeignMenus()
		{
			auto* ui = RE::UI::GetSingleton();
			if (ui) {
				for (const auto& name : hiddenMenus) {
					if (auto menu = ui->GetMenu(name)) {
						static_cast<void>(SetMovieVisible(menu.get(), true, false));
					}
				}
			}
			hiddenMenus.clear();

			SetFadedAlpha(true);
			fadedChildren.clear();
			partialMenus.clear();
		}

		[[nodiscard]] RE::GPtr<RE::IMenu> HudMenu()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
		}

		[[nodiscard]] bool AcquireBase(RE::GPtr<RE::IMenu>& a_menu, RE::GFxValue& a_base)
		{
			a_menu = HudMenu();
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			if (!view) {
				return false;
			}
			return view->GetVariable(&a_base, "_root.HUDMovieBaseInstance") && a_base.IsObject();
		}

		[[nodiscard]] bool AcquireRoot(RE::GPtr<RE::IMenu>& a_menu, RE::GFxValue& a_root)
		{
			a_menu = HudMenu();
			auto view = a_menu ? a_menu->uiMovie : nullptr;
			if (!view) {
				return false;
			}
			return view->GetVariable(&a_root, "_root") && a_root.IsDisplayObject();
		}

		// Timeline children live in a reserved depth band starting at -16384, in
		// PlaceObject order. Vanilla's hudmenu.swf places its 29 children at swf
		// depths 1 to 290, so a thousand covers it with plenty of margin.
		constexpr std::int32_t kTimelineBase = -16384;
		constexpr std::int32_t kTimelineSpan = 1024;

		// The other band: anything attachMovie'd at runtime, at zero and above.
		// Bounded because the extent comes from someone else's movie.
		constexpr std::int32_t kAttachedCap = 256;

		// Every child, found by asking the movie. Members can't be listed, but AS2's
		// getInstanceAtDepth can be called for each depth and the result's _name read,
		// which works for any HUD. Returns false only when the walk found nothing at
		// all, so the caller falls back.
		[[nodiscard]] bool EnumerateChildren(RE::GFxValue& a_parent, std::vector<std::string>& a_out)
		{
			const auto collect = [&](std::int32_t a_depth) {
				RE::GFxValue arg{ static_cast<double>(a_depth) };
				RE::GFxValue child;
				if (!a_parent.Invoke("getInstanceAtDepth", &child, &arg, 1) ||
					!child.IsDisplayObject()) {
					return;
				}

				RE::GFxValue name;
				if (!child.GetMember("_name", &name) || !name.IsString()) {
					return;
				}

				const char* raw = name.GetString();
				if (!raw || !*raw) {
					return;  // unnamed: nothing to hold it by, and nothing to restore
				}

				std::string named{ raw };
				if (std::find(a_out.begin(), a_out.end(), named) == a_out.end()) {
					a_out.push_back(std::move(named));
				}
			};

			const auto before = a_out.size();

			for (std::int32_t i = 0; i < kTimelineSpan; ++i) {
				collect(kTimelineBase + i);
			}

			// getNextHighestDepth returns zero when everything is on the timeline, so the
			// common case skips this band.
			RE::GFxValue next;
			if (a_parent.Invoke("getNextHighestDepth", &next, nullptr, 0) && next.IsNumber()) {
				const auto top = static_cast<std::int32_t>(next.GetNumber());
				const auto end = std::min(top, kAttachedCap);
				for (std::int32_t d = 0; d < end; ++d) {
					collect(d);
				}

				// Logged so it's clear a capped walk may have left something visible.
				if (top > kAttachedCap && depthCapReported.Take()) {
					Log::Warn(Log::Category::kStaging,
						"HUD depth walk stopped at {} of {} attached depths; anything above may stay visible."sv,
						kAttachedCap, top);
				}
			}

			return a_out.size() > before;
		}

		// Find the movie's children and hide everything not released. Returns false
		// when the movie couldn't be read at all; the two callers handle that
		// differently. a_report controls logging so the retry in Enforce is quiet.
		[[nodiscard]] bool ApplyKeepList(bool a_report)
		{
			RE::GPtr<RE::IMenu> menu;
			RE::GFxValue        base;
			if (!AcquireBase(menu, base)) {
				return false;
			}

			hidden.clear();
			hiddenRoot.clear();

			// Walk first, then names, and use the union. A name list that knows more
			// children than the movie has is normal; the names only matter if the walk
			// skipped something (Flash only promises MovieClips back from
			// getInstanceAtDepth).
			std::vector<std::string> children;
			const bool               walked = EnumerateChildren(base, children);

			for (const auto& known : kKnownChildren) {
				const std::string key{ known };
				if (std::find(children.begin(), children.end(), key) != children.end()) {
					continue;
				}

				RE::GFxValue member;
				if (base.GetMember(key.c_str(), &member) && member.IsDisplayObject()) {
					children.push_back(key);
				}
			}

			if (children.empty()) {
				return false;
			}

			std::string   kept;
			std::string   taken;
			std::uint32_t keptCount{ 0 };

			for (const auto& name : children) {
				if (Released(name)) {
					if (!kept.empty()) {
						kept += ", ";
					}
					kept += name;
					++keptCount;
					continue;
				}

				RE::GFxValue member;
				if (!base.GetMember(name.c_str(), &member) || !member.IsDisplayObject()) {
					continue;
				}

				// Skip anything already hidden, so restoring can't show an element the player
				// or another mod turned off.
				RE::GFxValue visible;
				if (member.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
					continue;
				}

				member.SetMember("_visible", RE::GFxValue{ false });
				hidden.push_back(name);

				if (!taken.empty()) {
					taken += ", ";
				}
				taken += name;
			}

			// One level up, for anything sharing _root with the base clip. Vanilla has
			// only HUDMovieBaseInstance there, so this normally hides nothing; it's for
			// mods that attach widgets beside the base clip.
			RE::GPtr<RE::IMenu> rootMenu;
			RE::GFxValue        root;
			if (AcquireRoot(rootMenu, root)) {
				std::vector<std::string> siblings;
				static_cast<void>(EnumerateChildren(root, siblings));

				for (const auto& name : siblings) {
					if (name == "HUDMovieBaseInstance" || Released(name)) {
						continue;
					}

					RE::GFxValue member;
					if (!root.GetMember(name.c_str(), &member) || !member.IsDisplayObject()) {
						continue;
					}

					RE::GFxValue visible;
					if (member.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) {
						continue;
					}

					member.SetMember("_visible", RE::GFxValue{ false });
					hiddenRoot.push_back(name);

					if (!taken.empty()) {
						taken += ", ";
					}
					taken += "_root." + name;
				}
			}

			if (a_report && inventoryReported.Take()) {
				Log::Info(Log::Category::kStaging, "HUD children hidden ({}): {}"sv,
					walked ? "depth walk"sv : "name probe"sv, taken.empty() ? "<nothing>"s : taken);
				Log::Info(Log::Category::kStaging, "HUD children released: {}"sv,
					kept.empty() ? "<nothing>"s : kept);

				// Log each released name this movie doesn't have, once. A missing released
				// element shows up as a notification quietly not appearing.
				for (const auto& release : kReleased) {
					const std::string key{ release };
					if (std::find(children.begin(), children.end(), key) == children.end()) {
						Log::Warn(Log::Category::kStaging,
							"Released element {} is not in this HUD; whatever it normally shows may be hidden with the rest."sv,
							key);
					}
				}
			}

			Log::Info(Log::Category::kStaging, "HUD suppressed; {} element(s) hidden, {} released."sv,
				hidden.size() + hiddenRoot.size(), keptCount);

			// Not re-walked on each sweep: timeline children are fixed and runtime
			// attachments happen when the movie is built. Menus opening mid-conversation
			// are caught by SuppressForeignMenus.
			//
			// True means the movie was read, not that anything was hidden; a HUD whose
			// elements were all already hidden is fine and shouldn't trigger the fallback.
			return true;
		}
	}

	void Interface::Suppress()
	{
		if (suppressed) {
			return;
		}

		// A full interval before the first re-sweep; this pass just walked the map.
		sweepCountdown = kSweepFrames;

		// Before the HUD, since the branch below can return early and the name lives
		// in DialogueMenu.
		if (hideSpeakerName) {
			SetNameAlpha(0.0f);
		}

		if (!HudMenu()) {
			// Returns without marking the conversation suppressed, so the caller retries
			// each frame until the HUD movie is available. Warned once per conversation;
			// RestoreHud resets it.
			if (hudUnavailableReported.Take()) {
				Log::Warn(Log::Category::kStaging,
					"HUD movie unavailable; retrying until it appears."sv);
			}
			return;
		}

		// If the movie can't be read, hide its root (losing notifications) rather than
		// leave the whole HUD in every shot. Enforce keeps retrying and restores the
		// notifications once it can read the children.
		if (!ApplyKeepList(true)) {
			if (auto hud = HudMenu(); hud && SetMovieVisible(hud.get(), false, true)) {
				hudHidden = true;
			}
			recoverySweeps = kRecoverySweeps;

			Log::Warn(Log::Category::kStaging,
				"No HUD children could be read; hiding the whole movie for now. "
				"Notifications and quest updates will not show while that holds."sv);
		}

		SuppressForeignMenus();
		LogRemainingMenus();

		suppressed = true;
	}

	void Interface::Enforce()
	{
		if (!suppressed) {
			return;
		}

		// The root hide, reapplied every frame with onlyIfShown off: it's already
		// hidden by us, so the guard would stop it holding.
		if (hudHidden) {
			static_cast<void>(SetMovieVisible(HudMenu().get(), false, false));
		}

		// Same for the children. Only elements this mod hid are in these lists, so
		// nothing that was already off gets touched and released elements are never
		// reached.
		if (!hidden.empty()) {
			RE::GPtr<RE::IMenu> menu;
			RE::GFxValue        base;
			if (AcquireBase(menu, base)) {
				for (const auto& name : hidden) {
					RE::GFxValue member;
					if (base.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
						member.SetMember("_visible", RE::GFxValue{ false });
					}
				}
			}
		}

		if (!hiddenRoot.empty()) {
			RE::GPtr<RE::IMenu> menu;
			RE::GFxValue        root;
			if (AcquireRoot(menu, root)) {
				for (const auto& name : hiddenRoot) {
					RE::GFxValue member;
					if (root.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
						member.SetMember("_visible", RE::GFxValue{ false });
					}
				}
			}
		}

		// Menus are the throttled part; walking ui->menuMap scales with what's
		// installed.
		if (--sweepCountdown > 0) {
			return;
		}
		sweepCountdown = kSweepFrames;

		SuppressForeignMenus();   // newcomers
		ReassertForeignMenus();   // what we already hold

		// Keep retrying the children while the root hide is in place. HUDMenu may not
		// be ready on the frame a conversation stages (after a cell load, on arrival
		// from fast travel, or while a HUD replacer rebuilds), and the root comes back
		// with the notifications as soon as it succeeds. Bounded to two seconds. Gated
		// on the countdown rather than hudHidden, because the case that most needs a
		// retry (no movie to hide yet) leaves hudHidden false.
		if (recoverySweeps > 0) {
			--recoverySweeps;

			if (ApplyKeepList(false)) {
				if (hudHidden) {
					hudHidden = false;
					static_cast<void>(SetMovieVisible(HudMenu().get(), true, false));
				}
				recoverySweeps = 0;
				Log::Info(Log::Category::kStaging,
					"HUD children turned up late; the keep-list is on and notifications with it."sv);
			}
		}
	}

	bool Interface::SetChoiceAlpha(float a_alpha)
	{
		auto view = DialogueView();
		if (!view) {
			return false;
		}
		SetNameAlpha(hideSpeakerName ? 0.0f : a_alpha);

		// Only a positively identified topic holder may be faded. Never the menu root,
		// subtitles, list rows, or the movie's input/progress gates.
		constexpr std::array kTopicPaths{
			"_root.DialogueMenu_mc.TopicListHolder"sv,
			"_root.DialogueMenu_mc.TopicList"sv,
			"_root.DialogueMenu_mc.topicList"sv,
			"_root.TopicListHolder"sv,
			"_root.TopicList"sv,
		};
		if (!Resolve(choiceProbe, view.get(), kTopicPaths, "topic list"sv)) {
			return false;
		}
		RE::GFxValue node;
		if (!view->GetVariable(&node, choiceProbe.path.c_str()) || !node.IsDisplayObject()) {
			return false;
		}
		choiceOverride.Bind({ view, node });
		const float wanted = std::clamp(a_alpha, 0.0f, 100.0f);
		// Ending a fade releases only our writes. A new menu's initial alpha and
		// visibility belong to its own construction and transitions.
		const bool alphaDone = choiceOverride.SetAlpha(static_cast<double>(wanted));
		const bool visibleDone = choiceOverride.SetHidden(wanted < kListDrawnAlpha);
		return alphaDone && visibleDone;
	}

	std::string_view Interface::Name(MenuPhase a_phase)
	{
		switch (a_phase) {
		case MenuPhase::kGreeting:      return "greeting"sv;
		case MenuPhase::kTopicList:     return "topic list"sv;
		case MenuPhase::kTopicClicked:  return "topic clicked"sv;
		case MenuPhase::kTransitioning: return "transitioning"sv;
		default:                        return "unknown"sv;
		}
	}

	std::string Interface::ReadSelectedTopic()
	{
		auto view = DialogueView();
		if (!view) {
			return {};
		}

		if (!Resolve(rowsProbe, view.get(), kListPaths, "topic list rows"sv)) {
			return {};
		}

		RE::GFxValue list;
		if (!view->GetVariable(&list, rowsProbe.path.c_str()) || !list.IsObject()) {
			return {};
		}

		// selectedEntry first: it's what the highlighted row actually holds.
		// entryList[selectedIndex] is the fallback.
		RE::GFxValue entry;
		if (list.GetMember("selectedEntry", &entry) && entry.IsObject()) {
			RE::GFxValue text;
			if (entry.GetMember("text", &text) && text.IsString()) {
				return text.GetString();
			}
		}

		RE::GFxValue index;
		RE::GFxValue entries;
		if (list.GetMember("selectedIndex", &index) && index.IsNumber() &&
			list.GetMember("entryList", &entries) && entries.IsArray()) {
			const auto i = static_cast<std::uint32_t>(std::max(0.0, index.GetNumber()));
			RE::GFxValue row;
			if (i < entries.GetArraySize() && entries.GetElement(i, &row) && row.IsObject()) {
				RE::GFxValue text;
				if (row.GetMember("text", &text) && text.IsString()) {
					return text.GetString();
				}
			}
		}

		return {};
	}

	std::uint64_t Interface::ReadTopicListFingerprint()
	{
		auto view = DialogueView();
		if (!view) {
			return 0;
		}

		if (!Resolve(rowsProbe, view.get(), kListPaths, "topic list rows"sv)) {
			return 0;
		}

		RE::GFxValue list;
		if (!view->GetVariable(&list, rowsProbe.path.c_str()) || !list.IsObject()) {
			return 0;
		}

		RE::GFxValue entries;
		if (!list.GetMember("entryList", &entries) || !entries.IsArray()) {
			return 0;
		}

		const auto count = entries.GetArraySize();
		if (count == 0) {
			return 0;
		}

		// FNV-1a over the row texts plus the row count. Only used to answer "same rows
		// as a moment ago?".
		std::uint64_t hash = 14695981039346656037ull;
		const auto    mix = [&hash](std::uint8_t a_byte) {
			hash ^= a_byte;
			hash *= 1099511628211ull;
		};

		mix(static_cast<std::uint8_t>(count));
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::GFxValue row;
			RE::GFxValue text;
			if (entries.GetElement(i, &row) && row.IsObject() &&
				row.GetMember("text", &text) && text.IsString()) {
				for (const char* c = text.GetString(); c && *c; ++c) {
					mix(static_cast<std::uint8_t>(*c));
				}
			}
			mix(0x1F);  // row separator, so ["ab","c"] and ["a","bc"] differ
		}

		// 0 means "couldn't be read", so a real hash of 0 becomes 1.
		return hash ? hash : 1;
	}

	Interface::DialoguePhase Interface::ReadDialoguePhase()
	{
		DialoguePhase out{};

		auto view = DialogueView();
		if (!view) {
			return out;  // no movie: not valid, caller falls back
		}

		if (!Resolve(menuProbe, view.get(), kMenuPaths, "dialogue menu"sv)) {
			return out;
		}

		RE::GFxValue node;
		if (!view->GetVariable(&node, menuProbe.path.c_str()) || !node.IsDisplayObject()) {
			return out;
		}

		// Read both members independently; voice readiness alone doesn't say whether
		// the movie has a supported topic-list state.
		std::optional<double> menuState;
		RE::GFxValue state;
		if (node.GetMember("eMenuState", &state) && state.IsNumber()) {
			menuState = state.GetNumber();
		}

		std::optional<bool> allowProgress;
		RE::GFxValue allow;
		if (node.GetMember("bAllowProgress", &allow) && allow.IsBool()) {
			allowProgress = allow.GetBool();
		}

		return DialoguePhase::FromMovie(menuState, allowProgress);
	}

	void Interface::SetKeepSubtitles(bool a_keep)
	{
		keepSubtitles = a_keep;
	}

	void Interface::SetHideSpeakerName(bool a_hide)
	{
		if (hideSpeakerName == a_hide) {
			return;
		}
		hideSpeakerName = a_hide;

		// Switching it off has to restore the name right away; otherwise nothing
		// writes it again until the conversation ends.
		if (!a_hide) {
			SetNameAlpha(100.0f);
		}
	}

	void Interface::RestoreHud()
	{
		// Re-armed for the next conversation on every close.
		hudUnavailableReported.Reset();

		if (!suppressed) {
			return;
		}
		suppressed = false;
		recoverySweeps = 0;
		RestoreForeignMenus();

		if (hudHidden) {
			hudHidden = false;
			auto* ui = RE::UI::GetSingleton();
			if (auto hud = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr) {
				static_cast<void>(SetMovieVisible(hud.get(), true, false));
			}
		}

		RE::GPtr<RE::IMenu> rootMenu;
		RE::GFxValue        root;
		if (AcquireRoot(rootMenu, root)) {
			for (const auto& name : hiddenRoot) {
				RE::GFxValue member;
				if (root.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
					member.SetMember("_visible", RE::GFxValue{ true });
				}
			}
		}
		hiddenRoot.clear();

		RE::GPtr<RE::IMenu> menu;
		RE::GFxValue        base;
		if (!AcquireBase(menu, base)) {
			hidden.clear();
			return;
		}

		for (const auto& name : hidden) {
			RE::GFxValue member;
			if (base.GetMember(name.c_str(), &member) && member.IsDisplayObject()) {
				member.SetMember("_visible", RE::GFxValue{ true });
			}
		}

		hidden.clear();
	}

	void Interface::ReleaseChoices()
	{
		// Check for a replaced movie before retrying. Restoration uses retained object
		// handles, never a path in whatever movie is up now.
		static_cast<void>(DialogueView());
		const bool choicesDone = choiceOverride.Release();
		const bool nameDone = nameOverride.Release();
		if (!choicesDone || !nameDone) {
			if (releaseFailedReported.Take()) {
				Log::Warn(Log::Category::kStaging,
					"Dialogue UI restore pending on its original display object; retrying."sv);
			}
			return;
		}
		releaseFailedReported.Reset();
	}

	void Interface::Restore()
	{
		// Both halves, always. Each is idempotent, so Close() doesn't need to know
		// which features were on.
		RestoreHud();
		ReleaseChoices();
	}
}

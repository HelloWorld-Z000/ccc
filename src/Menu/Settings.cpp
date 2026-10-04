#include "SD/Menu/Settings.h"

#include "SD/Camera/Director.h"
#include "SD/Camera/Presets.h"
#include "SD/Camera/Shot.h"
#include "SD/Core/Config.h"
#include "SD/Core/Hotkeys.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/Interface.h"
#include "SD/Scene/KeyLight.h"
#include "SD/Scene/LightRig.h"
#include "SD/Scene/LipSync.h"
#include "SD/Scene/Performance.h"
#include "SD/Scene/RegionalFace.h"

#include <chrono>

// Third-party header that doesn't build at /W4 /WX, so its warnings are
// suppressed here.
#pragma warning(push, 0)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace SD::Menu
{
	namespace
	{
		namespace MF = SKSEMenuFramework;
		namespace Im = ImGuiMCP;

		// --- Palette -------------------------------------------------------------
		//
		// The packed ImU32 overload of PushStyleColor takes 0xAABBGGRR (alpha, blue,
		// green, red), not RGBA: write the RGB you want, then reverse the three bytes.
		// The ImGuiCol_ constants aren't reachable here, so the indices are written
		// out, matching the enum in SKSEMenuFramework.h (Text is 0, CheckMark is 18).
		constexpr int kColText = 0;
		constexpr int kColCheck = 18;
		constexpr int kColButton = 21;
		constexpr int kColButtonHovered = 22;
		constexpr int kColButtonActive = 23;
		constexpr int kColHeader = 24;
		constexpr int kColHeaderHovered = 25;
		constexpr int kColHeaderActive = 26;
		constexpr int kColSeparator = 27;
		constexpr int kColTableHeaderBg = 46;

		// Blue is the accent color, used for identity only: the current page, the rule
		// under a heading, the main action on a panel.
		constexpr std::uint32_t kBlue = 0xFFFF9C40u;      // RGB(64,156,255)  accent
		constexpr std::uint32_t kBlueLine = 0xFFC87A32u;  // RGB(50,122,200)  rules
		constexpr std::uint32_t kBlueDeep = 0xFF5A3C1Eu;  // RGB(30,60,90)    fills

		constexpr std::uint32_t kTextOn = 0xFFFFFFFFu;
		constexpr std::uint32_t kTextOff = 0xFF7A7A7Au;
		constexpr std::uint32_t kMuted = 0xFF8A8A8Au;
		constexpr std::uint32_t kGreen = 0xFF66DD66u;
		constexpr std::uint32_t kAmber = 0xFF3BA0F0u;

		// --- Icons ---------------------------------------------------------------
		//
		// SKSE Menu Framework provides Font Awesome as a separate font
		// (PushSolid/PushRegular/PushBrands) rather than glyphs merged into the text
		// atlas, so an icon and a word can't share one string. Each icon is its own
		// item between a push and a pop, with SameLine for the text after it.
		//
		// The codepoints are all in the f000-f2ff block and date from Font Awesome 4,
		// so they exist in whichever version the framework ships. Every icon sits next
		// to a word anyway, so a missing glyph never hides what a control does.
		constexpr unsigned kIconCamera = 0xf03du;    // video
		constexpr unsigned kIconShots = 0xf008u;     // film
		constexpr unsigned kIconScreen = 0xf26cu;    // tv
		constexpr unsigned kIconFaces = 0xf118u;     // face
		constexpr unsigned kIconPresets = 0xf1deu;   // sliders
		constexpr unsigned kIconKeys = 0xf11cu;      // keyboard
		constexpr unsigned kIconAbout = 0xf05au;     // circle-info
		constexpr unsigned kIconCheck = 0xf00cu;     // check
		constexpr unsigned kIconWarning = 0xf071u;   // triangle-exclamation
		constexpr unsigned kIconReset = 0xf0e2u;     // arrow-rotate-left
		constexpr unsigned kIconSave = 0xf0c7u;      // floppy-disk
		constexpr unsigned kIconRename = 0xf044u;    // pen-to-square
		constexpr unsigned kIconDelete = 0xf1f8u;    // trash
		constexpr unsigned kIconTimer = 0xf017u;     // clock
		constexpr unsigned kIconLine = 0xf075u;      // comment
		constexpr unsigned kIconHold = 0xf04bu;      // play

		// Cached: UnicodeToUtf8 builds a converter and a string per call, and this
		// runs per icon per frame.
		[[nodiscard]] const char* Glyph(unsigned a_codepoint)
		{
			static std::unordered_map<unsigned, std::string> cache;
			auto                                            it = cache.find(a_codepoint);
			if (it == cache.end()) {
				it = cache.emplace(a_codepoint, FontAwesome::UnicodeToUtf8(a_codepoint)).first;
			}
			return it->second.c_str();
		}

		// One icon in one color, in a fixed-width slot, leaving the cursor on the same
		// line for the text. Font Awesome glyphs aren't monospaced, so without the
		// slot every heading's text would start at a different x. Measured in em
		// because the panel's font scale varies (see Compact). The font push is
		// balanced in the same statement, and CalcTextSize runs inside it so it
		// measures the icon font.
		void Icon(unsigned a_codepoint, std::uint32_t a_colour = kBlue)
		{
			const float slot = Im::GetFontSize() * 1.35f;

			FontAwesome::PushSolid();
			const float width = Im::CalcTextSize(Glyph(a_codepoint)).x;
			Im::PushStyleColor(kColText, a_colour);
			Im::TextUnformatted(Glyph(a_codepoint));
			Im::PopStyleColor();
			FontAwesome::Pop();

			// Clamped at zero, so a glyph wider than the slot costs alignment rather than
			// overlapping the text.
			const float gap = Im::GetStyle()->ItemSpacing.x;
			Im::SameLine(0.0f, std::max(slot - width, 0.0f) + gap);
		}

		// --- Density -------------------------------------------------------------
		//
		// The framework's default font size and padding only fit about seven rows of
		// the Shots page on screen. This scales the font and tightens spacing for the
		// duration of a page. Scoped and undone in a destructor because every page has
		// early returns, and a leftover font scale would carry into other mods'
		// sections.
		constexpr float kFontScale = 0.85f;

		struct Compact
		{
			Compact()
			{
				Im::SetWindowFontScale(kFontScale);

				// The density gain is vertical. Horizontal spacing stays roughly as is; the
				// name column on the Shots page is already the tight one.
				Im::PushStyleVar(ImGuiMCP::ImGuiStyleVar_ItemSpacing, Im::ImVec2(6.0f, 3.0f));
				Im::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, Im::ImVec2(4.0f, 2.0f));
				Im::PushStyleVar(ImGuiMCP::ImGuiStyleVar_CellPadding, Im::ImVec2(6.0f, 2.0f));
			}

			~Compact()
			{
				Im::PopStyleVar(3);
				Im::SetWindowFontScale(1.0f);
			}

			Compact(const Compact&) = delete;
			Compact(Compact&&) = delete;
			Compact& operator=(const Compact&) = delete;
			Compact& operator=(Compact&&) = delete;
		};

		// There's no bold font available, so emphasis is color: blue for headings,
		// white for something live, grey for something that isn't. (Faking bold by
		// drawing twice a pixel apart just looks doubled at this scale.)
		void Text(std::uint32_t a_colour, const char* a_text)
		{
			Im::PushStyleColor(kColText, a_colour);
			Im::TextUnformatted(a_text);
			Im::PopStyleColor();
		}

		// The line at the top of a page: icon, name and an optional grey note.
		void PageHeader(unsigned a_icon, const char* a_title, const char* a_note = nullptr)
		{
			Icon(a_icon, kBlue);
			Text(kBlue, a_title);

			if (a_note && *a_note) {
				Im::SameLine();
				Text(kMuted, a_note);
			}

			Im::PushStyleColor(kColSeparator, kBlueLine);
			Im::Separator();
			Im::PopStyleColor();
			Im::Spacing();
		}

		// A collapsible group heading with an icon. The ID is split off with ### so a
		// heading whose text changes keeps its identity (and its open state).
		[[nodiscard]] bool Group(unsigned a_icon, const char* a_title, const char* a_id,
			bool a_open = true)
		{
			Icon(a_icon, kBlue);

			char label[160]{};
			std::snprintf(label, sizeof(label), "%s###%s", a_title, a_id);

			Im::PushStyleColor(kColHeader, kBlueDeep);
			Im::PushStyleColor(kColHeaderHovered, kBlueLine);
			Im::PushStyleColor(kColHeaderActive, kBlue);
			const bool open = Im::CollapsingHeader(label,
				a_open ? ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen : 0);
			Im::PopStyleColor(3);
			return open;
		}

		// --- Row grid ------------------------------------------------------------
		//
		// Every control on every page is one row of a two-column table: the name on
		// the left, the control on the right. A table can't be overrun by its
		// neighbour, reflows with the panel and survives UI scaling, unlike
		// hand-picked pixel offsets. The control column takes whatever width is left.
		[[nodiscard]] bool BeginRows(const char* a_id)
		{
			constexpr auto kFlags = ImGuiMCP::ImGuiTableFlags_SizingStretchProp |
			                        ImGuiMCP::ImGuiTableFlags_NoSavedSettings |
			                        ImGuiMCP::ImGuiTableFlags_PadOuterX;

			if (!Im::BeginTable(a_id, 2, kFlags)) {
				return false;
			}

			// 40/60: wide enough for the longest name ("Return to First Person
			// Afterwards") on one line without starving the sliders.
			Im::TableSetupColumn("name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.40f, 1);
			Im::TableSetupColumn("value", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch, 0.60f, 2);
			return true;
		}

		void EndRows()
		{
			Im::EndTable();
			Im::Spacing();
		}

		// Opens a row: the name in the left column, the cursor in the right one with
		// the item width set to fill it. AlignTextToFramePadding puts the label on the
		// control's center line.
		void Row(const char* a_label, bool a_stretch = true)
		{
			Im::TableNextRow();
			Im::TableNextColumn();
			Im::AlignTextToFramePadding();
			Im::TextUnformatted(a_label);
			Im::TableNextColumn();
			if (a_stretch) {
				Im::SetNextItemWidth(Im::GetContentRegionAvail().x);
			}
		}

		// The (?) marker. Submitted after the control has been queried; see SliderRow
		// for why that order matters.
		void Help(const char* a_help)
		{
			if (!a_help || !*a_help) {
				return;
			}
			Im::SameLine();
			Im::TextDisabled("(?)");
			if (Im::IsItemHovered(ImGuiMCP::ImGuiHoveredFlags_AllowWhenDisabled)) {
				Im::BeginTooltip();
				Im::PushTextWrapPos(Im::GetFontSize() * 30.0f);
				Im::TextUnformatted(a_help);
				Im::PopTextWrapPos();
				Im::EndTooltip();
			}
		}

		// A slider row that draws, saves to SD_user.ini and applies live in one place.
		// The visible label is drawn by Row(), so the widget gets a hidden ID built
		// from the ini key, which is unique across the file.
		bool SliderRow(const char* a_label, int& a_value, int a_min, int a_max,
			const char* a_section, const char* a_key, const char* a_format = "%d",
			const char* a_help = nullptr)
		{
			Row(a_label);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			const int before = a_value;
			Im::SliderInt(id, &a_value, a_min, a_max, a_format);

			// Query the item here, before anything else is submitted: IsItem* refers to
			// the most recent item, and the help marker below submits one.
			// Deactivated-after-edit fires once when a drag ends, which is when to write
			// the file.
			const bool released = Im::IsItemDeactivatedAfterEdit();
			const bool active = Im::IsItemActive();
			const bool moved = a_value != before;

			Help(a_help);

			// Release is the main trigger. The second clause catches changes from
			// ctrl-click entry or the keyboard, which don't always produce a deactivation
			// edge. Neither fires every frame during a drag.
			if (released || (moved && !active)) {
				Config::SetInt(a_section, a_key, a_value);
			}

			// Live application uses the per-frame change, so dragging is felt immediately.
			return moved;
		}

		// A time slider that shows seconds in the track. Stored as hundredths like
		// every other timing key; the conversion happens here.
		bool SecondsRow(const char* a_label, int& a_hundredths, int a_min, int a_max,
			const char* a_section, const char* a_key, const char* a_help = nullptr)
		{
			Row(a_label);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			float seconds = static_cast<float>(a_hundredths) / 100.0f;
			Im::SliderFloat(id, &seconds, static_cast<float>(a_min) / 100.0f,
				static_cast<float>(a_max) / 100.0f, "%.2f s",
				ImGuiMCP::ImGuiSliderFlags_AlwaysClamp);

			const bool released = Im::IsItemDeactivatedAfterEdit();
			const bool active = Im::IsItemActive();

			// Rounded, not truncated, so 8.00 stores 800 rather than 799 (which preset
			// comparison would report as drift).
			const int rounded = std::clamp(
				static_cast<int>(std::lround(static_cast<double>(seconds) * 100.0)), a_min, a_max);
			const bool moved = rounded != a_hundredths;
			a_hundredths = rounded;

			Help(a_help);

			if (released || (moved && !active)) {
				Config::SetInt(a_section, a_key, a_hundredths);
			}
			return moved;
		}

		// A percent slider over a value stored in thousandths of screen height (the
		// letterbox: "115" means 11.5% per bar). Whole percents only; an older
		// fractional value snaps to the nearest percent when first touched.
		bool PercentRow(const char* a_label, int& a_thousandths, int a_maxPercent,
			const char* a_section, const char* a_key)
		{
			Row(a_label);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			int percent = (a_thousandths + 5) / 10;
			Im::SliderInt(id, &percent, 0, a_maxPercent, "%d%%");

			const bool released = Im::IsItemDeactivatedAfterEdit();
			const bool active = Im::IsItemActive();
			const int  wanted = percent * 10;
			const bool moved = wanted != a_thousandths;
			a_thousandths = wanted;

			if (released || (moved && !active)) {
				Config::SetInt(a_section, a_key, a_thousandths);
			}
			return moved;
		}

		// A toggle row. The box sits at the left of the right column, where every
		// other control starts.
		bool ToggleRow(const char* a_label, bool& a_value, const char* a_section,
			const char* a_key, const char* a_help = nullptr)
		{
			Row(a_label, false);

			char id[96]{};
			std::snprintf(id, sizeof(id), "###%s", a_key);

			bool changed = false;
			Im::PushStyleColor(kColCheck, kGreen);
			if (Im::Checkbox(id, &a_value)) {
				Config::SetBool(a_section, a_key, a_value);
				changed = true;
			}
			Im::PopStyleColor();

			Help(a_help);
			return changed;
		}

		// An amber warning with an icon, wrapped.
		void Warning(const char* a_text)
		{
			Icon(kIconWarning, kAmber);
			Im::PushStyleColor(kColText, kAmber);
			Im::TextWrapped("%s", a_text);
			Im::PopStyleColor();
		}

		// Grey note under a group: what a setting means for the section, or why it
		// currently does nothing.
		void Note(const char* a_text)
		{
			Im::PushStyleColor(kColText, kMuted);
			Im::TextWrapped("%s", a_text);
			Im::PopStyleColor();
		}

		// ---- Camera ------------------------------------------------------------

		void __stdcall RenderCamera()
		{
			const Compact compact;

			auto dials = Camera::Director::GetTunables();
			bool changed = false;

			PageHeader(kIconCamera, "CAMERA");

			if (BeginRows("cameraTop")) {
				changed |= ToggleRow("Direct the Camera", dials.enabled,
					"Direction", "bEnabled");

				// Read from the ini rather than Tunables; only Open() uses it.
				bool restoreFirst = Config::Bool("Direction", "bRestoreFirstPerson", true);
				static_cast<void>(ToggleRow("Return to First Person Afterwards", restoreFirst,
					"Direction", "bRestoreFirstPerson",
					"Only if this mod was the thing that took you out of it. A conversation "
					"you started in third person leaves you in third person either way."));

				EndRows();
			}

			Im::BeginDisabled(!dials.enabled);

			// ---- Per Line Angle Change ---------------------------------------------
			//
			// The first of two independent cut modes. The group's first row is its on/off
			// switch.
			if (Group(kIconLine, "Per Line Angle Change", "cutLines")) {
				Im::Indent();

				if (BeginRows("cutLinesGrid")) {
					changed |= ToggleRow("Change Angle Per Line", dials.perLineAngleChange,
						"Direction", "bPerLineAngleChange");
					EndRows();
				}

				Im::BeginDisabled(!dials.perLineAngleChange);

				if (BeginRows("cutLinesDials")) {
					changed |= SliderRow("Minimum Lines", dials.cutEveryMin, 1, 20,
						"Direction", "iCutEveryMin", "%d");
					changed |= SliderRow("Maximum Lines", dials.cutEveryMax, 1, 20,
						"Direction", "iCutEveryMax", "%d");
					changed |= ToggleRow("Ignore Short Lines", dials.holdOnShortLines,
						"Direction", "bHoldOnShortLines");
					EndRows();
				}

				Im::BeginDisabled(!dials.holdOnShortLines);
				if (BeginRows("cutLinesShort")) {
					changed |= SliderRow("Short Line Length", dials.shortLineWords, 2, 20,
						"Direction", "iShortLineWords", "%d words");
					EndRows();
				}
				Im::EndDisabled();

				Note("The camera keeps an angle until it has heard this many lines, "
					 "re-rolled between the two so the rhythm is not countable. "
					 "The count starts again with every reply.");

				Im::EndDisabled();
				Im::Unindent();
			}

			// ---- Reaction Shots ----------------------------------------------------
			if (Group(kIconFaces, "Reaction Shots", "cutReactions")) {
				Im::Indent();

				if (BeginRows("cutReactionsGrid")) {
					changed |= ToggleRow("Show You Listening", dials.reactionShots,
						"Direction", "bReactionShots",
						"Occasionally shows one of their lines on you, then cuts back. "
						"Skips short lines and full-intensity lines.");
					EndRows();
				}

				Im::BeginDisabled(!dials.reactionShots);
				if (BeginRows("cutReactionsDials")) {
					changed |= SliderRow("Lines Before A Reaction", dials.reactionEvery, 1, 10,
						"Direction", "iReactionEvery", "%d lines");
					changed |= SliderRow("Reaction Chance", dials.reactionChance, 0, 100,
						"Direction", "iReactionChance", "%d%%");
					EndRows();
				}
				Note("After this many of their lines, the chance is rolled. On a hit, "
					 "their next line plays on you. The count and the roll start again "
					 "with every reply.");
				Im::EndDisabled();

				Im::Unindent();
			}

			// ---- Persuasion --------------------------------------------------------
			if (Group(kIconLine, "Persuasion", "cutPersuasion", false)) {
				Im::Indent();
				if (BeginRows("cutPersuasionGrid")) {
					changed |= ToggleRow("Hold On Their Answer", dials.persuasionBeat,
						"Direction", "bPersuasionBeat",
						"When you persuade, intimidate or bribe, their answer gets a close-up "
						"of them, held for the whole reply while the camera pushes in slowly.");
					EndRows();
				}
				Im::Unindent();
			}

			// ---- Other People's Conversations --------------------------------------
			//
			// The key is on the Keys page and always works; only the automatic mode is set
			// here.
			if (Group(kIconShots, "Other People's Conversations", "cutScenes", false)) {
				Im::Indent();
				if (BeginRows("cutScenesGrid")) {
					changed |= ToggleRow("Film When You Stand Still", dials.sceneAuto,
						"Direction", "bFilmScenesAuto",
						"Two NPCs talking to each other in front of you are filmed like a "
						"conversation of your own once you have stood still nearby for a "
						"moment. Moving hands the camera back.");
					EndRows();
				}
				Im::BeginDisabled(!dials.sceneAuto);
				if (BeginRows("cutScenesDials")) {
					changed |= SliderRow("Within", dials.sceneRange, 150, 2000,
						"Direction", "iSceneRange", "%d units");
					changed |= SecondsRow("Stand Still For", dials.sceneWait, 0, 600,
						"Direction", "iSceneWait");
					EndRows();
				}
				Im::EndDisabled();
				Note("The Film Their Conversation key on the Keys page works with this off.");
				Im::Unindent();
			}

			// ---- Timed Angle Change ------------------------------------------------
			//
			// The second cut mode, independent of the first. Off on both sides by default.
			const bool anyTimer = dials.timedCutsWhileSpeaking || dials.timedCutsWhileChoosing;

			if (Group(kIconTimer, "Timed Angle Change", "cutTimed")) {
				Im::Indent();

				if (BeginRows("cutTimedToggles")) {
					changed |= ToggleRow("Timer While Talking", dials.timedCutsWhileSpeaking,
						"Direction", "bTimedCutsWhileSpeaking");
					changed |= ToggleRow("Timer While Choosing", dials.timedCutsWhileChoosing,
						"Direction", "bTimedCutsWhileChoosing");
					EndRows();
				}

				// Disabled when neither timer is on, since the duration would do nothing.
				Im::BeginDisabled(!anyTimer);
				if (BeginRows("cutTimedDials")) {
					changed |= SecondsRow("Change After", dials.maxShotTime, 100, 2000,
						"Direction", "iMaxShotTime");
					EndRows();
				}
				Im::EndDisabled();

				if (!anyTimer) {
					Note("Both timers are off, so angles change on lines alone.");
				}

				Im::Unindent();
			}

			// ---- Holds -------------------------------------------------------------
			//
			// Safety floors rather than a cut mode, so they're a separate group: they gate
			// every cut, including the line-based ones.
			if (Group(kIconHold, "Holds", "cutHolds", false)) {
				Im::Indent();
				if (BeginRows("cutHoldsGrid")) {
					changed |= SecondsRow("Shortest Hold", dials.minShotTime, 30, 900,
						"Direction", "iMinShotTime",
						"Minimum hold for normal cuts. Keep Subject Visible can recover sooner "
						"when the subject is obstructed.");
					changed |= SecondsRow("Shortest On A Turn", dials.minTurnTime, 0, 200,
						"Direction", "iMinTurnTime",
						"The floor when the conversation passes between the two of you, "
						"which is the one moment an angle is allowed to be cut short.");
					changed |= SecondsRow("Stay On You After You Pick", dials.playerBeat, 0, 300,
						"Direction", "iPlayerBeat");
					changed |= SecondsRow("Stay On You After You Speak", dials.playerVoiceHold, 0, 300,
						"Direction", "iPlayerVoiceHold",
						"For voiced player lines: how long the camera stays on you after your "
						"line ends. 0 cuts immediately.");
					EndRows();
				}
				Im::Unindent();
			}

			// ---- Framing -----------------------------------------------------------
			if (Group(kIconCamera, "Framing", "cutFraming", false)) {
				Im::Indent();
				if (BeginRows("cutFramingGrid")) {
					changed |= ToggleRow("Cut To You On Your Turn", dials.coverPlayerTurn,
						"Direction", "bCoverPlayerTurn");

					if (ToggleRow("Keep Subject Visible", dials.protectSubject,
						"Direction", "bKeepSubjectVisible",
						"Choose clear enabled shots and check the subject while the shot plays. "
						"Foreground objects matter only when they cover the face or touch the lens. "
						"Always checks actor obstructions. Enabling this turns off Ignore "
						"Obstructions Mid-Shot.")) {
						changed = true;
						if (dials.protectSubject) {
							dials.holdPlacement = false;
							Config::SetBool("Direction", "bHoldPlacement", false);
						}
					}

					Im::BeginDisabled(!dials.protectSubject);
					changed |= ToggleRow("First-Person Fallback", dials.firstPersonFallback,
						"Direction", "bFirstPersonFallback",
						"If no clear enabled shot is available, use first person while the "
						"conversation continues. Return after a cinematic view stays clear. "
						"Off can hold the current enabled shot even if obstructed. If it is "
						"disabled or unavailable, the normal game camera takes over.");
					Im::EndDisabled();

					Im::BeginDisabled(dials.protectSubject);
					changed |= ToggleRow("Avoid Framing Bystanders", dials.avoidCrowds,
						"Direction", "bAvoidCrowds",
						"Angles with somebody standing across the sightline score lower, so "
						"the camera picks around a crowd. Keep Subject Visible always checks "
						"actors and takes over this setting while enabled.");
					Im::EndDisabled();

					// The 180-degree rule. True 180 depends on Never Cross, so it turns that on
					// and locks it.
					if (ToggleRow("True 180 Rule", dials.true180,
						"Direction", "bTrue180",
						"Films you over one shoulder and them over the opposite one, so the "
						"camera stays on one side of the conversation. Also turns on Never "
						"Cross The Eyeline.")) {
						changed = true;
						if (dials.true180) {
							dials.enforceLine = true;
							Config::SetBool("Direction", "bEnforceLine", true);
						}
					}

					Im::BeginDisabled(dials.true180);
					changed |= ToggleRow("Never Cross The Eyeline", dials.enforceLine,
						"Direction", "bEnforceLine",
						"Stops the camera swinging an angle across the line between you while "
						"it looks for room. Reverse shots can still land on the far side; "
						"True 180 Rule fixes that.");
					Im::EndDisabled();

					changed |= ToggleRow("Close-Ups Follow The Face", dials.followFace,
						"Direction", "bFollowFace",
						"Close-ups stay anchored to the person but frame the face their head "
						"is actually carrying: lower and further forward when they lean over "
						"something, and from the side the face is turned toward. Decided when "
						"the shot cuts, so it never swings mid-shot.");

					if (ToggleRow("Ignore Obstructions Mid-Shot", dials.holdPlacement,
						"Direction", "bHoldPlacement",
						"Check for walls and bodies when the angle is chosen, then stop. "
						"The camera holds still while a cart or a passer-by crosses frame, "
						"instead of easing in and back out. It also stops getting out of "
						"the way, so a conversation that walks somewhere can clip. Enabling "
						"this turns off Keep Subject Visible.")) {
						changed = true;
						if (dials.holdPlacement) {
							dials.protectSubject = false;
							Config::SetBool("Direction", "bKeepSubjectVisible", false);
						}
					}
					EndRows();
				}
				if (dials.protectSubject) {
					Note("Subject protection includes actor checks. Objects beside the face "
						 "can stay in the foreground.");
				}
				Im::Unindent();
			}

			Im::EndDisabled();

			if (changed) {
				Camera::Director::ApplyTunables(dials);
			}
		}

		// ---- Screen ------------------------------------------------------------

		void __stdcall RenderScreen()
		{
			const Compact compact;

			auto dials = Camera::Director::GetTunables();
			bool changed = false;

			PageHeader(kIconScreen, "SCREEN");

			if (Group(kIconScreen, "Black Bars", "screenBars")) {
				Im::Indent();
				if (BeginRows("screenBarsGrid")) {
					// Before the switch existed, "no bars" meant dragging the height to zero.
					// Turning the switch on from a zero height gives it a default height; a height
					// someone set deliberately is never overwritten.
					if (ToggleRow("Black Bars", dials.letterbox,
							"Direction", "bLetterbox")) {
						changed = true;

						if (dials.letterbox && dials.letterboxHeight <= 0) {
							const Camera::Tunables shipped{};
							dials.letterboxHeight = shipped.letterboxHeight;
							Config::SetInt("Direction", "iLetterboxHeight",
								dials.letterboxHeight);
							Log::Info(Log::Category::kCore,
								"Black bars switched on at zero height; set to the shipped "
								"{}%."sv,
								dials.letterboxHeight / 10);
						}
					}

					Im::BeginDisabled(!dials.letterbox);
					changed |= PercentRow("Bar Height", dials.letterboxHeight, 30,
						"Direction", "iLetterboxHeight");
					changed |= ToggleRow("Subtitles In The Black Bar", dials.subtitlesInBar,
						"Direction", "bSubtitlesInBar",
						"The line being spoken sits centred in the bottom bar and follows it. "
						"With no bar, or one too thin for the lines, it goes back over the "
						"picture.");
					Im::EndDisabled();

					EndRows();
				}
				Im::Unindent();
			}

			if (Group(kIconLine, "Dialogue", "screenDialogue")) {
				Im::Indent();

				if (BeginRows("screenDialogueGrid")) {
					changed |= ToggleRow("Fade Out Dialogue", dials.fadeTopicList,
						"Direction", "bFadeTopicList");
					changed |= ToggleRow("Hide NPC Name", dials.hideSpeakerName,
						"Direction", "bHideSpeakerName");
					EndRows();
				}

				Im::BeginDisabled(!dials.fadeTopicList);
				if (BeginRows("screenFadeGrid")) {
					changed |= ToggleRow("Fade After PC Line", dials.fadeAfterPlayerLine,
						"Direction", "bFadeAfterPlayerLine",
						"Holds your options on screen for as long as your own voiced line "
						"is playing, then starts the delay. Needs a player-voice mod to do "
						"anything; without one your line is instant and the fade begins at "
						"the click either way.");
					changed |= SecondsRow("Fade Delay", dials.choiceFadeDelay, 0, 600,
						"Direction", "iChoiceFadeDelay");
					changed |= SecondsRow("Fade Time", dials.choiceFadeTime, 5, 200,
						"Direction", "iChoiceFadeTime");
					EndRows();
				}
				Im::EndDisabled();

				if (!dials.fadeTopicList) {
					Note("Your options stay at full opacity for the whole conversation.");
				}

				Im::Unindent();
			}

			if (changed) {
				Camera::Director::ApplyTunables(dials);
			}
		}

		// ---- Faces -------------------------------------------------------------
		// Expressions and the independent mouth fallback.
		void __stdcall RenderFaces()
		{
			const Compact compact;

			PageHeader(kIconFaces, "FACES");

			if (Group(kIconFaces, "Expressions", "faceExpressions")) {
				Im::Indent();
				bool expressions = Scene::Performance::ExpressionsEnabled();
				bool regional = Scene::RegionalFace::Enabled();
				bool touched = false;
				if (BeginRows("faceExpressionRows")) {
					touched |= ToggleRow("Responsive Expressions", expressions,
						"Performance", "bExpressions");
					Im::BeginDisabled(!expressions);
					if (ToggleRow("Eye & Cheek Detail", regional, "Performance", "bRegionalExpressions"))
						Scene::RegionalFace::SetEnabled(regional);
					Im::EndDisabled();
					EndRows();
				}
				if (touched) Scene::Performance::Configure(expressions, false);
				Note("Reactions follow dialogue intent and carry briefly between related lines. "
					"Eye & Cheek Detail adds regional movement on supported heads while preserving mouth lip sync.");
				Im::Unindent();
			}

			if (Group(kIconFaces, "Mouth", "faceVoice")) {
				Im::Indent();

				bool synth = Scene::LipSync::Enabled();
				int  mouth = Scene::LipSync::StrengthPercent();

				bool touched = false;

				if (BeginRows("faceVoiceMouth")) {
					touched |= ToggleRow("Lip Sync Fallback", synth,
						"Performance", "bSynthLipSync");

					Im::BeginDisabled(!synth);
					touched |= SliderRow("Mouth Movement", mouth, 0, 100,
						"Performance", "iLipSyncStrength", "%d%%");
					Im::EndDisabled();
					EndRows();
				}

				// Only shown while the option is on.
				if (synth) {
					Warning("For DBVO 1 or DBVO 2 only. Dragonborn ReVoiced is the better "
							"choice and drives your mouth itself \xe2\x80\x94 running both means "
							"two mods writing the same visemes.");
					Im::Spacing();
				}

				if (touched) {
					Scene::LipSync::Configure(synth, mouth);
				}

				Im::Unindent();
			}

			if (Group(kIconFaces, "Body", "faceBody")) {
				Im::Indent();
				auto dials = Camera::Director::GetTunables();
				bool changed = false;
				if (BeginRows("faceBodyRows")) {
					changed |= ToggleRow("Your Gestures Start With Your Line", dials.playerGestureCue,
						"Performance", "bPlayerGestureCue",
						"Sends the PlayDBVOTopic event when your voiced line starts, so a "
						"DBVO player-gesture add-on gestures while you speak. Not sent while "
						"DBVO 1 is loaded; it sends its own.");
					changed |= ToggleRow("They Stop Working To Talk", dials.stopWorkToTalk,
						"Performance", "bStopWorkToTalk",
						"Somebody at a workbench, smelter, forge or tanning rack steps away "
						"from it 3 to 5 seconds after they start talking. Seats, beds and "
						"quest scenes are left alone.");
					EndRows();
				}
				if (changed) {
					Camera::Director::ApplyTunables(dials);
				}
				Im::Unindent();
			}

			Note("Brow and squint poses are part of each expression profile, not a separate speech effect. "
				"Strength is automatically capped and tuned for cinematic acting. Edit Expression.* sections in SD_user.ini to customize profiles. Mouth lip sync stays independent.");
		}

		// ---- Keys --------------------------------------------------------------

		// Any key, captured from the game's own input. Reading the keyboard through
		// ImGui would need a translation table between its key enum and the game's
		// DirectX scan codes; capturing from the game's input stream records exactly
		// the code that will match later. See Hotkeys.h.
		using Action = Core::Hotkeys::Action;

		void KeyRow(const char* a_label, Action a_action)
		{
			const bool waiting = Core::Hotkeys::CapturingFor(a_action);
			const auto code = Core::Hotkeys::Binding(a_action);
			const auto name = Core::Hotkeys::KeyName(code);

			Im::PushID(a_label);
			Row(a_label, false);

			char button[96]{};
			if (waiting) {
				std::snprintf(button, sizeof(button), "Press any key...###bind");
			} else {
				std::snprintf(button, sizeof(button), "%.*s###bind",
					static_cast<int>(name.size()), name.data());
			}

			Im::PushStyleColor(kColButton, kBlueDeep);
			Im::PushStyleColor(kColButtonHovered, kBlueLine);
			Im::PushStyleColor(kColButtonActive, kBlue);
			if (Im::Button(button, ImGuiMCP::ImVec2{ 170.0f, 0.0f })) {
				if (waiting) {
					Core::Hotkeys::Cancel();
				} else {
					Core::Hotkeys::Arm(a_action);
				}
			}
			Im::PopStyleColor(3);

			if (code != 0 && !waiting) {
				Im::SameLine();
				if (Im::SmallButton("Clear")) {
					Core::Hotkeys::SetBinding(a_action, 0);
				}
			}

			if (waiting) {
				Im::SameLine();
				Im::TextDisabled("Esc cancels");
			}

			Im::PopID();
		}

		// The framework's input route. While this menu is open the framework takes
		// input to drive ImGui, so the game's own sink may not see the binding press.
		// Returns false: this observes and doesn't claim the press. OfferKey is
		// idempotent, so a key arriving by both routes is taken once.
		bool __stdcall OnMenuInput(RE::InputEvent* a_event)
		{
			if (!a_event || !Core::Hotkeys::Capturing()) {
				return false;
			}

			auto* button = a_event->AsButtonEvent();
			if (!button || !button->IsDown()) {
				return false;
			}
			if (a_event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
				return false;
			}

			static_cast<void>(Core::Hotkeys::OfferKey(button->GetIDCode()));
			return false;
		}

		void __stdcall RenderControls()
		{
			const Compact compact;

			// Polled here rather than in the input sink, because binding writes the ini.
			Core::Hotkeys::PollCapture();

			PageHeader(kIconKeys, "KEYS");

			if (BeginRows("keyGrid")) {
				KeyRow("Next Angle", Action::kNextAngle);
				KeyRow("Who To Look At", Action::kFraming);
				KeyRow("Film Their Conversation", Action::kFilmScene);
				EndRows();
			}

			Im::Separator();
			Im::Spacing();

			if (!Camera::Director::Staging()) {
				Note("Not in a conversation.");
			} else if (Camera::Director::FilmingScene()) {
				Icon(kIconCheck, kGreen);
				Im::Text("Filming their conversation.");
			} else {
				const auto label = Camera::FramingLabel(Camera::Director::CurrentFraming());
				Icon(kIconCheck, kGreen);
				Im::Text("Looking at: %.*s", static_cast<int>(label.size()), label.data());
			}
		}

		// ---- Shots -------------------------------------------------------------

		using Camera::ShotType;

		// The three pools, in reading order. Shoulder shots first in each, since
		// they're the staples.
		constexpr std::array kNpcShots{
			ShotType::kOverPlayerShoulder,
			ShotType::kOverPlayerShoulderLow,
			ShotType::kOverPlayerShoulderHigh,
			ShotType::kOverPlayerShoulderWide,
			ShotType::kDirtyNpc,
			ShotType::kThreeQuarterNpc,
			ShotType::kCloseUp,
			ShotType::kExtremeClose,
			ShotType::kCloseLow,
			ShotType::kCloseHigh,
			ShotType::kCloseProfile,
			ShotType::kCloseWide,
			ShotType::kMediumNpc,
			ShotType::kMediumProfile,
			ShotType::kLowAngle,
			ShotType::kLowProfile,
			ShotType::kLongNpc,
			ShotType::kOverhead,
		};

		constexpr std::array kPlayerShots{
			ShotType::kOverNpcShoulder,
			ShotType::kOverNpcShoulderLow,
			ShotType::kOverNpcShoulderHigh,
			ShotType::kOverNpcShoulderWide,
			ShotType::kDirtyPlayer,
			ShotType::kThreeQuarterPlayer,
			ShotType::kClosePlayer,
			ShotType::kExtremeClosePlayer,
			ShotType::kMediumPlayer,
			ShotType::kPlayerProfile,
			ShotType::kPlayerLow,
			ShotType::kHighAngle,
			ShotType::kLongPlayer,
			ShotType::kPlayerOverhead,
		};

		constexpr std::array kRoomShots{
			ShotType::kTwoShot,
			ShotType::kProfile,
			ShotType::kWide,
			ShotType::kMaster,
			ShotType::kGroundLevel,
			ShotType::kDistant,
			ShotType::kDistantLow,
		};

		// A shot missing from all three lists couldn't be switched off, so adding one
		// to the enum without listing it here fails the build.
		static_assert(
			kNpcShots.size() + kPlayerShots.size() + kRoomShots.size() ==
				static_cast<std::size_t>(ShotType::kCount),
			"Every shot must appear on the Shots page, or it cannot be turned off.");

		// Values a locked setup gets for the amount and duration it doesn't use.
		// They're hidden in the panel, but preset comparison still reads them, so
		// they're set to the same values the built-in looks write.
		constexpr int kInertAmount = 0;
		constexpr int kInertTime = 400;

		// The moves in enum order, which is the ini storage format (see Move).
		constexpr std::array kMoves{
			Camera::Move::kLocked, Camera::Move::kPushIn, Camera::Move::kPullOut,
			Camera::Move::kCraneUp, Camera::Move::kCraneDown,
			Camera::Move::kTiltUp, Camera::Move::kTiltDown,
			Camera::Move::kDrift,
			Camera::Move::kZoomIn, Camera::Move::kZoomOut,
			Camera::Move::kOrbitLeft, Camera::Move::kOrbitRight,
			Camera::Move::kTruckLeft, Camera::Move::kTruckRight,
		};

		// "None" for the no-move entry; everything else uses the shot table's name.
		[[nodiscard]] const char* EffectLabel(Camera::Move a_move)
		{
			return a_move == Camera::Move::kLocked ? "None" : Camera::MoveLabel(a_move).data();
		}

		void SetEffect(ShotType a_type, Camera::Move a_move)
		{
			Camera::Shot::SetMove(a_type, a_move);
			Config::SetInt("Shots", Camera::MoveKey(a_type), static_cast<int>(a_move));

			// Choosing None also resets the two hidden values. See kInertAmount.
			if (a_move == Camera::Move::kLocked) {
				Camera::Shot::SetMoveAmount(a_type, kInertAmount);
				Camera::Shot::SetMoveTime(a_type, kInertTime);
				Config::SetInt("Shots", Camera::MoveAmountKey(a_type), kInertAmount);
				Config::SetInt("Shots", Camera::MoveTimeKey(a_type), kInertTime);
			}
		}

		// The per-angle editor popup.
		void ShotEditor(ShotType a_type)
		{
			Im::SetNextWindowSize(ImGuiMCP::ImVec2{ 460.0f, 0.0f });
			if (!Im::BeginPopup("###edit")) {
				return;
			}

			// A popup is its own window and the font scale is per window, so set it here.
			// Not scoped: Compact's destructor would run after EndPopup and reset the
			// panel's scale instead.
			Im::SetWindowFontScale(kFontScale);

			const auto name = Camera::Name(a_type);
			Icon(kIconShots, kBlue);
			Im::Text("%.*s", static_cast<int>(name.size()), name.data());
			Im::SameLine();
			Im::TextDisabled("- %.*s", static_cast<int>(Camera::SubjectName(a_type).size()),
				Camera::SubjectName(a_type).data());
			Im::Separator();
			Im::Spacing();

			const int authoredWeight = Camera::AuthoredWeight(a_type);
			const int authoredLens = static_cast<int>(Camera::AuthoredLens(a_type));

			int  weight = Camera::Shot::Weight(a_type);
			int  lens = Camera::Shot::Lens(a_type);
			auto current = Camera::Shot::MoveOf(a_type);

			if (BeginRows("shotEdit")) {
				Row("Effect");
				if (Im::BeginCombo("###effect", EffectLabel(current))) {
					for (const auto move : kMoves) {
						const bool selected = move == current;
						if (Im::Selectable(EffectLabel(move), selected)) {
							SetEffect(a_type, move);
							current = move;
						}
						if (selected) {
							Im::SetItemDefaultFocus();
						}
					}
					Im::EndCombo();
				}

				// Hidden rather than greyed out; the row above already says "None".
				if (current != Camera::Move::kLocked) {
					int amount = Camera::Shot::MoveAmount(a_type);
					if (SliderRow("Amount", amount, 0, 100, "Shots",
							Camera::MoveAmountKey(a_type), "%d%%")) {
						Camera::Shot::SetMoveAmount(a_type, amount);
					}

					int moveTime = Camera::Shot::MoveTime(a_type);
					if (SecondsRow("Duration", moveTime, 30, 900, "Shots",
							Camera::MoveTimeKey(a_type))) {
						Camera::Shot::SetMoveTime(a_type, moveTime);
					}
				}

				// "FOV", after the unit, so the number in the track needs no caption.
				if (SliderRow("FOV", lens, Camera::kMinLens, Camera::kMaxLens, "Shots",
						Camera::LensKey(a_type), "%d\xc2\xb0")) {
					Camera::Shot::SetLens(a_type, lens);
				}

				// "Frequency" rather than "Weight": doubling it doubles how often the angle
				// comes up.
				if (SliderRow("Frequency", weight, 0, 100, "Shots",
						Camera::WeightKey(a_type), weight == 0 ? "never" : "%d")) {
					Camera::Shot::SetWeight(a_type, weight);
				}

				EndRows();
			}

			// Per-angle lighting, only shown with [Lighting] bPerShot=1 set by hand.
			if (Config::Bool("Lighting", "bPerShot", false)) {
				Im::Spacing();
				Im::SeparatorText("Light");

				const auto looks = Scene::AllLooks();
				int        look = Camera::Shot::LightOf(a_type);
				if (look < 0 || static_cast<std::size_t>(look) >= looks.size()) {
					look = Scene::FindLook(Camera::AuthoredLight(a_type));
				}
				if (look < 0) {
					look = Scene::DefaultLook();
				}

				if (BeginRows("shotLight")) {
					Row("Look");
					if (Im::BeginCombo("###look", looks[static_cast<std::size_t>(look)].name)) {
						for (std::size_t i = 0; i < looks.size(); ++i) {
							const bool selected = static_cast<int>(i) == look;
							if (Im::Selectable(looks[i].name, selected)) {
								Camera::Shot::SetLight(a_type, static_cast<int>(i));
								Config::SetString("Shots", Camera::LightKey(a_type), looks[i].key);
								look = static_cast<int>(i);
							}
							if (Im::IsItemHovered()) {
								Im::SetTooltip("%s", looks[i].summary);
							}
							if (selected) {
								Im::SetItemDefaultFocus();
							}
						}
						Im::EndCombo();
					}

					int  lx = Camera::Shot::LightOffsetX(a_type);
					int  ly = Camera::Shot::LightOffsetY(a_type);
					int  lz = Camera::Shot::LightOffsetZ(a_type);
					bool nudged = false;

					nudged |= SliderRow("Left / Right", lx, -400, 400, "Shots",
						Camera::LightXKey(a_type));
					nudged |= SliderRow("Near / Far", ly, -400, 400, "Shots",
						Camera::LightYKey(a_type));
					nudged |= SliderRow("Down / Up", lz, -400, 400, "Shots",
						Camera::LightZKey(a_type));

					if (nudged) {
						Camera::Shot::SetLightOffset(a_type, lx, ly, lz);
					}

					EndRows();
				}
			}

			// A Default button per angle, shown only when there's something to undo. It
			// doesn't touch the on/off switch, so a misclick can't undo someone's angle
			// selection.
			const auto authoredMove = Camera::Shot::AuthoredMove(a_type);
			const int  authoredAmount = Camera::Shot::AuthoredMoveAmount(a_type);
			const int  authoredTime = Camera::Shot::AuthoredMoveTime(a_type);

			const bool tuned = weight != authoredWeight || lens != authoredLens ||
				current != authoredMove ||
				Camera::Shot::MoveAmount(a_type) != authoredAmount ||
				Camera::Shot::MoveTime(a_type) != authoredTime;

			Im::Spacing();
			Im::Separator();

			Im::BeginDisabled(!tuned);
			Icon(kIconReset, tuned ? kBlue : kMuted);
			if (Im::Button("Put This Angle Back")) {
				Camera::Shot::SetWeight(a_type, authoredWeight);
				Camera::Shot::SetLens(a_type, authoredLens);
				Camera::Shot::SetMove(a_type, authoredMove);
				Camera::Shot::SetMoveAmount(a_type, authoredAmount);
				Camera::Shot::SetMoveTime(a_type, authoredTime);

				Config::SetInt("Shots", Camera::WeightKey(a_type), authoredWeight);
				Config::SetInt("Shots", Camera::LensKey(a_type), authoredLens);
				Config::SetInt("Shots", Camera::MoveKey(a_type), static_cast<int>(authoredMove));
				Config::SetInt("Shots", Camera::MoveAmountKey(a_type), authoredAmount);
				Config::SetInt("Shots", Camera::MoveTimeKey(a_type), authoredTime);
			}
			Im::EndDisabled();

			Im::SameLine();
			if (Im::Button("Close")) {
				Im::CloseCurrentPopup();
			}

			Im::EndPopup();
		}

		// Cells per angle. Used by the column setup, the fill and the blank half of an
		// odd row.
		constexpr int kShotColumns = 4;

		// Four columns per angle (on, name, frequency, edit), two angles per row. FOV
		// and the move live in the editor.
		//
		// Column widths are measured from the current font and padding rather than
		// fixed pixels, since the panel scales its font. The user_id argument keeps
		// the two "On" columns from sharing an ID.
		void SetupShotColumns(int a_half)
		{
			constexpr auto kFixed = ImGuiMCP::ImGuiTableColumnFlags_WidthFixed;
			constexpr auto kStretch = ImGuiMCP::ImGuiTableColumnFlags_WidthStretch;
			constexpr auto kNoHeader = ImGuiMCP::ImGuiTableColumnFlags_NoHeaderLabel;

			const auto id = [a_half](int a_index) {
				return static_cast<unsigned>(a_half * kShotColumns + a_index);
			};

			const auto* style = Im::GetStyle();
			const float cell = style->CellPadding.x * 2.0f;
			const float frame = style->FramePadding.x * 2.0f;

			// A few pixels of slack: an exact fit is one rounding error from clipping.
			constexpr float kSlack = 4.0f;

			const float onWidth = Im::GetFrameHeight() + cell + kSlack;
			const float freqWidth = Im::CalcTextSize("Frequency").x + cell + kSlack;
			const float editWidth = Im::CalcTextSize("Edit").x + frame + cell + kSlack;

			Im::TableSetupColumn("", kFixed | kNoHeader, onWidth, id(1));
			Im::TableSetupColumn("Angle", kStretch, 1.0f, id(2));
			Im::TableSetupColumn("Frequency", kFixed, freqWidth, id(3));
			Im::TableSetupColumn("", kFixed | kNoHeader, editWidth, id(4));
		}

		// One angle, filling the next four cells.
		void ShotCells(ShotType a_type)
		{
			Im::PushID(static_cast<int>(a_type));

			bool      on = Camera::Shot::Enabled(a_type);
			const int weight = Camera::Shot::Weight(a_type);

			// An angle that's on but weighted to zero isn't in play; the name shows that,
			// as does the header count.
			const bool live = on && weight > 0;

			Im::TableNextColumn();
			Im::PushStyleColor(kColCheck, kGreen);
			if (Im::Checkbox("###on", &on)) {
				Config::SetBool("Shots", Camera::Key(a_type), on);
				Camera::Shot::SetEnabled(a_type, on);
			}
			Im::PopStyleColor();

			// White if it's in play, grey if not.
			Im::TableNextColumn();
			Im::AlignTextToFramePadding();
			const auto name = Camera::Name(a_type);
			char       label[96]{};
			std::snprintf(label, sizeof(label), "%.*s", static_cast<int>(name.size()), name.data());
			Text(live ? kTextOn : kTextOff, label);

			// How often it comes up.
			Im::TableNextColumn();
			Im::AlignTextToFramePadding();
			if (weight == 0) {
				Im::TextDisabled("never");
			} else {
				Im::TextDisabled("%d", weight);
			}

			Im::TableNextColumn();
			Im::PushStyleColor(kColButton, kBlueDeep);
			Im::PushStyleColor(kColButtonHovered, kBlueLine);
			Im::PushStyleColor(kColButtonActive, kBlue);
			if (Im::SmallButton("Edit")) {
				Im::OpenPopup("###edit");
			}
			Im::PopStyleColor(3);

			// Inside the same PushID, so each angle has its own popup.
			ShotEditor(a_type);

			Im::PopID();
		}

		// Blank cells for the empty half of the last row in an odd-sized group.
		void EmptyShotCells()
		{
			for (int i = 0; i < kShotColumns; ++i) {
				Im::TableNextColumn();
			}
		}

		[[nodiscard]] int CountOn(std::span<const ShotType> a_shots)
		{
			int on = 0;
			for (const auto type : a_shots) {
				if (Camera::Shot::Enabled(type) && Camera::Shot::Weight(type) > 0) {
					++on;
				}
			}
			return on;
		}

		void SetAll(std::span<const ShotType> a_shots, bool a_on)
		{
			for (const auto type : a_shots) {
				Camera::Shot::SetEnabled(type, a_on);
				Config::SetBool("Shots", Camera::Key(type), a_on);
			}
		}

		void ShotSection(const char* a_title, const char* a_id, std::span<const ShotType> a_shots,
			const char* a_empty)
		{
			const int on = CountOn(a_shots);
			const int total = static_cast<int>(a_shots.size());

			char heading[128]{};
			std::snprintf(heading, sizeof(heading), "%s  -  %d of %d on###%s",
				a_title, on, total, a_id);

			Icon(kIconShots, kBlue);

			Im::PushStyleColor(kColHeader, kBlueDeep);
			Im::PushStyleColor(kColHeaderHovered, kBlueLine);
			Im::PushStyleColor(kColHeaderActive, kBlue);
			const bool open = Im::CollapsingHeader(heading, ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen);
			Im::PopStyleColor(3);

			if (!open) {
				return;
			}

			Im::PushID(a_id);

			// An empty group means the camera has nothing to draw for that side of the
			// conversation and falls back to the emergency single, so say so.
			if (on == 0) {
				Warning(a_empty);
			}

			Im::PushStyleColor(kColButton, kBlueDeep);
			Im::PushStyleColor(kColButtonHovered, kBlueLine);
			Im::PushStyleColor(kColButtonActive, kBlue);
			if (Im::SmallButton("All on")) {
				SetAll(a_shots, true);
			}
			Im::SameLine();
			if (Im::SmallButton("All off")) {
				SetAll(a_shots, false);
			}
			Im::PopStyleColor(3);

			// Row stripes instead of a full grid, with a single rule under the header.
			constexpr auto kTableFlags = ImGuiMCP::ImGuiTableFlags_RowBg |
			                             ImGuiMCP::ImGuiTableFlags_NoSavedSettings |
			                             ImGuiMCP::ImGuiTableFlags_SizingFixedFit |
			                             ImGuiMCP::ImGuiTableFlags_PadOuterX;

			Im::Spacing();

			// Grey header rather than blue; the accent belongs to the group title.
			Im::PushStyleColor(kColTableHeaderBg, 0x00000000u);

			if (Im::BeginTable("angles", kShotColumns * 2, kTableFlags)) {
				SetupShotColumns(0);
				SetupShotColumns(1);

				Im::PushStyleColor(kColText, kMuted);
				Im::TableHeadersRow();
				Im::PopStyleColor();

				const auto count = a_shots.size();
				for (std::size_t i = 0; i < count; i += 2) {
					Im::TableNextRow();

					ShotCells(a_shots[i]);

					if (i + 1 < count) {
						ShotCells(a_shots[i + 1]);
					} else {
						// Submit the empty cells so the stripe runs the full width.
						EmptyShotCells();
					}
				}

				Im::EndTable();
			}

			Im::PopStyleColor();

			Im::PopID();
		}

		void __stdcall RenderShots()
		{
			const Compact compact;

			const int on = CountOn(kNpcShots) + CountOn(kPlayerShots) + CountOn(kRoomShots);
			const int total = static_cast<int>(
				kNpcShots.size() + kPlayerShots.size() + kRoomShots.size());

			// The tally rides on the page heading.
			char tally[64]{};
			std::snprintf(tally, sizeof(tally), "%d of %d in play", on, total);
			PageHeader(kIconShots, "SHOTS", tally);

			ShotSection("NPC", "npcCols", kNpcShots,
				"Nothing here is on. The camera has no angle to use while they are "
				"speaking, so it will fall back to a plain shot for every line.");
			ShotSection("PC", "playerCols", kPlayerShots,
				"Nothing here is on. The camera has no angle to use on your turn. "
				"Turn at least one on, or switch off 'Cut To You On Your Turn' under "
				"Camera.");
			ShotSection("Room", "roomCols", kRoomShots,
				"Nothing here is on. That is a perfectly normal way to shoot a "
				"conversation - the camera will simply stay on faces.");
		}

		// ---- Presets -----------------------------------------------------------

		// Cached answer to "which preset is active, and how far is each one". One
		// drift value is around fifty profile reads, each a file operation that MO2's
		// virtual filesystem hooks; doing that per row per frame was over a thousand
		// syscalls a frame. Refreshed on a timer, since SD.ini can be edited outside
		// the game.
		struct PresetState
		{
			const Camera::Preset*                 active{ nullptr };
			int                                   activeSlot{ -1 };
			int                                   activeInstalled{ -1 };
			std::size_t                           count{ 0 };
			std::chrono::steady_clock::time_point taken{};
			bool                                  valid{ false };

			// Cached for the same reason; each slot is two profile reads.
			std::array<Camera::CustomSlot, Camera::kCustomSlots> slots{};
		};

		PresetState presetState;

		void RefreshPresetState(bool a_force)
		{
			const auto now = std::chrono::steady_clock::now();
			if (!a_force && presetState.valid &&
				now - presetState.taken < std::chrono::milliseconds(500)) {
				return;
			}

			const auto presets = Camera::AllPresets();
			presetState.count = presets.size();
			presetState.active = nullptr;

			// Stops at the first exact match; two presets can't both be active.
			for (std::size_t i = 0; i < presetState.count; ++i) {
				if (Camera::PresetDrift(presets[i]) == 0) {
					presetState.active = &presets[i];
					break;
				}
			}

			for (int i = 0; i < Camera::kCustomSlots; ++i) {
				presetState.slots[static_cast<std::size_t>(i)] = Camera::ReadCustomSlot(i);
			}

			// Preset files are compared against live state like the built-ins, so
			// they're cheap; the folder itself is only re-read every few seconds.
			Camera::RescanPresetFiles(false);
			presetState.activeInstalled = Camera::ActiveInstalledPreset();

			// Checked after the built-ins and takes precedence: a saved slot or an
			// exported file can hold the same settings as a built-in (right after
			// applying one and saving it). Only one row is ever ticked.
			presetState.activeSlot = Camera::ActiveCustomSlot();
			if (presetState.activeSlot >= 0) {
				presetState.active = nullptr;
				presetState.activeInstalled = -1;
			} else if (presetState.activeInstalled >= 0) {
				presetState.active = nullptr;
			}

			presetState.taken = now;
			presetState.valid = true;
		}

		// The player's own saved looks, kept separate from the built-ins: a built-in
		// is a designed set of shots, a slot is a snapshot of settings.
		void RenderCustomSlots()
		{
			// One rename buffer per slot; InputText edits in place.
			static std::array<std::array<char, 40>, Camera::kCustomSlots> nameBuf{};
			static bool                                                   primed = false;

			Im::Spacing();

			// Gold marks a slot that holds something; green marks the active one.
			constexpr unsigned kSlotGold = 0xFF4FC4E8u;

			Im::PushStyleColor(kColText, kSlotGold);
			Im::SeparatorText("MY PRESETS");
			Im::PopStyleColor();

			const int activeSlot = presetState.activeSlot;

			for (int i = 0; i < Camera::kCustomSlots; ++i) {
				const auto  idx = static_cast<std::size_t>(i);
				const auto& slot = presetState.slots[idx];
				const bool  live = slot.used && activeSlot == i;

				if (!primed) {
					std::snprintf(nameBuf[idx].data(), nameBuf[idx].size(), "%s",
						slot.used ? slot.name.c_str() : "");
				}

				Im::PushID(1000 + i);

				char header[160]{};
				if (slot.used) {
					std::snprintf(header, sizeof(header), "%s%s###slot",
						slot.name.c_str(), live ? "  (in use)" : "");
				} else {
					std::snprintf(header, sizeof(header), "Slot %d   -   empty###slot", i + 1);
				}

				Icon(live ? kIconCheck : kIconPresets,
					live ? kGreen : (slot.used ? kSlotGold : kMuted));

				Im::PushStyleColor(kColText, live ? kGreen : (slot.used ? kSlotGold : kMuted));
				const bool open = Im::CollapsingHeader(header);
				Im::PopStyleColor();

				if (open) {
					Im::Indent();

					if (slot.used) {
						Icon(kIconCheck, kBlue);
						if (Im::Button("Use")) {
							Camera::ApplyCustomSlot(i);
							RefreshPresetState(true);
						}
						Im::SameLine();
						Icon(kIconSave, kBlue);
						if (Im::Button("Save Over")) {
							Camera::SaveCustomSlot(i, nameBuf[idx].data());
							RefreshPresetState(true);
						}
						Im::SameLine();
						Icon(kIconDelete, kAmber);
						if (Im::Button("Delete")) {
							Camera::DeleteCustomSlot(i);
							nameBuf[idx][0] = '\0';
							RefreshPresetState(true);
						}

						Icon(kIconRename, kMuted);
						Im::SetNextItemWidth(220.0f);
						if (Im::InputText("Name", nameBuf[idx].data(), nameBuf[idx].size())) {
							// Renamed on every keystroke rather than behind a Save button.
							Camera::RenameCustomSlot(i, nameBuf[idx].data());
						}
					} else {
						Icon(kIconRename, kMuted);
						Im::SetNextItemWidth(220.0f);
						Im::InputTextWithHint("Name", "e.g. My tavern look",
							nameBuf[idx].data(), nameBuf[idx].size());

						const bool named = nameBuf[idx][0] != '\0';
						Im::BeginDisabled(!named);
						Icon(kIconSave, named ? kBlue : kMuted);
						if (Im::Button("Save What I Have Now")) {
							Camera::SaveCustomSlot(i, nameBuf[idx].data());
							RefreshPresetState(true);
						}
						Im::EndDisabled();
					}

					Im::Unindent();
				}

				Im::PopID();
			}

			primed = true;
		}

		// Preset files, which is how presets get shared: exported below, or someone
		// else's installed as a mod.
		void RenderInstalledPresets()
		{
			Im::Spacing();
			Im::PushStyleColor(kColText, kBlue);
			Im::SeparatorText("INSTALLED PRESETS");
			Im::PopStyleColor();

			const auto& installed = Camera::InstalledPresets();
			if (installed.empty()) {
				Note("None installed. Preset files go in SKSE/Plugins/SceneDirector/Presets/, "
					 "or come with a preset mod.");
				return;
			}

			for (std::size_t i = 0; i < installed.size(); ++i) {
				const auto& preset = installed[i];
				const bool  current = presetState.activeInstalled == static_cast<int>(i);

				Im::PushID(2000 + static_cast<int>(i));

				Icon(current ? kIconCheck : kIconPresets, current ? kGreen : kMuted);

				Im::BeginDisabled(!preset.usable);
				Im::PushStyleColor(kColText, current ? kGreen : 0xFFCCCCCCu);
				Im::PushStyleColor(kColCheck, kGreen);
				bool       ticked = current;
				const bool clicked = Im::Checkbox(preset.name.c_str(), &ticked);
				Im::PopStyleColor(2);
				Im::EndDisabled();

				std::string about = preset.author.empty() ? std::string{} : "by " + preset.author;
				if (!preset.description.empty()) {
					about += about.empty() ? preset.description : " - " + preset.description;
				}
				if (about.empty()) {
					about = preset.file;
				}
				Im::PushStyleColor(kColText, kMuted);
				Im::TextWrapped("      %s", about.c_str());
				Im::PopStyleColor();

				// A broken file can't be ticked; a file with typos still can, minus the
				// parts that were skipped.
				if (!preset.problem.empty()) {
					std::string problem = preset.usable ? preset.file + ", skipped: " + preset.problem :
					                                      preset.file + ": " + preset.problem;
					if (preset.skipped > 1) {
						problem += " (" + std::to_string(preset.skipped - 1) + " more in the log)";
					}
					Im::PushStyleColor(kColText, kAmber);
					Im::TextWrapped("      %s", problem.c_str());
					Im::PopStyleColor();
				}

				Im::PopID();

				// The refresh can re-read the folder, which replaces the list being walked.
				if (clicked) {
					if (ticked) {
						Camera::ApplyInstalledPreset(preset);
					}
					RefreshPresetState(true);
					break;
				}
			}
		}

		void RenderExport()
		{
			static std::array<char, 64>  name{};
			static std::array<char, 64>  author{};
			static std::array<char, 200> description{};
			static Camera::ExportResult  result{};

			Im::Spacing();
			Im::PushStyleColor(kColText, kBlue);
			Im::SeparatorText("EXPORT");
			Im::PopStyleColor();

			Note("Saves the current settings as a preset file, to share or to package as a mod.");

			Im::PushID("export");

			Icon(kIconRename, kMuted);
			Im::SetNextItemWidth(220.0f);
			Im::InputTextWithHint("Name", "e.g. Tavern Talk", name.data(), name.size());

			Icon(kIconRename, kMuted);
			Im::SetNextItemWidth(220.0f);
			Im::InputTextWithHint("Author", "optional", author.data(), author.size());

			Icon(kIconRename, kMuted);
			Im::SetNextItemWidth(320.0f);
			Im::InputTextWithHint("Description", "optional, one line", description.data(), description.size());

			const bool named = name[0] != '\0';

			// Exporting under a name that's already installed replaces that file.
			const bool replaces = named && Camera::ExportReplaces(name.data());

			Im::BeginDisabled(!named);
			Icon(kIconSave, named ? kBlue : kMuted);
			if (Im::Button(replaces ? "Replace Installed File" : "Export Current Settings")) {
				result = Camera::ExportPreset(name.data(), author.data(), description.data());
				RefreshPresetState(true);
			}
			Im::EndDisabled();

			if (!result.message.empty()) {
				if (result.ok) {
					Icon(kIconCheck, kGreen);
					Im::PushStyleColor(kColText, kGreen);
					Im::TextWrapped("%s", result.message.c_str());
					Im::PopStyleColor();
					Note("With Mod Organizer it's in Overwrite. Move it into a mod to keep it, "
						 "or zip that mod to share it.");
				} else {
					Warning(result.message.c_str());
				}
			}

			Im::PopID();
		}

		void __stdcall RenderPresets()
		{
			const Compact compact;

			RefreshPresetState(false);

			PageHeader(kIconPresets, "PRESETS");

			// The one note on this page: applying a look overwrites the Shots page.
			Note("Ticking one rewrites the Shots page. Nothing is locked afterwards.");
			Im::Spacing();

			const auto presets = Camera::AllPresets();

			// One checkbox per look; ticking it applies it. Nothing stores which preset is
			// on: it's worked out by comparing settings, so applying one makes the others
			// untick themselves. A saved slot in use also clears the active preset, so the
			// two lists never both claim to be running.
			for (std::size_t i = 0; i < presetState.count && i < presets.size(); ++i) {
				const auto& preset = presets[i];
				const bool  current = presetState.active == &preset;

				Im::PushID(preset.key);

				Icon(current ? kIconCheck : kIconPresets, current ? kGreen : kMuted);

				Im::PushStyleColor(kColText, current ? kGreen : 0xFFCCCCCCu);
				Im::PushStyleColor(kColCheck, kGreen);

				bool ticked = current;
				if (Im::Checkbox(preset.name, &ticked)) {
					if (ticked) {
						Camera::ApplyPreset(preset);
					}

					// Unticking isn't an action; the refresh puts the tick back because the
					// settings still match.
					RefreshPresetState(true);
				}

				Im::PopStyleColor(2);

				Im::PushStyleColor(kColText, kMuted);
				Im::TextWrapped("      %s", preset.summary);
				Im::PopStyleColor();

				Im::PopID();
			}

			RenderInstalledPresets();
			RenderCustomSlots();
			RenderExport();

			Im::Spacing();
			Im::Separator();
			Note("No menu? SD.ini, [Presets] sApply=close (or an installed preset's name) - "
				 "applies once, then clears.");
		}

		// ---- About -------------------------------------------------------------

		void __stdcall RenderAbout()
		{
			const Compact compact;

			PageHeader(kIconAbout, "CINEMATIC CONVERSATION CAMERA");
			Note("by hashhbbrown");
			Im::Spacing();

			Im::TextWrapped(
				"Skyrim points one camera at a conversation and leaves it there. "
				"Whoever is speaking, whatever they are saying, however long it goes "
				"on - the shot never changes. You are not in the scene, you are "
				"standing next to it.");
			Im::Spacing();

			Im::TextWrapped(
				"Every dialogue camera mod before this one describes the camera as an "
				"offset from the player. An offset can orbit you, raise you, tighten "
				"on you - but it can never stand behind the person you are talking "
				"to. That single limitation is why no Skyrim mod has ever cut to a "
				"reverse shot.");
			Im::Spacing();

			Im::TextWrapped(
				"This one solves for a position in the world instead, using where "
				"both people actually are. Once the camera is free of you it can do "
				"what a film crew does: shoot over your shoulder, cut to theirs, hold "
				"the two of you while you read your options, creep closer when a "
				"line is delivered with weight. A conversation that is cut, rather "
				"than merely watched.");
			Im::Spacing();

			// Diagnostics (iPoseMode, bLogFaceAnim) and [Lighting] aren't shown here on
			// purpose. They default when the key is missing and stay inert unless set by
			// hand.

			Im::Separator();

			if (Camera::Director::Staging()) {
				Icon(kIconCheck, kGreen);
				Im::PushStyleColor(kColText, kGreen);
				Im::TextUnformatted("Staging: active");
				Im::PopStyleColor();
			} else {
				Note("Staging: idle");
			}

			Note("Settings are written to SKSE/Plugins/SD_user.ini and applied immediately. "
				 "Updates replace SD.ini but never that file.");
			Note("Sliders persist when released, not while dragging.");

			Im::SeparatorText("Reset");

			Icon(kIconReset, kBlue);
			if (Im::Button("Restore Defaults")) {
				// Reset every key the Camera and Screen pages can write. Values come from a
				// default-constructed Tunables, so they can't drift from the shipped defaults.
				const Camera::Tunables defaults{};
				Camera::Director::ApplyTunables(defaults);

				Config::SetInt("Direction", "iCutEveryMin", defaults.cutEveryMin);
				Config::SetInt("Direction", "iCutEveryMax", defaults.cutEveryMax);
				Config::SetBool("Direction", "bPerLineAngleChange", defaults.perLineAngleChange);
				Config::SetBool("Direction", "bHoldOnShortLines", defaults.holdOnShortLines);
				Config::SetInt("Direction", "iShortLineWords", defaults.shortLineWords);
				Config::SetBool("Direction", "bTimedCutsWhileSpeaking",
					defaults.timedCutsWhileSpeaking);
				Config::SetBool("Direction", "bTimedCutsWhileChoosing",
					defaults.timedCutsWhileChoosing);
				Config::SetInt("Direction", "iMinShotTime", defaults.minShotTime);
				Config::SetInt("Direction", "iMinTurnTime", defaults.minTurnTime);
				Config::SetInt("Direction", "iMaxShotTime", defaults.maxShotTime);
				Config::SetInt("Direction", "iPlayerBeat", defaults.playerBeat);
				Config::SetInt("Direction", "iPlayerVoiceHold", defaults.playerVoiceHold);
				Config::SetBool("Direction", "bReactionShots", defaults.reactionShots);
				Config::SetInt("Direction", "iReactionEvery", defaults.reactionEvery);
				Config::SetInt("Direction", "iReactionChance", defaults.reactionChance);
				Config::SetBool("Direction", "bKeepSubjectVisible", defaults.protectSubject);
				Config::SetBool("Direction", "bFirstPersonFallback", defaults.firstPersonFallback);
				Config::SetBool("Direction", "bHoldPlacement", defaults.holdPlacement);
				Config::SetBool("Direction", "bTrue180", defaults.true180);
				Config::SetBool("Direction", "bFollowFace", defaults.followFace);
				Config::SetBool("Performance", "bPlayerGestureCue", defaults.playerGestureCue);
				Config::SetBool("Performance", "bStopWorkToTalk", defaults.stopWorkToTalk);
				Config::SetBool("Direction", "bEnforceLine", defaults.enforceLine);
				Config::SetBool("Direction", "bLetterbox", defaults.letterbox);
				Config::SetInt("Direction", "iLetterboxHeight", defaults.letterboxHeight);
				Config::SetBool("Direction", "bFadeTopicList", defaults.fadeTopicList);
				Config::SetBool("Direction", "bHideSpeakerName", defaults.hideSpeakerName);
				Config::SetBool("Direction", "bFadeAfterPlayerLine", defaults.fadeAfterPlayerLine);
				Config::SetInt("Direction", "iChoiceFadeDelay", defaults.choiceFadeDelay);
				Config::SetInt("Direction", "iChoiceFadeTime", defaults.choiceFadeTime);
				Config::SetBool("Direction", "bPersuasionBeat", defaults.persuasionBeat);
				Config::SetBool("Direction", "bSubtitlesInBar", defaults.subtitlesInBar);
				Config::SetBool("Direction", "bFilmScenesAuto", defaults.sceneAuto);
				Config::SetInt("Direction", "iSceneRange", defaults.sceneRange);
				Config::SetInt("Direction", "iSceneWait", defaults.sceneWait);

				Log::Info(Log::Category::kCore, "Direction dials reset to defaults from the menu."sv);
			}
			Note("Does not touch the Shots page. Use a preset for that.");
		}
	}

	void Settings::Register()
	{
		if (!MF::IsInstalled()) {
			Log::Info(Log::Category::kCore,
				"SKSE Menu Framework not present; SD.ini is the only settings interface."sv);
			return;
		}

		MF::SetSection("Cinematic Conversation Camera");

		// Kept alive for the process; the wrapper unregisters in its destructor.
		static auto* menuInput = MF::AddInputEvent(OnMenuInput);
		static_cast<void>(menuInput);

		// In the order a new player would use them: pick a look, tune the camera, pick
		// angles, tidy the screen, then the rest.
		//
		// The Light page isn't registered. The lamps still exist (Scene::LightRig,
		// [Lighting] in SD.ini) and work when [Lighting] bLights=1 is set by hand;
		// per-angle lighting shows in the shot editor with [Lighting] bPerShot=1.
		MF::AddSectionItem("Presets", RenderPresets);
		MF::AddSectionItem("Camera", RenderCamera);
		MF::AddSectionItem("Shots", RenderShots);
		MF::AddSectionItem("Screen", RenderScreen);
		MF::AddSectionItem("Faces", RenderFaces);
		MF::AddSectionItem("Keys", RenderControls);
		MF::AddSectionItem("About", RenderAbout);

		Log::Info(Log::Category::kCore, "SKSE Menu Framework detected; settings panel registered."sv);
	}
}

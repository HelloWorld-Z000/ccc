#pragma once

#include "SD/Camera/Shot.h"

namespace SD::Camera
{
	// Named ways of shooting a conversation. A preset sets the cutting rhythm,
	// which setups are in play, and for each one its lens, move, weight and
	// lighting. It's applied once by writing every key it covers, then stops
	// existing: nothing consults it afterwards, so everything stays individually
	// editable.
	struct Style
	{
		int minShotTime;
		int maxShotTime;
		int cutEveryMin;
		int cutEveryMax;

		// perLineAngleChange: whether the line count above may ask for a new angle at
		// all.
		bool perLineAngleChange;
		bool holdOnShortLines;
		bool timedCutsWhileSpeaking;
		bool timedCutsWhileChoosing;
	};

	// What a preset asks one setup to do while it's on screen. Two looks can draw
	// from overlapping setups and still feel nothing alike because of this.
	struct Motion
	{
		ShotType shot;
		Move     move;
		int      amount;  // 0-100 of that move's own travel
		int      time;    // hundredths of a second
	};

	// The lens for one setup in one look. Separate from Motion because most
	// entries change the lens without changing the move.
	//
	// Distance is solved for the fill at the chosen lens and then floored at the
	// subject's minimum, so a tight fill on a wide lens gets clamped and renders
	// looser than intended. Roughly: a ceiling near 66 degrees at 0.85 fill, 80 at
	// 0.68, no constraint below about 0.46. Over-the-shoulders are exempt.
	struct Lens
	{
		ShotType shot;
		int      degrees;  // a REAL horizontal field of view, kMinLens..kMaxLens
	};

	// How often each setup is drawn, per look. The table's weights are meant for
	// the whole vocabulary, not a short preset list. Weight also decides how much
	// of a conversation goes to the room, since Coverage() rolls the environmental
	// pool's total against the coverage pool's.
	struct Weight
	{
		ShotType shot;
		int      weight;  // 0-100, as the slider reads it. 0 is never.
	};

	// How each setup is lit, per preset. Only used with [Lighting] bPerShot on,
	// but always written so turning that on later gives a coherent set. Stored as
	// a look key, like Shot::LightKey.
	struct Light
	{
		ShotType    shot;
		const char* look;  // a Scene::LookSpec key
	};

	struct Preset
	{
		// The ini token. Never rename one: an existing [Presets] sApply line would
		// stop resolving.
		const char* key;

		const char* name;     // menu label
		const char* summary;  // one line, under the label
		const char* detail;   // what the camera will actually do

		Style                     style;
		std::span<const ShotType> shots;  // everything absent from this is switched off

		// What every setup in this preset does unless `motion` overrides it, and how
		// strongly.
		Move baseMove;
		int  baseAmount;
		int  baseTime;

		// The exceptions, by name. Anything not listed takes the baseline above.
		std::span<const Motion> motion;

		// Lens per setup. Anything not listed gets the table's own lens back when the
		// preset is applied, so a look is reproducible.
		std::span<const Lens> lenses;

		// How often each setup comes up. Same fallback rule as the lens.
		std::span<const Weight> weights;

		// Lighting per setup. Same fallback rule: unlisted setups get their shipped
		// look.
		std::span<const Light> lights;
	};

	[[nodiscard]] std::span<const Preset> AllPresets();
	[[nodiscard]] const Preset*           FindPreset(std::string_view a_key);

	// The look a clean install starts in (Close), and the fallback for every
	// per-setup key in the settings loader, so a fresh profile gets Close's
	// lenses, weights, moves and lighting even with a minimal SD.ini.
	[[nodiscard]] const Preset& DefaultPreset();

	// Whether this preset switches this setup on.
	[[nodiscard]] bool PresetUses(const Preset& a_preset, ShotType a_type);

	// What this preset asks this shot to do: its own exception if it has one,
	// otherwise the baseline. Shared by apply, drift and the menu.
	[[nodiscard]] Motion PresetMotion(const Preset& a_preset, ShotType a_type);

	// The lens this preset uses for this setup, in degrees. Same contract as
	// PresetMotion.
	[[nodiscard]] int PresetLens(const Preset& a_preset, ShotType a_type);

	// How often this preset draws this setup. Same contract.
	[[nodiscard]] int PresetWeight(const Preset& a_preset, ShotType a_type);

	// The lighting rig key for this setup. Same contract; never null.
	[[nodiscard]] const char* PresetLight(const Preset& a_preset, ShotType a_type);

	// How many settings differ between a preset and what's live. Always derived: a
	// preset doesn't exist after it's applied, so the active one is worked out
	// from the settings.
	[[nodiscard]] int PresetDrift(const Preset& a_preset);

	// The preset the current settings match exactly, or nullptr.
	[[nodiscard]] const Preset* ActivePreset();

	// Write every key the preset covers to SD.ini and apply it live. Leaves the
	// letterbox height, gaze split and diagnostic pose mode alone.
	void ApplyPreset(const Preset& a_preset);

	// ---- Saved presets -----------------------------------------------------
	//
	// Three slots the player fills from the current settings. Kept separate from
	// the built-in looks: a built-in is a designed set, a slot is a snapshot.
	inline constexpr int kCustomSlots = 3;

	struct CustomSlot
	{
		int         index{ 0 };  // 0-based
		std::string name;        // empty means the slot is free
		bool        used{ false };
	};

	[[nodiscard]] CustomSlot ReadCustomSlot(int a_index);

	// Which slot the live settings match exactly, or -1. Takes precedence over
	// ActivePreset in the menu.
	[[nodiscard]] int ActiveCustomSlot();

	// Snapshot everything a preset covers into a slot: the cutting settings and
	// every shot's on/off, move, amount, duration, lens and weight. Must cover
	// everything ApplyPreset writes, or the slot can't reproduce its look.
	void SaveCustomSlot(int a_index, std::string_view a_name);

	void ApplyCustomSlot(int a_index);
	void RenameCustomSlot(int a_index, std::string_view a_name);
	void DeleteCustomSlot(int a_index);

	// Apply [Presets] sApply if it names a preset (built-in or installed), for
	// setups without SKSE Menu Framework. The key is cleared afterwards so it
	// only runs once.
	void ApplyPendingPreset();

	// ---- Preset files ------------------------------------------------------
	//
	// Presets as JSON files in Data/SKSE/Plugins/SceneDirector/Presets/, so they
	// can be shared and installed as their own mods. A file holds one preset or a
	// list of them; the format is described in PresetFile.h and USAGE.md. Angles
	// a preset lists are on and every other angle is off; anything else it leaves
	// out uses the angle's own default, or Close's cutting. Menu thread and
	// startup only.
	struct InstalledPreset
	{
		struct Entry
		{
			bool on{ false };
			Move move{ Move::kLocked };
			int  amount{ 0 };
			int  time{ 0 };
			int  lens{ 0 };
			int  weight{ 0 };
			int  light{ 0 };  // index into Scene::AllLooks
		};

		std::wstring path;
		std::string  file;  // file name with extension, UTF-8
		std::string  stem;  // without the extension
		std::string  name;
		std::string  author;
		std::string  description;

		// Why it can't be applied, or the first part of it that was skipped. The
		// rest are in the log.
		std::string problem;
		int         skipped{ 0 };
		bool        usable{ false };

		Style                                                         style{};
		std::array<Entry, static_cast<std::size_t>(ShotType::kCount)> shots{};
		int                                                           shotsOn{ 0 };
	};

	// The folder preset files are read from and exported to.
	[[nodiscard]] std::wstring PresetFolder();

	// Re-reads the folder. Without a_force, only if the last scan is a few seconds
	// old, so the Presets page can call it every refresh.
	void RescanPresetFiles(bool a_force);

	// Sorted by name. Unreadable files are listed too, with usable false.
	[[nodiscard]] const std::vector<InstalledPreset>& InstalledPresets();

	// By preset name, then file name; case-insensitive. Null if not installed.
	[[nodiscard]] const InstalledPreset* FindInstalledPreset(std::string_view a_name);

	[[nodiscard]] int InstalledPresetDrift(const InstalledPreset& a_preset);

	// Index into InstalledPresets() of the one the live settings match, or -1.
	[[nodiscard]] int ActiveInstalledPreset();

	// Writes everything the preset covers to SD_user.ini and applies it live.
	void ApplyInstalledPreset(const InstalledPreset& a_preset);

	struct ExportResult
	{
		bool        ok{ false };
		std::string message;  // where it was saved, or what went wrong
	};

	// Whether exporting under this name would replace a file that's installed.
	[[nodiscard]] bool ExportReplaces(std::string_view a_name);

	// Saves the current settings as <name>.json in PresetFolder(), replacing a
	// file of the same name.
	[[nodiscard]] ExportResult ExportPreset(std::string_view a_name, std::string_view a_author,
		std::string_view a_description);
}

#pragma once

#include "SD/Core/SettingsCache.h"

namespace SD::Config
{
	// Checks the three ini revisions once, then reuses native read results for the
	// batch. Nested batches share the snapshot; menu writes invalidate it.
	class ReadScope
	{
	public:
		ReadScope();
	private:
		SettingsCache::Scope scope;
	};
	// An absolute path to something under the game's Data folder, for the profile
	// API. Used to read Improved Camera's config as well as SD's own files. Built
	// from the same exe-derived root as the settings and returned wide (see
	// Config.cpp). Empty in, or an unknown root, gives empty out.
	[[nodiscard]] std::wstring DataPath(std::wstring_view a_relative);

	// Settings, read from whichever source has them: MCM Helper's
	// Data/MCM/Settings/SceneDirector.ini first, then SD_user.ini, then
	// SKSE/Plugins/SD.ini. Neither MCM Helper nor the menu is required.
	[[nodiscard]] int  Int(const char* a_section, const char* a_key, int a_default);
	[[nodiscard]] bool Bool(const char* a_section, const char* a_key, bool a_default);

	// Text values. Only used for the preset requested by name in the ini; an index
	// would point at a different preset if the list were reordered.
	[[nodiscard]] std::string String(const char* a_section, const char* a_key, const char* a_default);

	// Save a value to SD_user.ini. The menu applies the live effect separately, so
	// a slider doesn't wait for the next conversation.
	void SetInt(const char* a_section, const char* a_key, int a_value);
	void SetBool(const char* a_section, const char* a_key, bool a_value);
	void SetString(const char* a_section, const char* a_key, const char* a_value);

	// Logs which source answered, once.
	void ReportSource();
}

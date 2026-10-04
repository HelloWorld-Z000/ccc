#include "SD/Compat/ImprovedCamera.h"

#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"

namespace SD::Compat
{
	namespace
	{
		bool present{ false };
		bool asked{ false };

		// Which generation was found, since they don't share a config layout. Null
		// means nothing matched.
		const wchar_t* matched{ nullptr };

		// Detected by loaded module, like Conflicts.cpp. Both file names: the original
		// ships ImprovedCamera.dll, Improved Camera SE (and its NG builds) ships
		// ImprovedCameraSE.dll.
		constexpr std::array kModules{
			L"ImprovedCameraSE.dll",
			L"ImprovedCamera.dll",
		};

		// Where Improved Camera keeps its settings. ImprovedCameraSE.ini is the
		// loader's config; the camera settings are in Profiles\<ProfileName>, where
		// [MODULE DATA] ProfileName names the file (Default.ini by default). The name
		// includes its extension.
		[[nodiscard]] std::wstring ProfilePath()
		{
			const auto loader = Config::DataPath(L"SKSE\\Plugins\\ImprovedCameraSE\\ImprovedCameraSE.ini");
			if (loader.empty()) {
				return {};
			}

			// Check the loader file exists first: GetPrivateProfileStringW returns the
			// default when the file is missing, which would produce a plausible but wrong
			// path.
			std::error_code ec{};
			if (!std::filesystem::exists(std::filesystem::path{ loader }, ec) || ec) {
				return {};
			}

			wchar_t name[MAX_PATH]{};
			::GetPrivateProfileStringW(L"MODULE DATA", L"ProfileName", L"Default.ini",
				name, static_cast<DWORD>(std::size(name)), loader.c_str());

			if (!name[0]) {
				return {};
			}

			std::wstring relative = L"SKSE\\Plugins\\ImprovedCameraSE\\Profiles\\";
			relative += name;
			return Config::DataPath(relative);
		}

		// The one setting that decides whether these two mods can share a camera.
		//
		// Skyrim disables movement controls during dialogue, which Improved Camera
		// treats as a "scripted" third-person event:
		//
		//     if (Helper::IsScripted() || !controlMap->IsMovementControlsEnabled() ||
		//         Helper::CorrectFurnitureIdle())
		//         m_ThirdPersonState = CameraThirdPerson::State::kScriptedEnter;
		//
		// With [EVENTS] bScripted on (the default), a first-person player gets
		// Improved Camera's fake first person during every conversation: it pins the
		// zoom and moves the camera to the player's head every frame, while this mod
		// is driving its own shot. The view flips between the two.
		//
		// Reported rather than worked around: 1.1.x exposes nothing to negotiate with
		// (only the SKSE entry points, and it ignores SmoothCam's refusal).
		//
		// bScripted=0 turns off Improved Camera's handling for the whole
		// scripted-third-person category, not just dialogue; the warning says so.
		//
		// Only relevant in first person, so third-person players aren't warned.
		void ReportScriptedEvent()
		{
			// The original Improved Camera has a different layout; don't read Improved
			// Camera SE's schema for it.
			if (matched && std::wstring_view{ matched } == L"ImprovedCamera.dll") {
				Log::Warn(Log::Category::kCompat,
					"This is the original Improved Camera, not Improved Camera SE. Scene Director "
					"knows the SE configuration layout only, so it cannot tell you whether the "
					"conflicting scripted-camera setting is on. If conversations fight the camera "
					"in first person, look for that mod's scripted forced-third-person option."sv);
				return;
			}

			const auto profile = ProfilePath();
			if (profile.empty()) {
				Log::Warn(Log::Category::kCompat,
					"Improved Camera's profile could not be located, so its [EVENTS] bScripted "
					"setting is unknown. If conversations fight the camera in first person, set "
					"it to 0."sv);
				return;
			}

			// -1 as the default, so a missing key isn't reported as "configured
			// correctly".
			const int scripted = ::GetPrivateProfileIntW(L"EVENTS", L"bScripted", -1, profile.c_str());

			if (scripted < 0) {
				Log::Warn(Log::Category::kCompat,
					"Improved Camera's profile was found but has no [EVENTS] bScripted key, so "
					"its setting is unknown. If conversations fight the camera in first person, "
					"set it to 0."sv);
				return;
			}

			if (scripted == 0) {
				Log::Info(Log::Category::kCompat,
					"Improved Camera's [EVENTS] bScripted is off. It leaves the camera alone "
					"during dialogue, and the two mods will not fight."sv);
				return;
			}

			Log::Warn(Log::Category::kCompat,
				"Improved Camera's [EVENTS] bScripted is ON. Skyrim disables movement controls "
				"during dialogue, which Improved Camera reads as a scripted third-person event, "
				"so IF YOU PLAY IN FIRST PERSON it will pin the zoom and pull the camera back to "
				"your head on every line while Scene Director is staging the shot. The view "
				"flips between the two. Setting bScripted=0 fixes it. Note what that costs: the "
				"key is Improved Camera's whole scripted-forced-third-person category, not a "
				"dialogue switch, so its first-person handling goes for every event in that "
				"category. Dialogue is the one that conflicts here."sv);

			// Log the path, since the file to edit isn't the one named after the mod.
			// Log::Utf8, never path::string(): this is under MO2's virtual Data folder.
			Log::Warn(Log::Category::kCompat, "The file to edit is: {}"sv, Log::Utf8(profile));
		}
	}

	void ImprovedCamera::Detect()
	{
		if (asked) {
			return;
		}
		asked = true;

		for (const auto* module : kModules) {
			if (::GetModuleHandleW(module) != nullptr) {
				present = true;
				matched = module;
				break;
			}
		}

		if (!present) {
			return;
		}

		// Logged once at startup in full.
		Log::Info(Log::Category::kCompat,
			"Improved Camera is loaded. The third-person zoom will not be written back at the "
			"end of conversations: that zoom is how Improved Camera moves between first and "
			"third person, and a second author on it is what makes the view pump."sv);

		ReportScriptedEvent();
	}

	bool ImprovedCamera::Present() noexcept
	{
		return present;
	}
}

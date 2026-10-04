#include "SD/Camera/Presets.h"

#include "SD/Camera/Director.h"
#include "SD/Camera/PresetFile.h"
#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/LightRig.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

namespace SD::Camera
{
	namespace
	{
		namespace fs = std::filesystem;

		std::vector<InstalledPreset>          installed;
		std::chrono::steady_clock::time_point lastScan{};
		bool                                  scanned{ false };
		std::size_t                           lastLoggedCount{ static_cast<std::size_t>(-1) };

		// Each file's write time when its problems were last logged, so the rescan
		// every few seconds doesn't repeat them.
		std::map<std::wstring, fs::file_time_type> logged;

		constexpr auto kRescanInterval = std::chrono::seconds(5);

		// A file bigger than this isn't a preset.
		constexpr std::uintmax_t kMaxFileBytes = 1u << 20;

		// What the Shots page stores for an angle whose effect is None.
		constexpr int kInertAmount = 0;
		constexpr int kInertTime = 400;

		// Effect names besides the menu's, for files written by hand. Same order as
		// Move.
		constexpr std::array<std::string_view, 14> kMoveIds{ "Locked", "PushIn", "PullOut", "CraneUp",
			"CraneDown", "TiltUp", "TiltDown", "Drift", "ZoomIn", "ZoomOut", "OrbitLeft", "OrbitRight",
			"TruckLeft", "TruckRight" };
		static_assert(kMoveIds.size() == static_cast<std::size_t>(Move::kCount),
			"Every move needs a name here, in enum order.");

		// The names a file may use. Angles go by their settings key without the
		// "b" (CloseUp, ExtremeClosePlayer), since the menu reuses names like
		// "Close Up" on both sides. Effects and lights go by their menu names.
		[[nodiscard]] const PresetFile::Vocabulary& Words()
		{
			static const PresetFile::Vocabulary words = [] {
				PresetFile::Vocabulary out;
				for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
					out.angles.push_back({ std::string{ Key(static_cast<ShotType>(i)) }.substr(1) });
				}
				for (std::size_t i = 0; i < kMoveIds.size(); ++i) {
					const auto move = static_cast<Move>(i);
					auto&      names = out.effects.emplace_back();
					if (move == Move::kLocked) {
						names.emplace_back("None");
					}
					names.emplace_back(MoveLabel(move));
					names.emplace_back(kMoveIds[i]);
				}
				for (const auto& look : Scene::AllLooks()) {
					out.lights.push_back({ look.key, look.name });
				}
				return out;
			}();
			return words;
		}

		[[nodiscard]] fs::path Utf8Path(std::string_view a_text)
		{
			return fs::path{ std::u8string{ reinterpret_cast<const char8_t*>(a_text.data()), a_text.size() } };
		}

		[[nodiscard]] std::optional<std::string> ReadText(const fs::path& a_path)
		{
			std::error_code ec;
			const auto      size = fs::file_size(a_path, ec);
			if (ec || size > kMaxFileBytes) {
				return std::nullopt;
			}
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return std::nullopt;
			}
			std::string text(static_cast<std::size_t>(size), '\0');
			file.read(text.data(), static_cast<std::streamsize>(size));
			text.resize(static_cast<std::size_t>(file.gcount()));
			return text;
		}

		[[nodiscard]] InstalledPreset Named(const fs::path& a_path)
		{
			InstalledPreset preset{};
			preset.path = a_path.wstring();
			preset.file = Log::Utf8(a_path.filename().wstring());
			preset.stem = Log::Utf8(a_path.stem().wstring());
			preset.name = preset.stem;
			return preset;
		}

		[[nodiscard]] int AuthoredLook(ShotType a_type)
		{
			const int look = Scene::FindLook(AuthoredLight(a_type));
			return look >= 0 ? look : Scene::DefaultLook();
		}

		[[nodiscard]] InstalledPreset Resolve(const PresetFile::Preset& a_source, const fs::path& a_path)
		{
			auto preset = Named(a_path);
			if (!a_source.name.empty()) {
				preset.name = a_source.name;
			}
			preset.author = a_source.author;
			preset.description = a_source.description;

			// Cutting left out of the file uses Close's.
			const auto& base = DefaultPreset().style;
			const auto& cut = a_source.cutting;
			auto&       style = preset.style;
			style.perLineAngleChange = cut.changeAnglePerLine.value_or(base.perLineAngleChange);
			style.cutEveryMin = std::clamp(cut.minimumLines.value_or(base.cutEveryMin), 1, 20);
			style.cutEveryMax = std::clamp(cut.maximumLines.value_or(base.cutEveryMax), style.cutEveryMin, 20);
			style.holdOnShortLines = cut.ignoreShortLines.value_or(base.holdOnShortLines);
			style.timedCutsWhileSpeaking = cut.timerWhileTalking.value_or(base.timedCutsWhileSpeaking);
			style.timedCutsWhileChoosing = cut.timerWhileChoosing.value_or(base.timedCutsWhileChoosing);
			style.maxShotTime = std::clamp(cut.changeAfter.value_or(base.maxShotTime), 100, 2000);
			style.minShotTime = std::clamp(cut.shortestHold.value_or(base.minShotTime), 30, 900);

			for (const auto& angle : a_source.angles) {
				if (angle.angle < 0 || angle.angle >= static_cast<int>(ShotType::kCount)) {
					continue;
				}
				const auto type = static_cast<ShotType>(angle.angle);
				auto&      entry = preset.shots[static_cast<std::size_t>(angle.angle)];

				entry.on = true;
				entry.move = angle.effect ? static_cast<Move>(*angle.effect) : Shot::AuthoredMove(type);
				if (entry.move == Move::kLocked) {
					entry.amount = kInertAmount;
					entry.time = kInertTime;
				} else {
					entry.amount = std::clamp(angle.amount.value_or(Shot::AuthoredMoveAmount(type)), 0, 100);
					entry.time = std::clamp(angle.duration.value_or(Shot::AuthoredMoveTime(type)), 30, 900);
				}
				entry.lens = std::clamp(angle.fov.value_or(static_cast<int>(std::lround(AuthoredLens(type)))),
					kMinLens, kMaxLens);
				entry.weight = std::clamp(angle.frequency.value_or(AuthoredWeight(type)), 0, 100);
				entry.light = angle.light ? *angle.light : AuthoredLook(type);
			}

			for (const auto& entry : preset.shots) {
				preset.shotsOn += entry.on ? 1 : 0;
			}

			preset.usable = preset.shotsOn > 0;
			preset.skipped = static_cast<int>(a_source.warnings.size());
			if (!preset.usable) {
				preset.problem = "Lists no angles, so it would switch every angle off.";
			} else if (!a_source.warnings.empty()) {
				preset.problem = a_source.warnings.front();
			}
			return preset;
		}

		// Reads one file into a_out: one entry per preset, or one unusable entry
		// saying why the file can't be read.
		void ReadFile(const fs::path& a_path, std::vector<InstalledPreset>& a_out)
		{
			const auto      name = Log::Utf8(a_path.filename().wstring());
			std::error_code ec;
			const auto      stamp = fs::last_write_time(a_path, ec);
			const auto      seen = logged.find(a_path.wstring());
			const bool      report = ec || seen == logged.end() || seen->second != stamp;
			if (!ec) {
				logged[a_path.wstring()] = stamp;
			}

			const auto text = ReadText(a_path);
			if (!text) {
				auto broken = Named(a_path);
				broken.problem = "Couldn't be read.";
				a_out.push_back(std::move(broken));
				if (report) {
					Log::Warn(Log::Category::kCamera, "Preset file {} couldn't be read."sv, name);
				}
				return;
			}

			const auto file = PresetFile::Read(*text, Words());
			if (!file.error.empty()) {
				auto broken = Named(a_path);
				broken.problem = "Not a valid preset file: " + file.error;
				a_out.push_back(std::move(broken));
				if (report) {
					Log::Warn(Log::Category::kCamera, "Preset file {} can't be used: {}"sv, name, file.error);
				}
				return;
			}

			for (const auto& preset : file.presets) {
				a_out.push_back(Resolve(preset, a_path));
			}

			if (report) {
				Log::Info(Log::Category::kCamera, "Preset file {}: {} preset(s)."sv, name, file.presets.size());
				for (const auto& warning : file.warnings) {
					Log::Warn(Log::Category::kCamera, "Preset file {}: {}"sv, name, warning);
				}
				for (const auto& preset : file.presets) {
					for (const auto& warning : preset.warnings) {
						Log::Warn(Log::Category::kCamera, "Preset file {}, '{}': {}"sv, name,
							preset.name.empty() ? "unnamed" : preset.name, warning);
					}
				}
			}
		}

		[[nodiscard]] bool SameText(std::string_view a_left, std::string_view a_right)
		{
			return PresetFile::Lower(a_left) == PresetFile::Lower(a_right);
		}
	}

	std::wstring PresetFolder()
	{
		return Config::DataPath(L"SKSE\\Plugins\\SceneDirector\\Presets");
	}

	void RescanPresetFiles(bool a_force)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!a_force && scanned && now - lastScan < kRescanInterval) {
			return;
		}
		scanned = true;
		lastScan = now;

		std::vector<InstalledPreset> found;
		std::size_t                  files = 0;
		const auto                   folder = PresetFolder();
		std::error_code              ec;
		if (!folder.empty() && fs::is_directory(folder, ec)) {
			// Error codes throughout: a throw here would take the game down with it.
			for (fs::directory_iterator it{ folder, ec }, end; !ec && it != end; it.increment(ec)) {
				const auto&     item = *it;
				std::error_code itemError;
				if (!item.is_regular_file(itemError) || !SameText(Log::Utf8(item.path().extension().wstring()), ".json")) {
					continue;
				}
				++files;
				ReadFile(item.path(), found);
			}
		}

		std::stable_sort(found.begin(), found.end(), [](const InstalledPreset& a_left, const InstalledPreset& a_right) {
			return PresetFile::Lower(a_left.name) < PresetFile::Lower(a_right.name);
		});
		installed = std::move(found);

		if (installed.size() != lastLoggedCount) {
			lastLoggedCount = installed.size();
			Log::Info(Log::Category::kCamera, "{} installed preset(s) from {} file(s) in {}."sv, installed.size(),
				files, Log::Utf8(folder));
		}
	}

	const std::vector<InstalledPreset>& InstalledPresets()
	{
		return installed;
	}

	const InstalledPreset* FindInstalledPreset(std::string_view a_name)
	{
		for (const auto& preset : installed) {
			if (preset.usable && SameText(preset.name, a_name)) {
				return &preset;
			}
		}
		for (const auto& preset : installed) {
			if (preset.usable && (SameText(preset.stem, a_name) || SameText(preset.file, a_name))) {
				return &preset;
			}
		}
		return nullptr;
	}

	int InstalledPresetDrift(const InstalledPreset& a_preset)
	{
		const auto& s = a_preset.style;
		const auto  live = Director::GetTunables();
		int         drift = 0;

		const auto differs = [&drift](auto a_have, auto a_want) {
			if (a_have != a_want) {
				++drift;
			}
		};

		differs(live.minShotTime, s.minShotTime);
		differs(live.maxShotTime, s.maxShotTime);
		differs(live.cutEveryMin, s.cutEveryMin);
		differs(live.cutEveryMax, s.cutEveryMax);
		differs(live.perLineAngleChange, s.perLineAngleChange);
		differs(live.holdOnShortLines, s.holdOnShortLines);
		differs(live.timedCutsWhileSpeaking, s.timedCutsWhileSpeaking);
		differs(live.timedCutsWhileChoosing, s.timedCutsWhileChoosing);

		for (std::size_t i = 0; i < a_preset.shots.size(); ++i) {
			const auto  type = static_cast<ShotType>(i);
			const auto& entry = a_preset.shots[i];
			differs(Shot::Enabled(type), entry.on);
			if (!entry.on) {
				continue;  // same rule as PresetDrift: a switched-off angle has no opinion
			}
			differs(Shot::MoveOf(type), entry.move);
			if (entry.move != Move::kLocked) {
				// With no effect these two do nothing, and files leave them out.
				differs(Shot::MoveAmount(type), entry.amount);
				differs(Shot::MoveTime(type), entry.time);
			}
			differs(Shot::Lens(type), entry.lens);
			differs(Shot::Weight(type), entry.weight);
			differs(Shot::LightOf(type), entry.light);
		}
		return drift;
	}

	int ActiveInstalledPreset()
	{
		for (std::size_t i = 0; i < installed.size(); ++i) {
			if (installed[i].usable && InstalledPresetDrift(installed[i]) == 0) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	void ApplyInstalledPreset(const InstalledPreset& a_preset)
	{
		if (!a_preset.usable) {
			Log::Warn(Log::Category::kCamera, "Preset '{}' ({}) not applied: {}"sv, a_preset.name, a_preset.file,
				a_preset.problem);
			return;
		}

		const auto& s = a_preset.style;
		Config::SetInt("Direction", "iMinShotTime", s.minShotTime);
		Config::SetInt("Direction", "iMaxShotTime", s.maxShotTime);
		Config::SetInt("Direction", "iCutEveryMin", s.cutEveryMin);
		Config::SetInt("Direction", "iCutEveryMax", s.cutEveryMax);
		Config::SetBool("Direction", "bPerLineAngleChange", s.perLineAngleChange);
		Config::SetBool("Direction", "bHoldOnShortLines", s.holdOnShortLines);
		Config::SetBool("Direction", "bTimedCutsWhileSpeaking", s.timedCutsWhileSpeaking);
		Config::SetBool("Direction", "bTimedCutsWhileChoosing", s.timedCutsWhileChoosing);

		const auto looks = Scene::AllLooks();
		for (std::size_t i = 0; i < a_preset.shots.size(); ++i) {
			const auto  type = static_cast<ShotType>(i);
			const auto& entry = a_preset.shots[i];

			Config::SetBool("Shots", Key(type), entry.on);
			Shot::SetEnabled(type, entry.on);
			if (!entry.on) {
				continue;  // leave the tuning on switched-off angles alone, like ApplyPreset
			}

			Config::SetInt("Shots", MoveKey(type), static_cast<int>(entry.move));
			Config::SetInt("Shots", MoveAmountKey(type), entry.amount);
			Config::SetInt("Shots", MoveTimeKey(type), entry.time);
			Config::SetInt("Shots", LensKey(type), entry.lens);
			Config::SetInt("Shots", WeightKey(type), entry.weight);
			Shot::SetMove(type, entry.move);
			Shot::SetMoveAmount(type, entry.amount);
			Shot::SetMoveTime(type, entry.time);
			Shot::SetLens(type, entry.lens);
			Shot::SetWeight(type, entry.weight);

			if (entry.light >= 0 && static_cast<std::size_t>(entry.light) < looks.size()) {
				Config::SetString("Shots", LightKey(type), looks[static_cast<std::size_t>(entry.light)].key);
				Shot::SetLight(type, entry.light);
			}
		}

		Director::LoadSettings();

		Log::Info(Log::Category::kCamera, "Preset '{}' from {} applied: {} angle(s) on."sv, a_preset.name,
			a_preset.file, a_preset.shotsOn);
	}

	bool ExportReplaces(std::string_view a_name)
	{
		const auto stem = PresetFile::FileStem(PresetFile::OneLine(a_name));
		for (const auto& preset : installed) {
			if (SameText(preset.stem, stem)) {
				return true;
			}
		}
		return false;
	}

	ExportResult ExportPreset(std::string_view a_name, std::string_view a_author, std::string_view a_description)
	{
		ExportResult result{};

		const auto folder = PresetFolder();
		if (folder.empty()) {
			result.message = "Couldn't find the game's Data folder.";
			return result;
		}

		std::error_code ec;
		fs::create_directories(folder, ec);
		if (ec) {
			result.message = "Couldn't create the presets folder: " + ec.message();
			return result;
		}

		const auto name = PresetFile::OneLine(a_name);
		const auto stem = PresetFile::FileStem(name);
		const auto path = fs::path{ folder } / Utf8Path(stem + ".json");

		PresetFile::Preset preset;
		preset.name = name.empty() ? stem : name;
		preset.author = PresetFile::OneLine(a_author);
		preset.description = PresetFile::OneLine(a_description);

		const auto live = Director::GetTunables();
		auto&      cut = preset.cutting;
		cut.changeAnglePerLine = live.perLineAngleChange;
		cut.minimumLines = live.cutEveryMin;
		cut.maximumLines = live.cutEveryMax;
		cut.ignoreShortLines = live.holdOnShortLines;
		cut.timerWhileTalking = live.timedCutsWhileSpeaking;
		cut.timerWhileChoosing = live.timedCutsWhileChoosing;
		cut.changeAfter = live.maxShotTime;
		cut.shortestHold = live.minShotTime;

		const auto looks = Scene::AllLooks();
		for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
			const auto type = static_cast<ShotType>(i);
			if (!Shot::Enabled(type)) {
				continue;
			}

			PresetFile::Angle angle{};
			angle.angle = static_cast<int>(i);
			angle.frequency = Shot::Weight(type);
			angle.fov = Shot::Lens(type);

			const auto move = Shot::MoveOf(type);
			angle.effect = static_cast<int>(move);
			if (move != Move::kLocked) {
				angle.amount = Shot::MoveAmount(type);
				angle.duration = Shot::MoveTime(type);
			}

			const int look = Shot::LightOf(type);
			angle.light = look >= 0 && static_cast<std::size_t>(look) < looks.size() ? look : AuthoredLook(type);

			preset.angles.push_back(angle);
		}

		const auto text = PresetFile::Write(preset, Words());

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(text.data(), static_cast<std::streamsize>(text.size()));
		file.close();
		if (!file) {
			result.message = "Couldn't write " + stem + ".json.";
			Log::Warn(Log::Category::kCamera, "Preset export failed: couldn't write {}."sv, Log::Utf8(path.wstring()));
			return result;
		}

		result.ok = true;
		result.message = "Saved as SKSE/Plugins/SceneDirector/Presets/" + stem + ".json.";
		Log::Info(Log::Category::kCamera, "Exported preset '{}' ({} angle(s) on) to {}."sv, preset.name,
			preset.angles.size(), Log::Utf8(path.wstring()));

		RescanPresetFiles(true);
		return result;
	}
}

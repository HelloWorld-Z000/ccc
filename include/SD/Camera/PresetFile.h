#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace SD::Camera::PresetFile
{
	// Reading and writing preset files (SKSE/Plugins/SceneDirector/Presets/*.json).
	// No game types, so it can be unit tested: the caller passes in the names of
	// the angles, effects and lights, and gets indices back.
	//
	// A file holds one preset object, or a list of them:
	//
	//   {
	//       "name": "Tavern Talk",
	//       "author": "Someone",
	//       "description": "Close singles, slow cuts.",
	//       "cutting": { "minimumLines": 3, "maximumLines": 6, "shortestHold": 2.4 },
	//       "angles": {
	//           "CloseUp": { "frequency": 60, "fov": 45, "effect": "Push in", "amount": 40, "duration": 8.0 },
	//           "ClosePlayer": {}
	//       }
	//   }
	//
	// The angles listed are the ones in use; every other angle is off. Anything
	// else left out is filled in by the caller. Names are matched ignoring case,
	// spaces, dashes and underscores, and // comments are allowed.

	// Each entry is one thing under every name it may be written as. The first
	// name is the one written out.
	using Names = std::vector<std::vector<std::string>>;

	struct Vocabulary
	{
		Names angles;
		Names effects;
		Names lights;
	};

	// Times are in hundredths of a second, like the settings; files use seconds.
	struct Cutting
	{
		std::optional<bool> changeAnglePerLine;
		std::optional<int>  minimumLines;
		std::optional<int>  maximumLines;
		std::optional<bool> ignoreShortLines;
		std::optional<bool> timerWhileTalking;
		std::optional<bool> timerWhileChoosing;
		std::optional<int>  changeAfter;
		std::optional<int>  shortestHold;

		bool operator==(const Cutting&) const = default;
	};

	struct Angle
	{
		int                angle{ 0 };  // index into Vocabulary::angles
		std::optional<int> frequency;
		std::optional<int> fov;
		std::optional<int> effect;  // index into Vocabulary::effects
		std::optional<int> amount;
		std::optional<int> duration;
		std::optional<int> light;   // index into Vocabulary::lights

		bool operator==(const Angle&) const = default;
	};

	struct Preset
	{
		std::string              name;
		std::string              author;
		std::string              description;
		Cutting                  cutting;
		std::vector<Angle>       angles;    // the angles in use, in file order
		std::vector<std::string> warnings;  // parts of this preset that were skipped
	};

	struct File
	{
		std::vector<Preset>      presets;
		std::vector<std::string> warnings;  // parts of the file outside any preset that were skipped
		std::string              error;     // set when nothing in the file could be read
	};

	[[nodiscard]] inline std::string Lower(std::string_view a_text)
	{
		std::string out{ a_text };
		for (auto& c : out) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}
		return out;
	}

	// "Push in", "pushIn" and "push_in" are the same name.
	[[nodiscard]] inline std::string Simplify(std::string_view a_text)
	{
		std::string out;
		for (const char c : a_text) {
			if (c == ' ' || c == '-' || c == '_' || c == '\t') {
				continue;
			}
			out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}
		return out;
	}

	[[nodiscard]] inline int Lookup(const Names& a_names, std::string_view a_text)
	{
		const auto wanted = Simplify(a_text);
		for (std::size_t i = 0; i < a_names.size(); ++i) {
			for (const auto& name : a_names[i]) {
				if (Simplify(name) == wanted) {
					return static_cast<int>(i);
				}
			}
		}
		return -1;
	}

	namespace detail
	{
		// Ordered, so angles come out in the order the file lists them.
		using Json = nlohmann::ordered_json;

		// The parser's message without its "[json.exception.parse_error.101] " prefix.
		[[nodiscard]] inline std::string Tidy(std::string_view a_what)
		{
			if (a_what.starts_with('[')) {
				if (const auto end = a_what.find("] "); end != std::string_view::npos) {
					a_what.remove_prefix(end + 2);
				}
			}
			return std::string{ a_what };
		}

		[[nodiscard]] inline std::optional<double> Number(const Json& a_value)
		{
			if (a_value.is_number()) {
				return a_value.get<double>();
			}
			return std::nullopt;
		}

		[[nodiscard]] inline std::optional<bool> Switch(const Json& a_value)
		{
			if (a_value.is_boolean()) {
				return a_value.get<bool>();
			}
			if (a_value.is_number()) {
				return a_value.get<double>() != 0.0;
			}
			return std::nullopt;
		}

		[[nodiscard]] inline int Whole(double a_value)
		{
			return static_cast<int>(std::lround(std::clamp(a_value, -1.0e6, 1.0e6)));
		}

		[[nodiscard]] inline int Hundredths(double a_seconds)
		{
			return Whole(a_seconds * 100.0);
		}

		// Valid JSON whatever the bytes: anything that isn't UTF-8 is replaced.
		[[nodiscard]] inline std::string Quote(std::string_view a_text)
		{
			return Json(std::string{ a_text }).dump(-1, ' ', false, Json::error_handler_t::replace);
		}

		// 2.4 rather than 2.40, and 9.0 rather than 9.
		[[nodiscard]] inline std::string Seconds(int a_hundredths)
		{
			char buffer[32]{};
			const char* format = a_hundredths % 10 != 0 ? "%.2f" : "%.1f";
			std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(a_hundredths) / 100.0);
			return buffer;
		}

		struct Reader
		{
			const Vocabulary& words;

			void Text(const Json& a_value, std::string& a_out, std::string_view a_key,
				std::vector<std::string>& a_warnings) const
			{
				if (a_value.is_string()) {
					a_out = a_value.get<std::string>();
				} else {
					a_warnings.push_back("\"" + std::string{ a_key } + "\" should be text in quotes; ignored.");
				}
			}

			void ReadCutting(const Json& a_object, Cutting& a_out, std::vector<std::string>& a_warnings) const
			{
				if (!a_object.is_object()) {
					a_warnings.emplace_back("\"cutting\" should be a { ... } block; ignored.");
					return;
				}
				for (const auto& item : a_object.items()) {
					const std::string key = item.key();
					const Json&       value = item.value();
					const auto        id = Simplify(key);
					const auto flag = [&](std::optional<bool>& a_field) {
						a_field = Switch(value);
						if (!a_field) {
							a_warnings.push_back("\"" + key + "\" should be true or false; ignored.");
						}
					};
					const auto count = [&](std::optional<int>& a_field, bool a_seconds) {
						if (const auto number = Number(value)) {
							a_field = a_seconds ? Hundredths(*number) : Whole(*number);
						} else {
							a_warnings.push_back("\"" + key + "\" should be a number; ignored.");
						}
					};

					if (id == "changeangleperline") {
						flag(a_out.changeAnglePerLine);
					} else if (id == "minimumlines") {
						count(a_out.minimumLines, false);
					} else if (id == "maximumlines") {
						count(a_out.maximumLines, false);
					} else if (id == "ignoreshortlines") {
						flag(a_out.ignoreShortLines);
					} else if (id == "timerwhiletalking") {
						flag(a_out.timerWhileTalking);
					} else if (id == "timerwhilechoosing") {
						flag(a_out.timerWhileChoosing);
					} else if (id == "changeafter") {
						count(a_out.changeAfter, true);
					} else if (id == "shortesthold") {
						count(a_out.shortestHold, true);
					} else {
						a_warnings.push_back("\"" + key + "\" isn't a cutting setting; ignored.");
					}
				}
			}

			void ReadAngle(const std::string& a_name, const Json& a_object, Angle& a_out,
				std::vector<std::string>& a_warnings) const
			{
				if (!a_object.is_object()) {
					a_warnings.push_back("\"" + a_name + "\" should be a { ... } block (it can be empty); used with its defaults.");
					return;
				}
				for (const auto& item : a_object.items()) {
					const std::string key = item.key();
					const Json&       value = item.value();
					const auto        id = Simplify(key);
					const auto where = "\"" + key + "\" on " + a_name;
					const auto count = [&](std::optional<int>& a_field, bool a_seconds) {
						if (const auto number = Number(value)) {
							a_field = a_seconds ? Hundredths(*number) : Whole(*number);
						} else {
							a_warnings.push_back(where + " should be a number; ignored.");
						}
					};
					const auto named = [&](std::optional<int>& a_field, const Names& a_names) {
						const int index = value.is_string() ? Lookup(a_names, value.get<std::string>()) : -1;
						if (index >= 0) {
							a_field = index;
						} else {
							a_warnings.push_back(where + " isn't one of the choices in the menu; ignored.");
						}
					};

					if (id == "frequency") {
						count(a_out.frequency, false);
					} else if (id == "fov") {
						count(a_out.fov, false);
					} else if (id == "effect") {
						named(a_out.effect, words.effects);
					} else if (id == "amount") {
						count(a_out.amount, false);
					} else if (id == "duration") {
						count(a_out.duration, true);
					} else if (id == "light") {
						named(a_out.light, words.lights);
					} else {
						a_warnings.push_back(where + " isn't an angle setting; ignored.");
					}
				}
			}

			[[nodiscard]] Preset ReadPreset(const Json& a_object) const
			{
				Preset preset;
				auto&  warnings = preset.warnings;
				for (const auto& item : a_object.items()) {
					const std::string key = item.key();
					const Json&       value = item.value();
					const auto        id = Simplify(key);
					if (id == "name") {
						Text(value, preset.name, key, warnings);
					} else if (id == "author") {
						Text(value, preset.author, key, warnings);
					} else if (id == "description") {
						Text(value, preset.description, key, warnings);
					} else if (id == "cutting") {
						ReadCutting(value, preset.cutting, warnings);
					} else if (id == "angles") {
						if (!value.is_object()) {
							warnings.emplace_back("\"angles\" should be a { ... } block of angle names; ignored.");
							continue;
						}
						for (const auto& entry : value.items()) {
							const std::string angleName = entry.key();
							const Json&       angleValue = entry.value();
							const int index = Lookup(words.angles, angleName);
							if (index < 0) {
								warnings.push_back("\"" + angleName + "\" isn't an angle name; skipped.");
								continue;
							}
							Angle angle{};
							angle.angle = index;
							ReadAngle(angleName, angleValue, angle, warnings);

							// Listed twice under different spellings: the later one wins.
							std::erase_if(preset.angles, [index](const Angle& a_angle) { return a_angle.angle == index; });
							preset.angles.push_back(angle);
						}
					} else {
						warnings.push_back("\"" + key + "\" isn't a preset setting; ignored.");
					}
				}
				return preset;
			}
		};
	}

	[[nodiscard]] inline File Read(std::string_view a_text, const Vocabulary& a_words)
	{
		using detail::Json;

		File file;
		Json root;
		try {
			// Comments allowed; a UTF-8 BOM is skipped by the parser.
			root = Json::parse(a_text.begin(), a_text.end(), nullptr, true, true);
		} catch (const Json::exception& e) {
			file.error = detail::Tidy(e.what());
			return file;
		}

		const detail::Reader reader{ a_words };
		if (root.is_object()) {
			file.presets.push_back(reader.ReadPreset(root));
		} else if (root.is_array()) {
			for (std::size_t i = 0; i < root.size(); ++i) {
				if (root[i].is_object()) {
					file.presets.push_back(reader.ReadPreset(root[i]));
				} else {
					file.warnings.push_back("Entry " + std::to_string(i + 1) + " of the list isn't a preset { ... }; skipped.");
				}
			}
			if (file.presets.empty()) {
				file.error = "The list has no presets in it.";
			}
		} else {
			file.error = "A preset file should hold one preset { ... } or a list of them [ ... ].";
		}
		return file;
	}

	// One preset, laid out for reading: one line per angle, fields in menu order,
	// and only the fields that are set.
	[[nodiscard]] inline std::string Write(const Preset& a_preset, const Vocabulary& a_words)
	{
		using detail::Quote;
		using detail::Seconds;

		const auto name = [](const Names& a_names, int a_index) -> std::string {
			if (a_index >= 0 && static_cast<std::size_t>(a_index) < a_names.size() &&
				!a_names[static_cast<std::size_t>(a_index)].empty()) {
				return a_names[static_cast<std::size_t>(a_index)].front();
			}
			return {};
		};

		std::string out = "{\r\n";
		out += "    \"name\": " + Quote(a_preset.name) + ",\r\n";
		out += "    \"author\": " + Quote(a_preset.author) + ",\r\n";
		out += "    \"description\": " + Quote(a_preset.description) + ",\r\n";

		std::vector<std::string> fields;
		const auto&              c = a_preset.cutting;
		const auto flag = [&fields](const char* a_key, const std::optional<bool>& a_value) {
			if (a_value) {
				fields.push_back("\"" + std::string{ a_key } + "\": " + (*a_value ? "true" : "false"));
			}
		};
		const auto count = [&fields](const char* a_key, const std::optional<int>& a_value, bool a_seconds) {
			if (a_value) {
				fields.push_back("\"" + std::string{ a_key } + "\": " +
								 (a_seconds ? Seconds(*a_value) : std::to_string(*a_value)));
			}
		};
		flag("changeAnglePerLine", c.changeAnglePerLine);
		count("minimumLines", c.minimumLines, false);
		count("maximumLines", c.maximumLines, false);
		flag("ignoreShortLines", c.ignoreShortLines);
		flag("timerWhileTalking", c.timerWhileTalking);
		flag("timerWhileChoosing", c.timerWhileChoosing);
		count("changeAfter", c.changeAfter, true);
		count("shortestHold", c.shortestHold, true);

		out += "\r\n    \"cutting\": {";
		for (std::size_t i = 0; i < fields.size(); ++i) {
			out += (i == 0 ? "\r\n        " : ",\r\n        ") + fields[i];
		}
		out += fields.empty() ? "},\r\n" : "\r\n    },\r\n";

		// Angle names padded so the blocks line up.
		std::size_t widest = 0;
		for (const auto& angle : a_preset.angles) {
			widest = std::max(widest, name(a_words.angles, angle.angle).size());
		}

		out += "\r\n    \"angles\": {";
		for (std::size_t i = 0; i < a_preset.angles.size(); ++i) {
			const auto& angle = a_preset.angles[i];
			const auto  label = name(a_words.angles, angle.angle);

			std::vector<std::string> parts;
			if (angle.frequency) {
				parts.push_back("\"frequency\": " + std::to_string(*angle.frequency));
			}
			if (angle.fov) {
				parts.push_back("\"fov\": " + std::to_string(*angle.fov));
			}
			if (angle.effect) {
				parts.push_back("\"effect\": " + Quote(name(a_words.effects, *angle.effect)));
			}
			if (angle.amount) {
				parts.push_back("\"amount\": " + std::to_string(*angle.amount));
			}
			if (angle.duration) {
				parts.push_back("\"duration\": " + Seconds(*angle.duration));
			}
			if (angle.light) {
				parts.push_back("\"light\": " + Quote(name(a_words.lights, *angle.light)));
			}

			std::string body;
			for (std::size_t j = 0; j < parts.size(); ++j) {
				body += (j == 0 ? "" : ", ") + parts[j];
			}

			out += i == 0 ? "\r\n        " : ",\r\n        ";
			out += Quote(label) + ": " + std::string(widest - label.size(), ' ');
			out += body.empty() ? "{}" : "{ " + body + " }";
		}
		out += a_preset.angles.empty() ? "}\r\n" : "\r\n    }\r\n";
		out += "}\r\n";
		return out;
	}

	// A file name (without extension) for a preset name: characters Windows
	// doesn't allow are replaced, trailing dots and spaces are dropped, reserved
	// device names get a suffix, and the result is capped at 64 bytes without
	// splitting a UTF-8 character. Never empty.
	[[nodiscard]] inline std::string FileStem(std::string_view a_name)
	{
		const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
		while (!a_name.empty() && isSpace(a_name.front())) {
			a_name.remove_prefix(1);
		}
		while (!a_name.empty() && isSpace(a_name.back())) {
			a_name.remove_suffix(1);
		}

		std::string out;
		for (const char c : a_name) {
			const auto byte = static_cast<unsigned char>(c);
			if (byte < 0x20 || std::string_view{ "<>:\"/\\|?*" }.find(c) != std::string_view::npos) {
				out += '_';
			} else {
				out += c;
			}
		}

		if (out.size() > 64) {
			std::size_t cut = 64;
			while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
				--cut;  // don't end in the middle of a multi-byte character
			}
			out.resize(cut);
		}
		while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
			out.pop_back();
		}
		if (out.empty()) {
			return "Preset";
		}

		std::string upper = out;
		for (auto& c : upper) {
			if (c >= 'a' && c <= 'z') {
				c = static_cast<char>(c - 'a' + 'A');
			}
		}
		static constexpr std::string_view kReserved[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
			"COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
			"LPT7", "LPT8", "LPT9" };
		for (const auto reserved : kReserved) {
			if (upper == reserved) {
				out += '_';
				break;
			}
		}
		return out;
	}

	// One line of free text: newlines become spaces, outer spaces are dropped.
	[[nodiscard]] inline std::string OneLine(std::string_view a_text)
	{
		std::string out;
		for (const char c : a_text) {
			out += (c == '\r' || c == '\n') ? ' ' : c;
		}
		const auto first = out.find_first_not_of(" \t");
		if (first == std::string::npos) {
			return {};
		}
		return out.substr(first, out.find_last_not_of(" \t") - first + 1);
	}
}

#include "SD/Core/Logging.h"

#include <spdlog/sinks/basic_file_sink.h>

namespace SD::Log
{
	std::string_view Name(Category a_category) noexcept
	{
		switch (a_category) {
		case Category::kCore:       return "Core"sv;
		case Category::kDialogue:   return "Dialogue"sv;
		case Category::kStaging:    return "Staging"sv;
		case Category::kCamera:     return "Camera"sv;
		case Category::kContinuity: return "Continuity"sv;
		case Category::kCompat:     return "Compat"sv;
		case Category::kRender:     return "Render"sv;
		case Category::kCount:
		default:                    return "Unknown"sv;
		}
	}

	namespace
	{
		// Non-ASCII paths. std::filesystem::path::string() converts through the ANSI
		// codepage and throws std::system_error when a character can't be mapped. On
		// systems where the user folder name is in a non-Latin script that happened on
		// startup, before anything else ran. Nothing here can throw: a logger must
		// never take down what it's logging.

		// A narrow path the ANSI file APIs can open, or empty if there isn't one.
		// spdlog in this build is compiled with std::string filenames and opens files
		// with the narrow CRT call, so UTF-8 doesn't work here (and
		// SPDLOG_WCHAR_FILENAMES can't be enabled without rebuilding it). If the ANSI
		// form doesn't round-trip, fall back to the 8.3 short name, which is ASCII.
		[[nodiscard]] std::string AnsiPath(const std::wstring& a_path)
		{
			if (a_path.empty()) {
				return {};
			}

			const auto narrow = [](const std::wstring& a_wide) -> std::string {
				BOOL      lossy = FALSE;
				const int needed = ::WideCharToMultiByte(CP_ACP, 0, a_wide.c_str(),
					static_cast<int>(a_wide.size()), nullptr, 0, nullptr, nullptr);
				if (needed <= 0) {
					return {};
				}

				std::string out(static_cast<std::size_t>(needed), '\0');
				::WideCharToMultiByte(CP_ACP, 0, a_wide.c_str(), static_cast<int>(a_wide.size()),
					out.data(), needed, nullptr, &lossy);

				// Any substituted character means the name no longer refers to the file.
				return lossy ? std::string{} : out;
			};

			if (auto direct = narrow(a_path); !direct.empty()) {
				return direct;
			}

			// The 8.3 name requires the target to exist, which the directory does by now
			// and the log file may not, so shorten the directory and append the (ASCII)
			// file name.
			const std::filesystem::path full{ a_path };
			const std::wstring          parent = full.parent_path().wstring();
			if (parent.empty()) {
				return {};
			}

			const DWORD needed = ::GetShortPathNameW(parent.c_str(), nullptr, 0);
			if (needed == 0) {
				return {};  // 8.3 generation disabled on this volume
			}

			std::wstring shortParent(needed, L'\0');
			const DWORD  written = ::GetShortPathNameW(parent.c_str(), shortParent.data(), needed);
			if (written == 0 || written >= needed) {
				return {};
			}
			shortParent.resize(written);

			return narrow(shortParent + L'\\' + full.filename().wstring());
		}
	}

	std::string Utf8(std::wstring_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}

		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text.data(),
			static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
		if (needed <= 0) {
			return {};
		}

		std::string out(static_cast<std::size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	void Setup(std::string_view a_pluginName)
	{
		auto path = logger::log_directory();
		if (!path) {
			// Not report_and_fail, which shows a message box and terminates. spdlog keeps
			// a default logger, so the mod still runs; only the log is lost.
			return;
		}

		*path /= fmt::format("{}.log"sv, a_pluginName);

		// Created before the short-name lookup, which needs the directory to exist.
		std::error_code error;
		std::filesystem::create_directories(path->parent_path(), error);

		// Keep the previous log, so a crash isn't wiped out by the next launch.
		auto previous = *path;
		previous.replace_extension(".previous.log");
		std::filesystem::remove(previous, error);
		std::filesystem::rename(*path, previous, error);

		const std::string openable = AnsiPath(path->wstring());
		if (openable.empty()) {
			return;  // nowhere to write that this process can name. Not fatal.
		}

		// spdlog throws when a file won't open; that's not a reason to lose the mod.
		std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
		try {
			sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(openable, true);
		} catch (...) {
			return;
		}

		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
		const auto level = spdlog::level::trace;
#else
		const auto level = spdlog::level::info;
#endif
		log->set_level(level);
		log->flush_on(level);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v"s);
	}

	void LogLoadedModule()
	{
		HMODULE module = nullptr;
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LogLoadedModule),
				&module) ||
			!module) {
			return;
		}

		std::array<wchar_t, MAX_PATH> path{};
		const auto length = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0 || length >= path.size()) {
			return;
		}

		// Utf8, not path::string(): an install under a non-ASCII folder would
		// otherwise throw here.
		Info(Category::kCore, "Loaded from: {}"sv, Utf8(std::wstring{ path.data(), length }));
	}
}

#pragma once

namespace SD::Log
{
	enum class Category : std::uint8_t
	{
		kCore = 0,
		kDialogue,   // session edges, line starts, topic info
		kStaging,    // participants, composition solving
		kCamera,     // pose override, interpolation, collision
		kContinuity, // cut policy, the 180 line, shot selection
		kCompat,     // SmoothCam, IACC, other camera owners
		kRender,     // the letterbox layer: Present hook, device state
		kCount
	};

	[[nodiscard]] std::string_view Name(Category a_category) noexcept;

	// For per-frame code paths that should only log once.
	class OnceFlag
	{
	public:
		[[nodiscard]] bool Take() noexcept { return !taken.exchange(true, std::memory_order_relaxed); }
		void Reset() noexcept { taken.store(false, std::memory_order_relaxed); }

	private:
		std::atomic_bool taken{ false };
	};

	template <class... Args>
	void Info(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::info("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Warn(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::warn("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Error(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::error("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Debug(Category a_category, fmt::format_string<Args...> a_format, Args&&... a_args)
	{
		logger::debug("[SD.{}] {}"sv, Name(a_category), fmt::format(a_format, std::forward<Args>(a_args)...));
	}

	// Wide to UTF-8, for paths and other wide text going into the log. Never use
	// std::filesystem::path::string() for this: it converts through the ANSI
	// codepage and throws on characters it can't represent. Empty or unconvertible
	// input gives empty output; never throws.
	[[nodiscard]] std::string Utf8(std::wstring_view a_text);

	void Setup(std::string_view a_pluginName);

	// Logs the DLL Windows actually loaded, for when more than one build is
	// installed.
	void LogLoadedModule();
}

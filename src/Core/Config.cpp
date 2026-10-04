#include "SD/Core/Config.h"

#include "SD/Core/Logging.h"

namespace SD::Config
{
	namespace
	{
		// The game's root directory, from the running exe, as an absolute wide path.
		//
		// GetPrivateProfile* searches the Windows directory when given anything that
		// isn't a full path, so relative paths silently read defaults and write
		// nowhere. The path stays wide all the way to the profile API: narrowing it
		// through the ANSI codepage breaks on install paths with characters that
		// codepage can't represent (an accented or non-Latin user name is enough).
		//
		// The buffer grows until it fits. GetModuleFileNameW truncates rather than
		// failing, and MAX_PATH is easy to exceed under a OneDrive-redirected
		// Documents folder.
		[[nodiscard]] const std::wstring& GameRoot()
		{
			static const std::wstring root = [] {
				std::vector<wchar_t> buffer(MAX_PATH);
				for (;;) {
					const auto length = ::GetModuleFileNameW(nullptr, buffer.data(),
						static_cast<DWORD>(buffer.size()));
					if (length == 0) {
						return std::wstring{};  // caller reports; "." would be worse
					}
					if (length < buffer.size()) {
						return std::filesystem::path{ buffer.data() }.parent_path().wstring();
					}
					if (buffer.size() >= 32768) {
						return std::wstring{};  // past the extended-path ceiling
					}
					buffer.resize(buffer.size() * 2);
				}
			}();
			return root;
		}

		[[nodiscard]] const wchar_t* McmPath()
		{
			static const std::wstring path = GameRoot() + L"\\Data\\MCM\\Settings\\SceneDirector.ini";
			return path.c_str();
		}

		// The shipped file: defaults and documentation. Replaced by every update.
		[[nodiscard]] const wchar_t* OwnPath()
		{
			static const std::wstring path = GameRoot() + L"\\Data\\SKSE\\Plugins\\SD.ini";
			return path.c_str();
		}

		// The user's file, written by the menu and never shipped. Keeping it separate
		// from SD.ini means an update can replace SD.ini without losing anything the
		// player changed (several per-shot keys only exist as user data).
		//
		// Reads go MCM -> user -> shipped -> built-in, and writes go here. No
		// migration is needed: an existing install keeps its values in SD.ini, and a
		// key moves here the first time it's changed in the menu.
		[[nodiscard]] const wchar_t* UserPath()
		{
			static const std::wstring path = GameRoot() + L"\\Data\\SKSE\\Plugins\\SD_user.ini";
			return path.c_str();
		}

		// Same root, for a file this mod doesn't own, so Compat::ImprovedCamera can
		// read Improved Camera's settings. Kept here so there's one answer to "where
		// is the game".
		[[nodiscard]] std::wstring DataPathImpl(std::wstring_view a_relative)
		{
			const auto& root = GameRoot();
			if (root.empty() || a_relative.empty()) {
				return {};
			}

			std::wstring path = root;
			path += L"\\Data\\";
			path += a_relative;
			return path;
		}

		// Section and key names are ASCII literals; converted properly rather than
		// cast.
		[[nodiscard]] std::wstring Widen(const char* a_text)
		{
			if (!a_text || !*a_text) {
				return {};
			}

			const int needed = ::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, nullptr, 0);
			if (needed <= 1) {
				return {};
			}

			std::wstring out(static_cast<std::size_t>(needed) - 1, L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_text, -1, out.data(), needed);
			return out;
		}

		// Back to UTF-8 for String() and the log (paths in log lines aren't ASCII).
		[[nodiscard]] std::string Narrow(const wchar_t* a_text, int a_length = -1)
		{
			if (!a_text || !*a_text) {
				return {};
			}

			const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_text, a_length,
				nullptr, 0, nullptr, nullptr);
			if (needed <= 0) {
				return {};
			}

			std::string out(static_cast<std::size_t>(needed), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_text, a_length, out.data(), needed, nullptr, nullptr);
			if (a_length == -1 && !out.empty() && out.back() == '\0') {
				out.pop_back();
			}
			return out;
		}

		// A UTF-8 byte order mark breaks the profile API. It only recognises a UTF-16
		// BOM, so a UTF-8 one becomes part of line one. When line one is a section
		// header (as in SD_user.ini, where WritePrivateProfileString puts [Direction]
		// first), that whole section stops matching and its keys fall back to
		// defaults; writes then append a second copy of the section instead of
		// updating the first. A comment or blank first line absorbs it harmlessly.
		//
		// Editors like Notepad++ and VS Code can add one on save, so both files are
		// repaired automatically rather than warned about.

		// Empty on any failure, which the caller treats as nothing to repair. The size
		// cap keeps a wrong or corrupt path from becoming a large allocation.
		[[nodiscard]] std::optional<std::string> ReadWholeFile(const wchar_t* a_path)
		{
			constexpr LONGLONG kSaneCeiling = 8LL * 1024LL * 1024LL;

			const HANDLE file = ::CreateFileW(a_path, GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return std::nullopt;
			}

			LARGE_INTEGER size{};
			if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > kSaneCeiling) {
				::CloseHandle(file);
				return std::nullopt;
			}

			std::string data(static_cast<std::size_t>(size.QuadPart), '\0');
			DWORD       read = 0;
			const bool  ok = ::ReadFile(file, data.data(), static_cast<DWORD>(data.size()),
								 &read, nullptr) != FALSE &&
							read == data.size();
			::CloseHandle(file);

			if (!ok) {
				return std::nullopt;
			}
			return data;
		}

		[[nodiscard]] bool WriteWholeFile(const wchar_t* a_path, const void* a_data, std::size_t a_size)
		{
			const HANDLE file = ::CreateFileW(a_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}

			DWORD      written = 0;
			const bool ok = a_size == 0 ||
							(::WriteFile(file, a_data, static_cast<DWORD>(a_size), &written, nullptr) != FALSE &&
								written == a_size);
			::CloseHandle(file);
			return ok;
		}

		void RepairByteOrderMark(const wchar_t* a_path)
		{
			const auto content = ReadWholeFile(a_path);
			if (!content) {
				return;
			}

			constexpr std::string_view kUtf8Mark = "\xEF\xBB\xBF"sv;

			const std::string_view whole{ *content };
			if (!whole.starts_with(kUtf8Mark)) {
				return;
			}

			const std::string_view rest = whole.substr(kUtf8Mark.size());

			// ASCII content just loses the three bytes, so the file stays plain text that
			// other tools expect. A file with real non-ASCII content is rewritten as
			// UTF-16LE instead, which the API reads natively (without the BOM it would
			// read it through the legacy codepage and mangle, say, a Japanese slot name).
			const bool ascii = std::none_of(rest.begin(), rest.end(), [](char a_ch) {
				return static_cast<unsigned char>(a_ch) >= 0x80u;
			});

			bool        written = false;
			std::string how;

			if (ascii) {
				written = WriteWholeFile(a_path, rest.data(), rest.size());
				how = "removed it";
			} else {
				const int needed = rest.empty() ? 0 :
												  ::MultiByteToWideChar(CP_UTF8, 0, rest.data(),
													  static_cast<int>(rest.size()), nullptr, 0);
				if (needed > 0) {
					std::wstring wide(static_cast<std::size_t>(needed), L'\0');
					::MultiByteToWideChar(CP_UTF8, 0, rest.data(), static_cast<int>(rest.size()),
						wide.data(), needed);

					std::wstring out;
					out.reserve(wide.size() + 1);
					// The UTF-16 BOM, written as a number so it isn't an invisible character in
					// the source.
					out.push_back(static_cast<wchar_t>(0xFEFFu));
					out.append(wide);

					written = WriteWholeFile(a_path, out.data(), out.size() * sizeof(wchar_t));
				}
				how = "converted the file to UTF-16, because it contains non-ASCII text";
			}

			if (written) {
				Log::Warn(Log::Category::kCore,
					"{} began with a UTF-8 byte order mark. Windows does not recognise one in an "
					"ini, so if a [Section] header was the first line of the file, that section "
					"and every setting in it was being ignored. Repaired: {}. If you edit this "
					"file by hand, save it as ANSI or as UTF-8 WITHOUT a byte order mark."sv,
					Narrow(a_path), how);
			} else {
				Log::Error(Log::Category::kCore,
					"{} begins with a UTF-8 byte order mark and could not be rewritten "
					"(error {}). Windows does not recognise one in an ini, so if a [Section] "
					"header is the first line of the file, that section and every setting in it "
					"is being ignored. Re-save it as ANSI, or as UTF-8 without a byte order "
					"mark."sv,
					Narrow(a_path), ::GetLastError());
			}
		}

		// Runs once, before anything reads or writes: called from the readers and
		// writers themselves rather than from Runtime, so the order can't go wrong.
		// Only SD's own two files; the MCM file belongs to MCM Helper.
		void EnsureRepaired()
		{
			static const bool once = [] {
				if (!GameRoot().empty()) {
					RepairByteOrderMark(OwnPath());
					RepairByteOrderMark(UserPath());
				}
				return true;
			}();
			static_cast<void>(once);
		}

		// A sentinel no real setting uses, so a missing key can be told apart from a
		// legitimate zero.
		constexpr int kAbsent = -999999;

		Log::OnceFlag sourceReported;

		[[nodiscard]] bool McmPresent()
		{
			return ::GetFileAttributesW(McmPath()) != INVALID_FILE_ATTRIBUTES;
		}

		SettingsCache& Cache()
		{
			static SettingsCache cache;
			return cache;
		}

		SettingsCache::Revision Revisions()
		{
			EnsureRepaired();
			SettingsCache::Revision revision{};
			const std::array paths{ McmPath(), UserPath(), OwnPath() };
			// A failed metadata query mustn't make stale values look unchanged.
			static std::uint32_t failedProbe = 0;
			for (std::size_t i = 0; i < paths.size(); ++i) {
				WIN32_FILE_ATTRIBUTE_DATA data{};
				if (::GetFileAttributesExW(paths[i], GetFileExInfoStandard, &data)) {
					revision[i] = { data.dwFileAttributes, data.ftLastWriteTime.dwHighDateTime,
						data.ftLastWriteTime.dwLowDateTime, data.nFileSizeHigh, data.nFileSizeLow, 0 };
				} else {
					const auto error = ::GetLastError();
					if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
						revision[i][5] = ++failedProbe;
					}
				}
			}
			return revision;
		}
	}

	ReadScope::ReadScope() : scope(Cache(), Revisions) {}

	std::wstring DataPath(std::wstring_view a_relative)
	{
		return DataPathImpl(a_relative);
	}

	int Int(const char* a_section, const char* a_key, int a_default)
	{
		return Cache().Int(a_section ? a_section : "", a_key ? a_key : "", a_default, [&] {
			EnsureRepaired();

			const auto section = Widen(a_section);
			const auto key = Widen(a_key);

			if (McmPresent()) {
				const int fromMcm = ::GetPrivateProfileIntW(section.c_str(), key.c_str(),
					kAbsent, McmPath());
				if (fromMcm != kAbsent) {
					return fromMcm;
				}
			}

			// The player's own choice outranks the shipped file. See UserPath.
			const int fromUser = ::GetPrivateProfileIntW(section.c_str(), key.c_str(),
				kAbsent, UserPath());
			if (fromUser != kAbsent) {
				return fromUser;
			}

			const int fromOwn = ::GetPrivateProfileIntW(section.c_str(), key.c_str(),
				kAbsent, OwnPath());
			return fromOwn != kAbsent ? fromOwn : a_default;
		});
	}

	bool Bool(const char* a_section, const char* a_key, bool a_default)
	{
		return Int(a_section, a_key, a_default ? 1 : 0) != 0;
	}

	namespace
	{
		// Reads one key, growing the buffer until the value fits.
		// GetPrivateProfileString signals truncation only by returning size-1, and a
		// saved preset slot is several hundred characters. Starts small and grows by
		// 4x; capped so a corrupt file can't loop.
		[[nodiscard]] std::wstring ReadProfileString(const wchar_t* a_section, const wchar_t* a_key,
			const wchar_t* a_default, const wchar_t* a_path)
		{
			std::vector<wchar_t> buffer(256);
			for (int attempt = 0; attempt < 6; ++attempt) {
				const auto length = ::GetPrivateProfileStringW(a_section, a_key, a_default,
					buffer.data(), static_cast<DWORD>(buffer.size()), a_path);

				if (length + 1 < buffer.size()) {
					return std::wstring{ buffer.data(), length };
				}
				buffer.resize(buffer.size() * 4);
			}
			return std::wstring{ buffer.data(), buffer.size() - 1 };
		}
	}

	std::string String(const char* a_section, const char* a_key, const char* a_default)
	{
		return Cache().String(a_section ? a_section : "", a_key ? a_key : "", a_default ? a_default : "", [&] {
			EnsureRepaired();

			// Same precedence as Int(). No sentinel needed: an absent key returns the
			// default string.
			const auto section = Widen(a_section);
			const auto key = Widen(a_key);

			if (McmPresent()) {
				const auto value = ReadProfileString(section.c_str(), key.c_str(), L"", McmPath());
				if (!value.empty()) {
					return Narrow(value.c_str(), static_cast<int>(value.size()));
				}
			}

			if (const auto value = ReadProfileString(section.c_str(), key.c_str(), L"", UserPath());
				!value.empty()) {
				return Narrow(value.c_str(), static_cast<int>(value.size()));
			}

			const auto fallback = Widen(a_default);
			const auto value =
				ReadProfileString(section.c_str(), key.c_str(), fallback.c_str(), OwnPath());
			return Narrow(value.c_str(), static_cast<int>(value.size()));
		});
	}

	void SetString(const char* a_section, const char* a_key, const char* a_value)
	{
		const auto write = Cache().Write();
		// Repair before writing too: writing into a file with a UTF-8 BOM appends a
		// duplicate section.
		EnsureRepaired();

		const auto section = Widen(a_section);
		const auto key = Widen(a_key);
		const auto value = Widen(a_value);

		if (!::WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), UserPath())) {
			Log::Warn(Log::Category::kCore, "Could not write [{}] {}={} to {} (error {})."sv,
				a_section, a_key, a_value, Narrow(UserPath()), ::GetLastError());
			return;
		}
		::WritePrivateProfileStringW(nullptr, nullptr, nullptr, UserPath());
	}

	void SetInt(const char* a_section, const char* a_key, int a_value)
	{
		const auto write = Cache().Write();
		// See SetString.
		EnsureRepaired();

		// Written to SD_user.ini only. The MCM file is rewritten by MCM Helper, and
		// SD.ini is replaced by updates.
		//
		// Because Int() checks MCM first, an MCM holding the same key will shadow a
		// value written here. That's the right precedence, but the in-game menu and an
		// MCM shouldn't both own the same key.
		wchar_t text[32]{};
		std::swprintf(text, std::size(text), L"%d", a_value);

		const auto section = Widen(a_section);
		const auto key = Widen(a_key);

		if (!::WritePrivateProfileStringW(section.c_str(), key.c_str(), text, UserPath())) {
			Log::Warn(Log::Category::kCore, "Could not write [{}] {}={} to {} (error {})."sv,
				a_section, a_key, a_value, Narrow(UserPath()), ::GetLastError());
			return;
		}

		// Flush the profile cache; Windows buffers these writes and reports success
		// before the file changes.
		::WritePrivateProfileStringW(nullptr, nullptr, nullptr, UserPath());

		// Read it back through Int() to catch both failures: the write not landing
		// (possible under MO2's virtual filesystem) and the write landing but being
		// shadowed by an MCM value. One profile read per slider release.
		const int effective = Int(a_section, a_key, kAbsent);
		if (effective != a_value) {
			Log::Error(Log::Category::kCore,
				"Wrote [{}] {}={} to {} but the value in force is {}. {}"sv,
				a_section, a_key, a_value, Narrow(UserPath()),
				effective == kAbsent ? std::string{ "nothing" } : std::to_string(effective),
				McmPresent() ?
					"An MCM settings file exists and takes precedence over SD.ini, so it is "
					"shadowing this write — change the setting in the MCM, or remove "
					"Data/MCM/Settings/SceneDirector.ini."sv :
					"The file is not being persisted — check that it is writable and not "
					"locked by a mod manager."sv);
		}
	}

	void SetBool(const char* a_section, const char* a_key, bool a_value)
	{
		SetInt(a_section, a_key, a_value ? 1 : 0);
	}

	void ReportSource()
	{
		if (!sourceReported.Take()) {
			return;
		}

		// Before the settings-source line, so a repair is logged above it.
		EnsureRepaired();

		if (GameRoot().empty()) {
			Log::Error(Log::Category::kCore,
				"Could not resolve the game directory from the running executable, so every "
				"settings path is relative and nothing will read or write correctly. No setting "
				"will persist in this session."sv);
		}

		const bool mcm = McmPresent();
		const bool own = ::GetFileAttributesW(OwnPath()) != INVALID_FILE_ATTRIBUTES;
		const bool user = ::GetFileAttributesW(UserPath()) != INVALID_FILE_ATTRIBUTES;

		// Logs the resolved path, not just present/absent, so a path that resolved
		// somewhere unexpected is visible.
		Log::Info(Log::Category::kCore,
			"Settings source: MCM {}, SD_user.ini {}, SD.ini {}. Reading from {}."sv,
			mcm ? "present"sv : "absent"sv,
			user ? "present"sv : "absent"sv,
			own ? "present"sv : "absent"sv,
			Narrow(OwnPath()));

		if (!mcm && !own && !user) {
			Log::Warn(Log::Category::kCore, "No settings file found; built-in defaults in use."sv);
		}

		// With both present the in-game menu appears to do nothing: it writes SD.ini,
		// and reads come from the MCM file first.
		if (mcm && own) {
			Log::Warn(Log::Category::kCore,
				"Both an MCM settings file and SD.ini are present. The MCM wins every key it "
				"carries, so changes made in the in-game menu will not take effect for those "
				"keys. MCM file: {}"sv,
				Narrow(McmPath()));
		}
	}
}

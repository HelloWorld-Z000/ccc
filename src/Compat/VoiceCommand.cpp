#include "SD/Compat/VoiceCommand.h"

#include "SD/Core/Logging.h"

namespace SD::Compat
{
	namespace
	{
		RE::SCRIPT_FUNCTION::Execute_t* original{ nullptr };
		bool                            installed{ false };

		// Console commands run on a thread we don't own, and LipSync reads on the main
		// thread. One short string, one write per line.
		std::mutex  mutex;
		std::string pack;

		// The line SpeakSound was last asked to play on the player, waiting to be
		// consumed. Same mutex. No SKSE task marshalling: the consumer polls every
		// frame, and a task would only add a frame of latency.
		bool               startPending{ false };
		VoiceCommand::Line pendingLine;

		// Set on the first player line and never cleared. See EverHeard.
		std::atomic_bool everHeard{ false };

		Log::OnceFlag learnedReported;
		Log::OnceFlag firstLineReported;
		Log::OnceFlag otherRefReported;

		// "DBVO/voicebella/Some_line.fuz" -> "voicebella". Takes the segment between
		// the first and second separator rather than matching a known root, so a
		// DBReV-native path (DBReV/<pack>/<plugin>/...) works too. Either slash is
		// accepted.
		[[nodiscard]] std::string PackFromPath(std::string_view a_path)
		{
			const auto first = a_path.find_first_of("/\\");
			if (first == std::string_view::npos) {
				return {};
			}

			const auto second = a_path.find_first_of("/\\", first + 1);
			if (second == std::string_view::npos || second <= first + 1) {
				return {};
			}

			return std::string{ a_path.substr(first + 1, second - first - 1) };
		}

		// SpeakSound plays any sound and mods use it for non-speech too. Voice lines
		// are .fuz files; anything else doesn't open a line.
		[[nodiscard]] bool IsVoiceFile(std::string_view a_path)
		{
			constexpr std::string_view kExtension = ".fuz"sv;
			if (a_path.size() <= kExtension.size()) {
				return false;
			}

			const auto tail = a_path.substr(a_path.size() - kExtension.size());
			for (std::size_t i = 0; i < kExtension.size(); ++i) {
				const char raw = tail[i];
				const char lower = static_cast<char>(raw >= 'A' && raw <= 'Z' ? raw + 32 : raw);
				if (lower != kExtension[i]) {
					return false;
				}
			}

			return true;
		}

		bool Execute(const RE::SCRIPT_PARAMETER* a_paramInfo,
			RE::SCRIPT_FUNCTION::ScriptData*    a_scriptData,
			RE::TESObjectREFR*                  a_thisObj,
			RE::TESObjectREFR*                  a_containingObj,
			RE::Script*                         a_scriptObj,
			RE::ScriptLocals*                   a_locals,
			double&                             a_result,
			std::uint32_t&                      a_opcodeOffsetPtr)
		{
			// Parse a copy of the offset; ParseParameters advances the cursor, and the
			// original call needs the untouched one.
			std::uint32_t offset = a_opcodeOffsetPtr;
			char          buffer[512]{};

			if (RE::Script::ParseParameters(a_paramInfo, a_scriptData, offset, a_thisObj,
					a_containingObj, a_scriptObj, a_locals, buffer)) {
				buffer[sizeof(buffer) - 1] = '\0';

				if (auto found = PackFromPath(buffer); !found.empty()) {
					bool changed = false;
					{
						const std::scoped_lock lock{ mutex };
						changed = pack != found;
						if (changed) {
							pack = found;
						}
					}

					// Reported on the first line and whenever it changes (the pack is per
					// character in DBVO 2 and DBReV, so switching characters changes it).
					if (changed) {
						if (learnedReported.Take()) {
							Log::Info(Log::Category::kCompat,
								"Voice pack in use: \"{}\", read from the SpeakSound call. Line lengths will be measured from this pack."sv,
								found);
						} else {
							Log::Info(Log::Category::kCompat,
								"Voice pack changed to \"{}\"."sv, found);
						}
					}

					// --- The line ------------------------------------------------------------
					//
					// Only when the player is the one speaking: a follower framework voicing an
					// NPC through the same command mustn't drive the player's mouth. The pack
					// learning above isn't gated, since it already works; if the ref assumption is
					// wrong, this path just never fires and EverHeard leaves consumers on their
					// old code.
					const bool onPlayer = a_thisObj && a_thisObj->IsPlayerRef();

					if (onPlayer && IsVoiceFile(buffer)) {
						{
							const std::scoped_lock lock{ mutex };

							// A new start replacing an unconsumed one is a topic clicked before the last
							// line finished; animate the newest.
							pendingLine.path.assign(buffer);
							pendingLine.pack = found;
							startPending = true;
						}

						everHeard.store(true, std::memory_order_relaxed);

						if (firstLineReported.Take()) {
							Log::Info(Log::Category::kCompat,
								"SpeakSound named the player's line: \"{}\". Line starts and lengths now come from the command rather than from sound handles."sv,
								buffer);
						}
					} else if (!onPlayer && otherRefReported.Take()) {
						// Logged once: otherwise a log that learned the pack but never opened a line
						// looks the same as one where the wrapper never ran.
						Log::Info(Log::Category::kCompat,
							"SpeakSound seen on something other than the player (\"{}\"); noted for the pack only."sv,
							buffer);
					}
				}
			}

			// Always call the original; this wrapper only observes.
			return original ? original(a_paramInfo, a_scriptData, a_thisObj, a_containingObj,
									a_scriptObj, a_locals, a_result, a_opcodeOffsetPtr) :
							  true;
		}
	}

	void VoiceCommand::Install()
	{
		if (installed) {
			return;
		}
		installed = true;

		auto* command = RE::SCRIPT_FUNCTION::LocateConsoleCommand("SpeakSound"sv);
		if (!command || !command->executeFunction) {
			Log::Warn(Log::Category::kCompat,
				"SpeakSound console command not found; the voice pack cannot be identified and line lengths will fall back to searching every pack."sv);
			return;
		}

		original = command->executeFunction;

		RE::SCRIPT_FUNCTION::Execute_t* replacement = &Execute;
		REL::safe_write(reinterpret_cast<std::uintptr_t>(std::addressof(command->executeFunction)),
			std::addressof(replacement), sizeof(replacement));

		Log::Info(Log::Category::kCompat,
			"Watching SpeakSound to identify the active voice pack and the player's lines."sv);
	}

	std::string VoiceCommand::Pack()
	{
		const std::scoped_lock lock{ mutex };
		return pack;
	}

	bool VoiceCommand::TakeLineStart(Line& a_out)
	{
		const std::scoped_lock lock{ mutex };
		if (!startPending) {
			return false;
		}

		startPending = false;
		a_out = pendingLine;
		return true;
	}

	bool VoiceCommand::EverHeard() noexcept
	{
		return everHeard.load(std::memory_order_relaxed);
	}

	void VoiceCommand::Reset()
	{
		const std::scoped_lock lock{ mutex };
		startPending = false;
		pendingLine = {};

		// `pack` is kept on purpose: it belongs to the character, which is usually the
		// same after a load.
	}
}

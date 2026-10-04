#include "SD/Scene/LipSync.h"

#include "SD/Compat/DBReV.h"
#include "SD/Compat/VoiceCommand.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/FaceGen.h"
#include "SD/Scene/ExpressionModel.h"
#include "SD/Scene/Interface.h"
#include "SD/Scene/Performance.h"

#include <cmath>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace SD::Scene
{
	namespace
	{
		// Skyrim's facegen rig has 16 phoneme slots, in Creation Kit order:
		//
		//   0 Aah  1 BigAah  2 BMP  3 ChJSh  4 DST  5 Eee  6 Eh  7 FV
		//   8 I    9 K      10 N   11 Oh    12 OohQ 13 R  14 Th 15 W
		//
		// Slot 0 acts as the jaw hinge (forced to 1.0 it holds the mouth fully open).
		constexpr std::uint32_t kSlotCount = 16;
		constexpr std::uint32_t kSlotJaw = 0;

		// Shape table: how far the jaw drops plus one lip slot. Most speech barely
		// opens the jaw and the lips do the work (rounding for Oh and W, spreading for
		// Eee, closing for BMP), so most entries keep the jaw at or under 0.30.
		struct Shape
		{
			float         jaw;
			std::uint32_t slot;
			float         weight;
		};

		constexpr Shape kShapes[] = {
			{ 0.80f, 1, 0.55f },   // BigAah  - the wide one, used sparingly
			{ 0.55f, 6, 0.50f },   // Eh
			{ 0.30f, 5, 0.75f },   // Eee     - spread, jaw nearly shut
			{ 0.26f, 8, 0.60f },   // I
			{ 0.50f, 11, 0.80f },  // Oh      - rounded
			{ 0.24f, 12, 0.85f },  // OohQ    - tight round
			{ 0.16f, 15, 0.70f },  // W       - pursed
			{ 0.04f, 2, 0.90f },   // BMP     - lips closed. Speech needs these.
			{ 0.20f, 7, 0.65f },   // FV
			{ 0.28f, 10, 0.50f },  // N
			{ 0.36f, 3, 0.55f },   // ChJSh
			{ 0.12f, 14, 0.45f },  // Th
			{ 0.34f, 4, 0.50f },   // DST
			{ 0.30f, 9, 0.45f },   // K
			{ 0.22f, 13, 0.55f },  // R
		};

		// --- Text-driven shapes -------------------------------------------------
		//
		// The random table above is the fallback for when the line's text can't be
		// read. Otherwise the mouth follows the words with a simple grapheme-to-viseme
		// pass (letter clusters to mouth positions, not a pronunciation dictionary).
		// It gets some English spellings wrong, but it puts closures on m and p,
		// rounding on o and w and spread on ee, which is what makes it read as speech.
		//
		// Lip weights are near the top of their range and the jaw is low on everything
		// except open vowels; with equal weights the jaw dominates and every shape
		// reads as "mouth open a bit".
		//
		// Two rests: a short gap between words and a fuller close at sentence
		// punctuation. People don't close their mouths between the words of a phrase.
		constexpr Shape kRest = { 0.02f, 0, 0.00f };   // sentence punctuation
		constexpr Shape kSoftRest = { 0.15f, 0, 0.00f };  // between words
		constexpr Shape kAah = { 0.78f, 1, 0.55f };
		constexpr Shape kEh = { 0.48f, 6, 0.65f };
		constexpr Shape kEee = { 0.22f, 5, 0.95f };
		constexpr Shape kIh = { 0.22f, 8, 0.80f };
		constexpr Shape kOh = { 0.44f, 11, 1.00f };
		constexpr Shape kOoh = { 0.16f, 12, 1.00f };
		constexpr Shape kWuh = { 0.10f, 15, 0.95f };
		constexpr Shape kBmp = { 0.01f, 2, 1.00f };
		constexpr Shape kFv = { 0.10f, 7, 0.95f };
		constexpr Shape kTh = { 0.10f, 14, 0.70f };
		constexpr Shape kCh = { 0.28f, 3, 0.80f };
		constexpr Shape kDst = { 0.24f, 4, 0.70f };
		constexpr Shape kKg = { 0.26f, 9, 0.60f };
		constexpr Shape kNl = { 0.22f, 10, 0.70f };
		constexpr Shape kRr = { 0.18f, 13, 0.80f };

		// Vowels are held about twice as long as consonants. Scaled by kSpeechRate.
		// These target roughly twelve to fifteen mouth positions a second, which is
		// typical conversational English. Lines with a measured length are scaled to
		// it anyway; these matter most when there's slack or no measurement.
		constexpr float kVowelSeconds = 0.070f;
		constexpr float kConsonantSeconds = 0.038f;
		constexpr float kWordGapSeconds = 0.048f;
		constexpr float kPunctuationSeconds = 0.150f;

		// Lead the target slightly to make up for the smoothing lag and the time it
		// takes to notice the sound handle.
		constexpr float kLeadSeconds = 0.075f;

		// Overall speed of the sequence. Raise it if the mouth finishes before the
		// audio, lower it if it runs past.
		constexpr float kSpeechRate = 1.0f;

		struct Phone
		{
			Shape shape;
			float seconds;
		};

		std::vector<Phone> phones;
		std::size_t        phoneCursor{ 0 };
		float              phoneRemaining{ 0.0f };
		bool               fromText{ false };

		// The measured line length and how far the estimate had to be scaled to fit
		// it. Both logged: a scale far from 1.0 on every line means the per-phoneme
		// constants need adjusting.
		float lineSeconds{ 0.0f };
		float lineScale{ 1.0f };
		float lineGapScale{ 1.0f };

		// How long one shape holds without text. Speech runs about eight to twelve
		// mouth positions a second.
		constexpr float kShapeHoldMin = 0.070f;
		constexpr float kShapeHoldMax = 0.135f;

		// Approach times toward the target. Asymmetric: the attack is fast enough that
		// a short consonant (the rounding for "ooh", the lip-bite for "f", the closure
		// for "m") actually arrives, and the release is slower so the mouth doesn't
		// chatter.
		constexpr float kAttackSeconds = 0.014f;
		constexpr float kReleaseSeconds = 0.045f;

		// The jaw gets a much slower attack than the lips. It's the heaviest part of
		// the face, and snapping it to a new opening every 40-70 ms looks like
		// chewing.
		constexpr float kJawAttackSeconds = 0.055f;
		constexpr float kJawReleaseSeconds = 0.075f;

		// About one shape in seven is a rest, which puts gaps between words.
		constexpr int kRestOneIn = 7;

		// A line longer than this is a sound handle that never cleared, not speech.
		// Without a cap a stuck handle leaves the mouth moving for the rest of the
		// conversation.
		constexpr float kMaxLineSeconds = 8.0f;

		bool  enabled{ false };
		float strength{ 0.55f };

		// The strength setting is mainly a jaw control. The jaw takes it linearly and
		// the lips get a gentler curve (square root), so turning the jaw down doesn't
		// also flatten the lip shapes. At 67 the lips land at 0.82.
		float lipStrength{ 0.74f };

		// Kept alongside the float so the slider doesn't drift by one from rounding.
		int   strengthPercent{ 55 };

		bool  engaged{ false };

		// State for the line currently being spoken.
		bool  lineActive{ false };
		std::uint64_t voiceLineSerial{ 0 };
		// The line was announced by DBReV or SpeakSound rather than inferred from a
		// sound handle (which could be a footstep). See PlayerLineAnnounced.
		bool  voiceLineAnnounced{ false };
		std::string voiceLineText;
		std::string highlightedTopic;
		bool  driving{ false };
		bool  overran{ false };
		float lineElapsed{ 0.0f };

		// How long the mouth should move, when something reliable says so (DBReV's
		// audioSeconds, or the measured .fuz). The sound handle often doesn't go idle
		// when the audio ends, so without this the mouth would keep going until the
		// kMaxLineSeconds backstop. 0 means only that backstop applies.
		float audioWindow{ 0.0f };

		// The sound handle this line runs on, and the one whose line has already
		// played out. Only used by the handle-polling path; DBReV sends one start per
		// line.
		std::uint32_t activeVoiceID{ RE::BSSoundHandle::kInvalidID };
		std::uint32_t consumedVoiceID{ RE::BSSoundHandle::kInvalidID };

		// The mouth and its target. Plain arrays: this and FaceGen's hook both run on
		// the main thread, Update earlier in the frame than the morph pass.
		float current[kSlotCount]{};
		float target[kSlotCount]{};
		float shapeRemaining{ 0.0f };

		std::atomic_bool writing{ false };


		// Seeded per line so two lines don't share a mouth sequence.
		std::uint32_t rng{ 1 };

		// Counts lines, to keep the seed changing on the event-driven path where
		// there's no handle ID. Wrapping is fine.
		std::uint32_t lineOrdinal{ 0 };

		[[nodiscard]] std::uint32_t NextRandom()
		{
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			return rng;
		}

		[[nodiscard]] float RandomUnit()
		{
			return static_cast<float>(NextRandom() % 10000u) / 10000.0f;
		}

		// Observed and logged only. See Update.
		float windowMin{ 0.0f };
		float windowMax{ 0.0f };
		bool  windowSampled{ false };

		Log::OnceFlag verdictReported;

		[[nodiscard]] RE::HighProcessData* HighOf(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process ? process->high : nullptr;
		}

		// Is a voice line playing on this actor? Read from soundHandles only.
		// voiceTimer isn't a per-line countdown on the player (it sits at a constant
		// value all session), and voiceState/voiceTimeElapsed belong to the shout
		// system. Returns the playing handle's ID, or kInvalidID; the ID also seeds
		// the line's shape sequence.
		[[nodiscard]] std::uint32_t PlayingVoiceID(RE::HighProcessData* a_high)
		{
			if (!a_high) {
				return RE::BSSoundHandle::kInvalidID;
			}

			for (const auto& handle : a_high->soundHandles) {
				if (handle.soundID != RE::BSSoundHandle::kInvalidID &&
					handle.state.get() == RE::BSSoundHandle::AssumedState::kPlaying) {
					return handle.soundID;
				}
			}

			return RE::BSSoundHandle::kInvalidID;
		}

		[[nodiscard]] RE::BSFaceGenAnimationData* FaceOf(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->GetFaceGenAnimationData() : nullptr;
		}

		// Peak of the phoneme channel, or negative if it couldn't be read. A channel
		// that can't be read must not be treated as one at rest.
		[[nodiscard]] float PeakPhoneme(RE::BSFaceGenAnimationData* a_data)
		{
			if (!a_data) {
				return -1.0f;
			}

			const auto& channel = a_data->phenomeKeyFrame;
			if (!channel.values || channel.count == 0 || channel.count > 256) {
				return -1.0f;
			}

			float peak = 0.0f;
			for (std::uint32_t i = 0; i < channel.count; ++i) {
				peak = std::max(peak, std::fabs(channel.values[i]));
			}
			return peak;
		}

		void Push(const Shape& a_shape, float a_seconds)
		{
			phones.push_back({ a_shape, a_seconds * kSpeechRate });
		}

		// --- Line length from the .fuz ------------------------------------------
		//
		// A per-phoneme estimate drifts against a real recording, so the length is
		// read from the file being played:
		//
		//   00        "FUZE"
		//   04        uint32 version (1)
		//   08        uint32 lip data size
		//   12        lip data (FaceFX, not parsed)
		//   12+size   RIFF/XWMA audio; its dpds chunk gives the length
		//
		// The duration comes from the RIFF, so nothing depends on the FaceFX format.

		// Spaces and characters NTFS won't accept become underscores; everything else
		// is kept. From the shipped packs:
		//
		//   Why_don't_you_charge_a_flat_fee_like_other_mercs_.fuz   ' kept, ? -> _
		//   We_shall_meet_again_in_battle,_then..fuz                , and . kept
		//   There's_no_time_for_this!.fuz                           ! kept
		//
		// Paths are built from UTF-8 explicitly. A std::filesystem::path made from a
		// plain std::string widens through the ANSI codepage on Windows, which turns a
		// Japanese topic or pack folder into a path that doesn't exist. The char8_t
		// cast tells the constructor the bytes are UTF-8.
		[[nodiscard]] std::filesystem::path Utf8Path(std::string_view a_text)
		{
			return std::filesystem::path{
				std::u8string_view{ reinterpret_cast<const char8_t*>(a_text.data()), a_text.size() }
			};
		}

		[[nodiscard]] std::string Sanitize(std::string_view a_text)
		{
			constexpr std::string_view kIllegal = "<>:\"/\\|?*";

			std::string out;
			out.reserve(a_text.size());
			for (const char c : a_text) {
				const bool unsafe = c == ' ' || static_cast<unsigned char>(c) < 0x20 ||
					kIllegal.find(c) != std::string_view::npos;
				out.push_back(unsafe ? '_' : c);
			}
			return out;
		}

		// An older naming rule, tried as a second guess. One extra exists() per pack
		// is cheaper than falling back to an estimate.
		[[nodiscard]] std::string SanitizeAlnum(std::string_view a_text)
		{
			std::string out;
			out.reserve(a_text.size());
			for (const char c : a_text) {
				const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					(c >= '0' && c <= '9');
				out.push_back(alnum ? c : '_');
			}
			return out;
		}

		// Seconds of audio in a .fuz, or 0 if it can't be read.
		[[nodiscard]] float FuzSeconds(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return 0.0f;
			}

			char header[12]{};
			if (!file.read(header, sizeof(header)) ||
				std::memcmp(header, "FUZE", 4) != 0) {
				return 0.0f;
			}

			std::uint32_t lipSize = 0;
			std::memcpy(&lipSize, header + 8, sizeof(lipSize));

			const auto total = std::filesystem::file_size(a_path);
			const auto audioAt = static_cast<std::uintmax_t>(12) + lipSize;
			if (audioAt + 44 >= total) {
				return 0.0f;
			}

			file.seekg(static_cast<std::streamoff>(audioAt), std::ios::beg);

			// RIFF header, then walk the chunks for 'fmt ' and 'data'. Walk rather than
			// assume offsets: XWMA has a 'dpds' chunk between them.
			char riff[12]{};
			if (!file.read(riff, sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0) {
				return 0.0f;
			}

			std::uint16_t channels = 0;
			std::uint32_t sampleRate = 0;
			std::uint32_t bytesPerSecond = 0;
			std::uint16_t bitsPerSample = 0;
			std::uint32_t dataBytes = 0;
			std::uint32_t decodedBytes = 0;  // last dpds entry; 0 when there is no dpds

			for (int guard = 0; guard < 32; ++guard) {
				char id[4]{};
				std::uint32_t size = 0;
				if (!file.read(id, 4) || !file.read(reinterpret_cast<char*>(&size), 4)) {
					break;
				}

				if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
					char fmt[16]{};
					if (!file.read(fmt, sizeof(fmt))) {
						break;
					}
					std::memcpy(&channels, fmt + 2, sizeof(channels));
					std::memcpy(&sampleRate, fmt + 4, sizeof(sampleRate));
					std::memcpy(&bytesPerSecond, fmt + 8, sizeof(bytesPerSecond));
					std::memcpy(&bitsPerSample, fmt + 14, sizeof(bitsPerSample));
					file.seekg(size - 16, std::ios::cur);
				} else if (std::memcmp(id, "dpds", 4) == 0 && size >= 4 && (size % 4) == 0) {
					// dpds holds a running total of decoded bytes per packet, so the last entry is
					// the size of the whole stream as PCM.
					file.seekg(size - 4, std::ios::cur);
					if (!file.read(reinterpret_cast<char*>(&decodedBytes), 4)) {
						break;
					}
					if (size & 1) {
						file.seekg(1, std::ios::cur);
					}
				} else if (std::memcmp(id, "data", 4) == 0) {
					dataBytes = size;
					break;
				} else {
					file.seekg(size + (size & 1), std::ios::cur);  // chunks are word-aligned
				}
			}

			// XWMA is compressed, so data size / nAvgBytesPerSec doesn't give the duration
			// (it came out 15-52% long). The last dpds entry is the decoded PCM size, and
			// PCM has a fixed rate, so this division is exact. Checked against DBReV's own
			// measurements of the same files.
			const std::uint32_t frameBytes =
				static_cast<std::uint32_t>(channels) * (bitsPerSample / 8u);

			if (decodedBytes > 0 && sampleRate > 0 && frameBytes > 0) {
				return static_cast<float>(decodedBytes) /
				       static_cast<float>(sampleRate * frameBytes);
			}

			// No dpds means plain PCM, where the byte rate is real and the simple formula
			// is correct.
			if (bytesPerSecond == 0 || dataBytes == 0) {
				return 0.0f;
			}

			return static_cast<float>(dataBytes) / static_cast<float>(bytesPerSecond);
		}

		// Every voice pack directory, enumerated once. Simpler than reading each voice
		// mod's own config: DBVO 1, DBVO 2 and DBReV store the selected pack
		// differently but lay out the audio the same way.
		[[nodiscard]] const std::vector<std::filesystem::path>& VoicePacks()
		{
			static std::vector<std::filesystem::path> packs = [] {
				std::vector<std::filesystem::path> found;
				std::error_code ec;
				const std::filesystem::path root{ "Data/Sound/DBVO" };
				for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
					if (!ec && entry.is_directory(ec)) {
						found.push_back(entry.path());
					}
				}
				Log::Info(Log::Category::kStaging,
					"LipSync: {} voice pack folder(s) under Data/Sound/DBVO."sv, found.size());
				return found;
			}();
			return packs;
		}

		// The spoken length of this topic, or 0 if the file can't be found.
		[[nodiscard]] float MeasureTopic(std::string_view a_topic)
		{
			if (a_topic.empty()) {
				return 0.0f;
			}

			const std::string names[]{ Sanitize(a_topic) + ".fuz",
				SanitizeAlnum(a_topic) + ".fuz" };
			std::error_code ec;

			// The pack actually being played comes first. With several packs installed,
			// the first file with the right name is often another actor's recording of the
			// same line, at a different pace. Compat::VoiceCommand reads the pack from the
			// SpeakSound call; it's empty until the first line of the session, and the
			// general search below covers that.
			if (const std::string active = Compat::VoiceCommand::Pack(); !active.empty()) {
				const std::filesystem::path root =
					std::filesystem::path{ "Data/Sound/DBVO" } / Utf8Path(active);

				for (const auto& name : names) {
					const auto candidate = root / Utf8Path(name);
					if (std::filesystem::exists(candidate, ec)) {
						const float seconds = FuzSeconds(candidate);
						if (seconds > 0.05f) {
							return seconds;
						}
					}
				}

				// Fall through: no pack covers every topic, and another actor's length is
				// still better than a pure text estimate.
			}

			for (const auto& name : names) {
				for (const auto& pack : VoicePacks()) {
					const auto candidate = pack / Utf8Path(name);
					if (std::filesystem::exists(candidate, ec)) {
						const float seconds = FuzSeconds(candidate);
						if (seconds > 0.05f) {
							return seconds;
						}
					}
				}
			}

			return 0.0f;
		}

		// The length of the exact file the voice mod handed the engine, resolved the
		// way the engine resolves it (relative to Data/Sound). Can't land on another
		// actor's recording or miss on punctuation differences.
		[[nodiscard]] float MeasureVoiceFile(std::string_view a_path)
		{
			if (a_path.empty()) {
				return 0.0f;
			}

			const auto file = std::filesystem::path{ "Data/Sound" } / Utf8Path(a_path);

			std::error_code ec;
			if (!std::filesystem::exists(file, ec)) {
				// Probably inside a BSA. Nothing to measure, so the caller uses its estimate;
				// the line and its words are still known.
				return 0.0f;
			}

			const float seconds = FuzSeconds(file);
			return seconds > 0.05f ? seconds : 0.0f;
		}

		// The words, recovered from the filename the voice mod chose. Punctuation is
		// lost (it became underscores), but it's better than no text at all.
		[[nodiscard]] std::string KeyFromPath(std::string_view a_path)
		{
			const auto slash = a_path.find_last_of("/\\");
			auto       name = slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);

			if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos) {
				name = name.substr(0, dot);
			}

			std::string out{ name };
			for (char& c : out) {
				if (c == '_') {
					c = ' ';
				}
			}

			return out;
		}

		// Seed for the command path, which has no handle ID or topic index: the path
		// mixed with the line ordinal.
		[[nodiscard]] std::uint32_t PathHash(std::string_view a_text) noexcept
		{
			std::uint32_t hash = 2166136261u;
			for (const char c : a_text) {
				hash ^= static_cast<std::uint8_t>(c);
				hash *= 16777619u;
			}
			return hash;
		}

		// Turn the line into a sequence of mouth positions. Digraphs ("sh", "th",
		// "ch") are tested before single letters since each is one position, and a
		// silent trailing "e" is dropped so "mine" doesn't end on Eee.
		//
		// a_measured is the line's real length in seconds when the caller already
		// knows it (DBReV supplies it), or 0 to look up the .fuz here.
		void BuildFromText(std::string_view a_text, float a_measured)
		{
			phones.clear();
			phoneCursor = 0;
			phoneRemaining = 0.0f;

			const auto lower = [](char c) {
				return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
			};
			const auto isAlpha = [](char c) {
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			};

			for (std::size_t i = 0; i < a_text.size();) {
				const char c = lower(a_text[i]);

				if (!isAlpha(c)) {
					// Sentence punctuation is a real pause; a space is a short one.
					if (c == '.' || c == ',' || c == '?' || c == '!' || c == ';' || c == ':') {
						Push(kRest, kPunctuationSeconds);
					} else if (c == ' ') {
						Push(kSoftRest, kWordGapSeconds);
					}
					++i;
					continue;
				}

				const char n = (i + 1 < a_text.size()) ? lower(a_text[i + 1]) : '\0';

				// Two-letter clusters that are one mouth position.
				if (n != '\0') {
					const auto pair = [&](char x, char y) { return c == x && n == y; };

					if (pair('t', 'h')) { Push(kTh, kConsonantSeconds); i += 2; continue; }
					if (pair('s', 'h') || pair('c', 'h')) { Push(kCh, kConsonantSeconds); i += 2; continue; }
					if (pair('p', 'h')) { Push(kFv, kConsonantSeconds); i += 2; continue; }
					if (pair('w', 'h')) { Push(kWuh, kConsonantSeconds); i += 2; continue; }
					if (pair('c', 'k')) { Push(kKg, kConsonantSeconds); i += 2; continue; }
					if (pair('n', 'g') || pair('k', 'n')) { Push(kNl, kConsonantSeconds); i += 2; continue; }
					if (pair('q', 'u')) { Push(kKg, kConsonantSeconds); Push(kWuh, kConsonantSeconds); i += 2; continue; }
					if (pair('o', 'o') || pair('o', 'u') || pair('e', 'w')) { Push(kOoh, kVowelSeconds); i += 2; continue; }
					if (pair('e', 'e') || pair('e', 'a')) { Push(kEee, kVowelSeconds); i += 2; continue; }
					if (pair('o', 'w')) { Push(kOh, kVowelSeconds); i += 2; continue; }
					if (pair('a', 'i') || pair('a', 'y')) { Push(kAah, kVowelSeconds); i += 2; continue; }
					if (pair('o', 'a')) { Push(kOh, kVowelSeconds); i += 2; continue; }
				}

				// A trailing silent "e" ("mine", "have", "there").
				const bool wordEnd = (i + 1 >= a_text.size()) || !isAlpha(a_text[i + 1]);
				if (c == 'e' && wordEnd && i > 0 && isAlpha(a_text[i - 1])) {
					++i;
					continue;
				}

				switch (c) {
				case 'a': Push(kAah, kVowelSeconds); break;
				case 'e': Push(kEh, kVowelSeconds); break;
				case 'i': Push(kIh, kVowelSeconds); break;
				case 'o': Push(kOh, kVowelSeconds); break;
				case 'u': Push(kOoh, kVowelSeconds); break;
				case 'y': Push(kEee, kVowelSeconds); break;

				case 'm': case 'b': case 'p': Push(kBmp, kConsonantSeconds); break;
				case 'f': case 'v':           Push(kFv, kConsonantSeconds); break;
				case 'w':                     Push(kWuh, kConsonantSeconds); break;
				case 'r':                     Push(kRr, kConsonantSeconds); break;
				case 'n': case 'l':           Push(kNl, kConsonantSeconds); break;
				case 'k': case 'g': case 'q': case 'x': Push(kKg, kConsonantSeconds); break;
				case 'j':                     Push(kCh, kConsonantSeconds); break;
				case 'd': case 't': case 's': case 'z': case 'c':
					Push(kDst, kConsonantSeconds); break;

				// 'h' on its own is breath, no mouth position.
				case 'h': break;
				default: break;
				}
				++i;
			}

			// Fit the sequence to the recording: the shapes come from the text and the
			// length from the file, so the mouth finishes when the voice does. Clamped, so
			// a mismatched file degrades gracefully instead of holding one shape for
			// seconds. DBReV's number comes first; it measures the file it's about to
			// play.
			const float measured = a_measured > 0.0f ? a_measured : MeasureTopic(a_text);
			if (measured <= 0.0f) {
				lineSeconds = 0.0f;
				return;
			}

			// No phonemes doesn't mean no measurement. Non-Latin text produces no
			// phonemes, but the length measured from the .fuz is still valid, so keep it:
			// the mouth falls back to the synthesized cadence and still stops when the
			// voice does.
			if (phones.empty()) {
				lineSeconds = measured;

				// Nothing to scale; don't leave the previous line's values around.
				lineScale = 1.0f;
				lineGapScale = 1.0f;
				return;
			}

			// Slack goes into the pauses, not the syllables. A recording has roughly fixed
			// silence at the ends plus the actor's pauses, so stretching every vowel to
			// fill a short line looks like slow motion. When the file is longer than the
			// words, the gaps take most of the extra; when it's shorter, everything
			// compresses.
			float speech = 0.0f;
			float gaps = 0.0f;
			for (const auto& phone : phones) {
				(phone.shape.weight > 0.0f ? speech : gaps) += phone.seconds;
			}

			const float estimated = speech + gaps;
			if (estimated <= 0.01f) {
				// A sequence that sums to nothing can't be scaled; drop the shapes (the caller
				// falls back to the synthesized cadence) but keep the measurement.
				phones.clear();
				lineSeconds = measured;
				lineScale = 1.0f;
				lineGapScale = 1.0f;
				return;
			}

			const float ratio = measured / estimated;
			float       speechScale = std::clamp(ratio, 0.4f, 2.5f);
			float       gapScale = speechScale;

			// Only when there's slack and somewhere to put it.
			if (ratio > 1.0f && gaps > 0.02f) {
				// Syllables never stretch. Since the line is fitted to its measured length,
				// the only way to articulate faster is to not fill the whole file with
				// syllables.
				constexpr float kMaxSyllableStretch = 1.0f;
				speechScale = std::min(ratio, kMaxSyllableStretch);

				// The gaps only take a modest share too, or the mouth stops dead mid sentence.
				// Whatever is left isn't scheduled: the sequence ends early and the mouth
				// rests until the line ends, which is what the tail of a recording looks like.
				constexpr float kMaxGapStretch = 2.2f;
				gapScale = std::clamp((measured - speech * speechScale) / gaps, 1.0f, kMaxGapStretch);
			}

			for (auto& phone : phones) {
				phone.seconds *= (phone.shape.weight > 0.0f) ? speechScale : gapScale;
			}

			lineSeconds = measured;
			lineScale = speechScale;
			lineGapScale = gapScale;
		}

		// Pick the next mouth position. Stress varies per shape rather than per line;
		// real speech has loud and swallowed syllables.
		void ChooseShape()
		{
			shapeRemaining = kShapeHoldMin + RandomUnit() * (kShapeHoldMax - kShapeHoldMin);

			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				target[i] = 0.0f;
			}

			if (static_cast<int>(NextRandom() % static_cast<std::uint32_t>(kRestOneIn)) == 0) {
				return;  // a rest: everything eases toward closed
			}

			const auto& shape = kShapes[NextRandom() % (sizeof(kShapes) / sizeof(kShapes[0]))];
			const float stress = 0.55f + RandomUnit() * 0.45f;

			target[kSlotJaw] = shape.jaw * stress;
			if (shape.slot < kSlotCount) {
				target[shape.slot] = shape.weight * stress;
			}
		}

		void EndLine()
		{
			// Closing the mouth eases the target to zero and lets the approach run, so
			// every write still goes through FaceGen and the mouth closes over about 50 ms
			// instead of snapping.
			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				target[i] = 0.0f;
			}

			lineActive = false;
			overran = false;
			lineElapsed = 0.0f;
			audioWindow = 0.0f;
			shapeRemaining = 0.0f;
			windowSampled = false;
			windowMin = 0.0f;
			windowMax = 0.0f;
			// `driving` stays set until the mouth has actually eased shut; see Update.

		}
	}

	void LipSync::Configure(bool a_enabled, int a_strength)
	{
		const bool wasEnabled = enabled;

		enabled = a_enabled;
		strengthPercent = std::clamp(a_strength, 0, 100);
		strength = static_cast<float>(strengthPercent) / 100.0f;

		// Square root: above the linear curve between the ends and equal at both, so 0
		// is still closed and 100 is still the table's own weights.
		lipStrength = std::sqrt(strength);

		if (!enabled) {
			writing.store(false, std::memory_order_relaxed);
		}

		// Enabled after startup (menu or ini edit). The hook is only installed at
		// load, so install it now; Install() returns immediately if it already ran.
		if (enabled && !wasEnabled && !FaceGen::Installed()) {
			FaceGen::Install();
			Log::Info(Log::Category::kStaging,
				"LipSync: enabled after startup; installing the morph hook now."sv);
		}

		// Logged on every call, so a mid-session toggle shows up.
		Log::Info(Log::Category::kStaging,
			"LipSync: synthesis {}, strength {:.2f} (lips {:.2f})."sv,
			enabled ? "ENABLED"sv : "disabled"sv, strength, lipStrength);
	}

	bool LipSync::Enabled() noexcept
	{
		return enabled;
	}

	int LipSync::StrengthPercent() noexcept
	{
		return strengthPercent;
	}

	void LipSync::Engage()
	{
		engaged = true;
		highlightedTopic.clear();
		verdictReported.Reset();

		// A new conversation retires nothing. Carrying a retired ID across
		// conversations would let one ID collision cost a line its mouth.
		activeVoiceID = RE::BSSoundHandle::kInvalidID;
		consumedVoiceID = RE::BSSoundHandle::kInvalidID;
	}

	void LipSync::Release()
	{
		// EndLine only eases the target to zero; Update carries the face the rest of
		// the way (it keeps running after the conversation closes). Snapping to zero
		// here would put a visible jump on the last frame.
		EndLine();
		highlightedTopic.clear();
		engaged = false;
	}

	void LipSync::Update(float a_delta)
	{
		// The camera handoff still needs voice timing when all face effects are off.
		if (!engaged && !enabled && !Performance::ExpressionsEnabled() && !lineActive && !driving) return;

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		// --- Where the line's start and end come from ---------------------------
		//
		// 1. DBReV broadcasts a start when it asks the engine to speak and an end
		//    when the dialogue advances. It's the only source that knows the
		//    post-line delay; when present it's used for everything.
		// 2. Otherwise the SpeakSound call. Every DBVO version plays the player's
		//    line with `Player.SpeakSound "DBVO/<pack>/<line>.fuz"`, which gives the
		//    exact file and the start time. No end event, so the mouth closes on
		//    the measured length.
		// 3. Otherwise sound handle polling.
		//
		// Polling can't tell the player's dialogue line from other sounds on the
		// player, so with an announcing source present, no announcement means the
		// player didn't speak and the mouth stays shut.
		//
		// EverSpoke / EverHeard rather than Present / Install: either mod can be
		// loaded while something else voices the player, so each only takes over once
		// it has actually announced a line.
		if (!lineActive && Interface::ReadDialoguePhase().phase == Interface::MenuPhase::kTopicList) {
			highlightedTopic = Interface::ReadSelectedTopic();
		}
		const bool eventDriven = Compat::DBReV::Present() && Compat::DBReV::EverSpoke();
		const bool commandDriven = !eventDriven && Compat::VoiceCommand::EverHeard();

		bool          startNow = false;
		std::uint32_t seed = 0;
		float         supplied = 0.0f;
		float         suppliedTotal = 0.0f;
		std::string   suppliedKey;

		// Which source supplied the length, for the log. Only read when `supplied` is
		// set.
		std::string_view suppliedFrom{};

		if (eventDriven) {
			// End first, then start: a superseded line's end and its replacement's start
			// can arrive in the same frame in either order.
			std::uint32_t reason = 0;
			if (Compat::DBReV::TakeLineEnd(reason) && lineActive) {
				// Usually a no-op, since the mouth already closed at audioSeconds and this
				// arrives after the post-line delay. It matters for a skip, which arrives
				// early.
				Log::Info(Log::Category::kStaging,
					"LipSync: line ended ({}) at {:.2f}s."sv,
					Compat::DBReV::EndReasonName(reason), lineElapsed);
				EndLine();
			}

			Compat::DBReV::Line line;
			if (Compat::DBReV::TakeLineStart(line)) {
				if (lineActive) {
					EndLine();  // superseded without an end having arrived yet
				}
				startNow = true;
				supplied = line.audioSeconds;
				suppliedFrom = "from DBReV"sv;
				suppliedTotal = line.totalSeconds;
				suppliedKey = line.topicKey;

				// Not the topic index alone: it's a position in the list (the top option is
				// always 0), so every line clicked in the same slot would get the same mouth.
				// A line counter keeps them apart; the index separates topics within a turn.
				seed = (++lineOrdinal * 2654435761u) ^ (line.topicIndex + 1u);
			}
		} else if (commandDriven) {
			// No end event on this path: SpeakSound only says a line started, so a line
			// skipped by clicking still runs its measured length. The window is the length
			// of the file the command named.
			//
			// On a DBReV profile the first line of a session can start here and then be
			// taken over by DBReV's event a frame later (the mouth restarts within 100
			// ms). After that EverSpoke is set and this branch isn't used.
			Compat::VoiceCommand::Line line;
			if (Compat::VoiceCommand::TakeLineStart(line)) {
				if (lineActive) {
					EndLine();  // a topic clicked before the last line finished
				}

				startNow = true;
				supplied = MeasureVoiceFile(line.path);
				suppliedFrom = "from the file SpeakSound named"sv;

				// No totalSeconds here: the post-line delay is a setting inside the voice mod.
				suppliedKey = KeyFromPath(line.path);

				seed = (++lineOrdinal * 2654435761u) ^ PathHash(line.path);
			}
		} else {
			const std::uint32_t voiceID = PlayingVoiceID(HighOf(player));
			const bool          playing = voiceID != RE::BSSoundHandle::kInvalidID;

			if (!playing) {
				if (lineActive) {
					EndLine();
				}

				// The handle went idle, so the next one is a genuinely new line.
				consumedVoiceID = RE::BSSoundHandle::kInvalidID;
			}

			// A handle whose line has already played out doesn't start another. The audio
			// window closes the line at its measured end, but the handle often stays in
			// kPlaying, which would otherwise look like a new line and replay the
			// sequence. Director::VoicePlaying handles the same thing with retiredVoiceID.
			startNow = playing && !lineActive && voiceID != consumedVoiceID;
			seed = voiceID;

			if (startNow) {
				activeVoiceID = voiceID;
			}
		}

		if (startNow) {
			lineActive = true;
			driving = true;  // from the first frame; see below
			overran = false;
			lineElapsed = 0.0f;
			windowSampled = false;
			windowMin = 0.0f;
			windowMax = 0.0f;

			// Zero and one are degenerate seeds for xorshift, hence the salt.
			rng = seed ? (seed * 2654435761u) | 1u : 1u;
			shapeRemaining = 0.0f;

			// The words, read from the menu's own list at the moment the voice starts,
			// while the highlight is still on the spoken line. Preferred over DBReV's
			// topicKey and the filename, which have their punctuation replaced by
			// underscores, and punctuation is what places the pauses.
			std::string topic = Interface::ReadSelectedTopic();
			if (topic.empty()) topic = highlightedTopic;
			highlightedTopic.clear();
			if (topic.empty() && !suppliedKey.empty()) {
				topic = suppliedKey;
				Log::Info(Log::Category::kStaging,
					"LipSync: topic unreadable from the movie; falling back to {} \"{}\"."sv,
					eventDriven ? "DBReV's key"sv : "the filename"sv, topic);
			}

			voiceLineText = topic;
			voiceLineAnnounced = eventDriven || commandDriven;
			++voiceLineSerial;
			BuildFromText(topic, supplied);
			fromText = !phones.empty();

			// When the mouth stops. Set after BuildFromText, which may have measured the
			// .fuz: lineSeconds is whichever measurement landed (DBReV's audioSeconds or
			// the file read here), so this works for DBVO profiles and non-English text
			// too. suppliedTotal is the last resort: DBReV couldn't measure the audio, so
			// the conversation window is all there is.
			audioWindow = lineSeconds > 0.0f ? lineSeconds :
			              (supplied > 0.0f ? supplied : suppliedTotal);

			// Start the schedule a lead's worth in so the smoothed mouth arrives with the
			// audio.
			phoneRemaining = -kLeadSeconds;

			if (fromText) {
				if (lineSeconds > 0.0f) {
					Log::Info(Log::Category::kStaging,
						"LipSync: \"{}\" - {} positions over {:.2f}s {} (syllables x{:.2f}, pauses x{:.2f})."sv,
						topic, phones.size(), lineSeconds,
						supplied > 0.0f ? suppliedFrom : "from the .fuz"sv,
						lineScale, lineGapScale);
				} else {
					// Why there's no measurement, by source. "No .fuz matched" only applies to the
					// polling path; the other two knew the file and couldn't read it.
					const std::string_view why =
						eventDriven   ? "DBReV could not measure the file"sv :
						commandDriven ? "the file SpeakSound named could not be measured"sv :
						                "no .fuz matched"sv;

					Log::Info(Log::Category::kStaging,
						"LipSync: \"{}\" - {} positions, ESTIMATED length ({})."sv,
						topic, phones.size(), why);
				}
			} else if (verdictReported.Take()) {
				// Why there are no shapes (unreadable topic, or text that isn't a-z) and
				// whether there's still a measured end. A synthesized cadence with a measured
				// end still stops with the voice.
				const std::string_view clock = audioWindow <= 0.0f ?
					"nothing to stop it but the stuck-line backstop"sv :
					lineSeconds > 0.0f ?
					"stopping on the measured length of the recording"sv :
					"stopping on the conversation window - no file was measured"sv;

				if (topic.empty()) {
					Log::Info(Log::Category::kStaging,
						"LipSync: no topic text readable; synthesized cadence, {}."sv, clock);
				} else {
					Log::Info(Log::Category::kStaging,
						"LipSync: \"{}\" holds no letters the phoneme builder reads - a non-Latin "
						"localisation does exactly this; synthesized cadence, {}."sv,
						topic, clock);
				}
			}
		}

		if (lineActive) {
			lineElapsed += a_delta;


			// The audio has stopped, so the mouth stops. audioSeconds, not totalSeconds:
			// totalSeconds includes the voice mod's post-line delay, which would leave the
			// mouth moving after the voice ended. EndLine eases it shut.
			if (audioWindow > 0.0f && lineElapsed >= audioWindow) {
				// Retire the handle too, or it immediately looks like a new line. See
				// consumedVoiceID.
				consumedVoiceID = activeVoiceID;
				EndLine();
			}
		}

		if (lineActive && !overran && lineElapsed > kMaxLineSeconds) {
			// Last-resort backstop. Should be unreachable when the file could be measured;
			// still guards the handle-polling path and unmeasurable files.
			overran = true;
			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				target[i] = 0.0f;
			}
			Log::Warn(Log::Category::kStaging,
				"LipSync: the player's line has run past {:.0f}s with no end. Treating it as stuck and closing the mouth."sv,
				kMaxLineSeconds);
		}

		if (!driving) return;

		// Driving starts on the line's first frame. The observation window is only
		// reported: the only other activity on the player's phoneme channel comes from
		// expression mods, not lip sync, so there's nothing to defer to.
		if (enabled && lineActive && !windowSampled) {
			const float peak = PeakPhoneme(FaceOf(player));
			if (peak >= 0.0f) {
				windowSampled = true;
				windowMin = peak;
				windowMax = peak;
				if (verdictReported.Take()) {
					Log::Info(Log::Category::kStaging,
						"LipSync: driving the player's mouth. Channel was at {:.3f} on entry - reported, not acted on."sv,
						peak);
				}
			}
		}

		if (lineActive && !overran) {
			if (fromText) {
				phoneRemaining -= a_delta;
				while (phoneRemaining <= 0.0f && phoneCursor < phones.size()) {
					const auto& phone = phones[phoneCursor++];
					phoneRemaining += phone.seconds;

					for (std::uint32_t i = 0; i < kSlotCount; ++i) {
						target[i] = 0.0f;
					}

					// A little stress variation, kept narrow; the shapes follow the words.
					const float stress = 0.78f + RandomUnit() * 0.22f;
					target[kSlotJaw] = phone.shape.jaw * stress;
					if (phone.shape.slot < kSlotCount && phone.shape.weight > 0.0f) {
						target[phone.shape.slot] = phone.shape.weight * stress;
					}
				}

				// The words ran out before the audio (short estimate, or a tail on the
				// recording). Rest rather than invent more.
				if (phoneCursor >= phones.size() && phoneRemaining <= 0.0f) {
					for (std::uint32_t i = 0; i < kSlotCount; ++i) {
						target[i] = 0.0f;
					}
				}
			} else {
				shapeRemaining -= a_delta;
				if (shapeRemaining <= 0.0f) {
					ChooseShape();
				}
			}
		}

		// Exponential approach, framerate-independent, per direction.
		const float attack = 1.0f - std::exp(-a_delta / kAttackSeconds);
		const float release = 1.0f - std::exp(-a_delta / kReleaseSeconds);
		const float jawAttack = 1.0f - std::exp(-a_delta / kJawAttackSeconds);
		const float jawRelease = 1.0f - std::exp(-a_delta / kJawReleaseSeconds);

		float peak = 0.0f;
		for (std::uint32_t i = 0; i < kSlotCount; ++i) {
			const bool  opening = target[i] > current[i];
			const bool  jaw = i == kSlotJaw;
			const float rate = jaw ? (opening ? jawAttack : jawRelease) :
			                         (opening ? attack : release);

			current[i] += (target[i] - current[i]) * rate;
			peak = std::max(peak, current[i]);
		}

		// Once the line is over and the mouth has eased shut, stop writing and hand
		// the channel back. Stopping as soon as the handle cleared would freeze the
		// last shape.
		if (!lineActive && peak < 0.002f) {
			for (std::uint32_t i = 0; i < kSlotCount; ++i) {
				current[i] = 0.0f;
			}
			driving = false;
		}

		writing.store(enabled && driving, std::memory_order_relaxed);

	}

	bool LipSync::PlayerSpeaking() noexcept
	{
		return lineActive && !overran;
	}

	bool LipSync::PlayerLineAnnounced() noexcept
	{
		return voiceLineAnnounced;
	}

	std::uint64_t LipSync::PlayerLineSerial() noexcept
	{
		return voiceLineSerial;
	}

	std::string_view LipSync::PlayerLineText() noexcept
	{
		return voiceLineText;
	}

	float LipSync::PlayerLineDuration() noexcept
	{
		return audioWindow;
	}

	float LipSync::PlayerLineElapsed() noexcept
	{
		return lineElapsed;
	}

	void LipSync::OnResponse()
	{
		// A reply retires stale kPlaying handles so they can't restart the old line.
		consumedVoiceID = PlayingVoiceID(HighOf(RE::PlayerCharacter::GetSingleton()));
		EndLine();
		highlightedTopic.clear();
	}

	bool LipSync::Sample(float* a_out, std::uint32_t a_count) noexcept
	{
		if (!a_out || !writing.load(std::memory_order_relaxed)) {
			return false;
		}

		const std::uint32_t count = std::min(a_count, kSlotCount);
		for (std::uint32_t i = 0; i < count; ++i) {
			// The jaw takes the strength linearly; the lips take the softer curve. See
			// lipStrength.
			const float scale = (i == kSlotJaw) ? strength : lipStrength;
			a_out[i] = std::clamp(current[i] * scale, 0.0f, 1.0f);
		}
		return true;
	}
}

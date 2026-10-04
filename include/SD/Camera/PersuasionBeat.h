#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace SD::Camera
{
	// Persuade, intimidate, bribe: the NPC's answer is the shot. The beat cuts to
	// their close-up when the reply starts, holds it for the whole answer and
	// pushes in slowly. Pure, so the classifier and state can be tested; the
	// Director reads the conditions from the engine and drives the camera.
	enum class SpeechCheck : std::uint8_t
	{
		kNone,
		kPersuade,
		kIntimidate,
		kBribe
	};

	[[nodiscard]] constexpr std::string_view SpeechCheckName(SpeechCheck a_check) noexcept
	{
		switch (a_check) {
		case SpeechCheck::kPersuade:   return "Persuade";
		case SpeechCheck::kIntimidate: return "Intimidate";
		case SpeechCheck::kBribe:      return "Bribe";
		default:                       return "none";
		}
	}

	// One condition on a reply, as much as the classifier needs: the function, its
	// first parameter as an integer, the operator, and whether the comparison
	// value is a global.
	struct CheckCondition
	{
		std::uint16_t function{ 0 };
		std::uint32_t parameter{ 0 };
		std::uint8_t  op{ 0 };  // 0 ==, 1 !=, 2 >, 3 >=, 4 <, 5 <=
		bool          global{ false };
	};

	namespace SpeechChecks
	{
		// How the game marks a speech check. Skyrim has topic subtypes for intimidate,
		// flatter and bribe, but barely uses them (one topic each in Skyrim.esm). Real
		// checks are ordinary topics whose replies carry the test as a condition:
		//
		//   GetIntimidateSuccess                 55 replies in Skyrim.esm
		//   GetBribeSuccess                      43
		//   GetActorValue Speech >= <global>     87 (SpeechEasy, SpeechAverage, ...)
		//
		// The skill test only counts against a global with >=. Speech compared to a
		// plain number is something else (a merchant remark pool checks Speech >= 30).
		constexpr std::uint16_t kGetActorValue = 14;
		constexpr std::uint16_t kGetBaseActorValue = 277;
		constexpr std::uint16_t kGetBribeAmount = 653;
		constexpr std::uint16_t kGetBribeSuccess = 654;
		constexpr std::uint16_t kGetIntimidateSuccess = 655;
		constexpr std::uint32_t kSpeechSkill = 17;
		constexpr std::uint8_t  kGreaterOrEqual = 3;

		// The failure reply has no condition: a check is a topic with a conditioned
		// success and an unconditioned fallback, so the failure is found through its
		// siblings. Check topics in the vanilla masters have twelve replies or fewer;
		// a bigger topic is a pool, where only a reply carrying the condition itself
		// counts.
		constexpr std::size_t kMaxCheckTopic = 12;

		[[nodiscard]] constexpr SpeechCheck Classify(const CheckCondition& a_condition) noexcept
		{
			switch (a_condition.function) {
			case kGetIntimidateSuccess:
				return SpeechCheck::kIntimidate;
			case kGetBribeSuccess:
			case kGetBribeAmount:
				return SpeechCheck::kBribe;
			case kGetActorValue:
			case kGetBaseActorValue:
				return a_condition.parameter == kSpeechSkill && a_condition.op == kGreaterOrEqual &&
				               a_condition.global ?
				           SpeechCheck::kPersuade :
				           SpeechCheck::kNone;
			default:
				return SpeechCheck::kNone;
			}
		}

		[[nodiscard]] constexpr SpeechCheck Classify(std::span<const CheckCondition> a_conditions) noexcept
		{
			for (const auto& condition : a_conditions) {
				if (const auto check = Classify(condition); check != SpeechCheck::kNone) {
					return check;
				}
			}
			return SpeechCheck::kNone;
		}

		// The prompt as a second opinion, for modded checks that use a scripted test
		// instead of these conditions. English only; the conditions cover every
		// language.
		[[nodiscard]] inline SpeechCheck ClassifyPrompt(std::string_view a_text) noexcept
		{
			const auto has = [&](std::string_view a_tag) {
				if (a_text.size() < a_tag.size()) {
					return false;
				}
				for (std::size_t i = 0; i + a_tag.size() <= a_text.size(); ++i) {
					bool match = true;
					for (std::size_t j = 0; j < a_tag.size(); ++j) {
						char c = a_text[i + j];
						if (c >= 'A' && c <= 'Z') {
							c = static_cast<char>(c - 'A' + 'a');
						}
						if (c != a_tag[j]) {
							match = false;
							break;
						}
					}
					if (match) {
						return true;
					}
				}
				return false;
			};

			if (has("(persuade)")) {
				return SpeechCheck::kPersuade;
			}
			if (has("(intimidate)")) {
				return SpeechCheck::kIntimidate;
			}
			if (has("(bribe)")) {
				return SpeechCheck::kBribe;
			}
			return SpeechCheck::kNone;
		}

		// How long the push takes: about the length of the answer, from its word count
		// (voice acting runs near 2.5 words a second). Bounded so a one-word answer
		// still moves and a speech doesn't crawl.
		[[nodiscard]] constexpr float PushSeconds(std::uint32_t a_words) noexcept
		{
			const float spoken = a_words > 0 ? static_cast<float>(a_words) / 2.6f + 0.6f : 4.0f;
			return std::clamp(spoken, 2.5f, 8.0f);
		}
	}

	// The beat's state, from the reply starting to the reply ending.
	class PersuasionBeat
	{
	public:
		// A reply to a speech check has started; the next cut goes to the beat shot.
		void Begin(SpeechCheck a_check) noexcept
		{
			check = a_check;
			cutOwed = a_check != SpeechCheck::kNone;
		}

		// The cut was attempted. Spent whether or not the shot placed, so a room that
		// refuses the close-up costs one attempt, not one per frame.
		void Cut() noexcept { cutOwed = false; }

		// The reply is over, or the conversation is.
		void End() noexcept
		{
			check = SpeechCheck::kNone;
			cutOwed = false;
		}

		[[nodiscard]] bool          Active() const noexcept { return check != SpeechCheck::kNone; }
		[[nodiscard]] bool          CutOwed() const noexcept { return cutOwed; }
		[[nodiscard]] bool          Holding() const noexcept { return Active() && !cutOwed; }
		[[nodiscard]] SpeechCheck   Check() const noexcept { return check; }

	private:
		SpeechCheck check{ SpeechCheck::kNone };
		bool        cutOwed{ false };
	};
}

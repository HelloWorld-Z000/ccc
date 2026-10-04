#include "SD/Camera/PersuasionBeat.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "PersuasionBeat: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	using SD::Camera::CheckCondition;
	using SD::Camera::PersuasionBeat;
	using SD::Camera::SpeechCheck;
	namespace Checks = SD::Camera::SpeechChecks;
}

int main()
{
	// The three shapes the masters use.
	Expect(Checks::Classify(CheckCondition{ Checks::kGetIntimidateSuccess, 0, 0, false }) == SpeechCheck::kIntimidate,
		"GetIntimidateSuccess is an intimidation");
	Expect(Checks::Classify(CheckCondition{ Checks::kGetBribeSuccess, 0, 0, false }) == SpeechCheck::kBribe,
		"GetBribeSuccess is a bribe");
	Expect(Checks::Classify(CheckCondition{ Checks::kGetBribeAmount, 0, 0, false }) == SpeechCheck::kBribe,
		"GetBribeAmount is a bribe");
	Expect(Checks::Classify(CheckCondition{ Checks::kGetActorValue, Checks::kSpeechSkill, Checks::kGreaterOrEqual, true }) ==
			   SpeechCheck::kPersuade,
		"Speech >= a global is a persuasion");
	Expect(Checks::Classify(CheckCondition{ Checks::kGetBaseActorValue, Checks::kSpeechSkill, Checks::kGreaterOrEqual, true }) ==
			   SpeechCheck::kPersuade,
		"base Speech >= a global is a persuasion");

	// And the near misses that are not.
	Expect(Checks::Classify(CheckCondition{ Checks::kGetActorValue, Checks::kSpeechSkill, Checks::kGreaterOrEqual, false }) ==
			   SpeechCheck::kNone,
		"Speech >= a plain number is a remark pool, not a check");
	Expect(Checks::Classify(CheckCondition{ Checks::kGetActorValue, Checks::kSpeechSkill, 4, true }) == SpeechCheck::kNone,
		"Speech < a global is not a check");
	Expect(Checks::Classify(CheckCondition{ Checks::kGetActorValue, 12, Checks::kGreaterOrEqual, true }) == SpeechCheck::kNone,
		"another skill against a global is not a speech check");
	Expect(Checks::Classify(CheckCondition{ 116, 0, 0, false }) == SpeechCheck::kNone,
		"IsIntimidatedByPlayer is a state, not a check");

	// A reply's condition list: the first check found decides.
	const std::array conditions{
		CheckCondition{ 72, 0, 0, false },
		CheckCondition{ Checks::kGetIntimidateSuccess, 0, 0, false },
		CheckCondition{ Checks::kGetBribeSuccess, 0, 0, false },
	};
	Expect(Checks::Classify(std::span<const CheckCondition>{ conditions }) == SpeechCheck::kIntimidate,
		"a list classifies by its first check");
	Expect(Checks::Classify(std::span<const CheckCondition>{}) == SpeechCheck::kNone, "no conditions, no check");

	// The prompt tag, case-insensitive, anywhere in the line.
	Expect(Checks::ClassifyPrompt("(Persuade) Surely you can make an exception.") == SpeechCheck::kPersuade,
		"the persuade tag");
	Expect(Checks::ClassifyPrompt("(INTIMIDATE) Talk, or else.") == SpeechCheck::kIntimidate, "the intimidate tag");
	Expect(Checks::ClassifyPrompt("How about some gold? (Bribe)") == SpeechCheck::kBribe, "the bribe tag at the end");
	Expect(Checks::ClassifyPrompt("I'd like to persuade you to come along.") == SpeechCheck::kNone,
		"the word without the tag is not a check");
	Expect(Checks::ClassifyPrompt("") == SpeechCheck::kNone, "an empty prompt is not a check");

	// The push length follows the answer, inside its bounds.
	Expect(Checks::PushSeconds(0) == 4.0f, "an unmeasured answer pushes for four seconds");
	Expect(Checks::PushSeconds(1) == 2.5f, "a one-word answer still moves visibly");
	Expect(Checks::PushSeconds(200) == 8.0f, "a speech does not crawl");
	Expect(Checks::PushSeconds(13) > 5.0f && Checks::PushSeconds(13) < 6.0f, "thirteen words is about five and a half seconds");

	// The state.
	PersuasionBeat beat;
	Expect(!beat.Active() && !beat.CutOwed(), "starts idle");
	beat.Begin(SpeechCheck::kNone);
	Expect(!beat.Active(), "beginning on nothing is nothing");
	beat.Begin(SpeechCheck::kBribe);
	Expect(beat.Active() && beat.CutOwed() && !beat.Holding(), "a reply to a check owes a cut");
	beat.Cut();
	Expect(beat.Active() && !beat.CutOwed() && beat.Holding(), "after the cut the beat holds");
	Expect(beat.Check() == SpeechCheck::kBribe, "and remembers which check it was");
	beat.End();
	Expect(!beat.Active() && !beat.Holding(), "the reply ending ends it");

	std::cout << "PersuasionBeat: all tests passed\n";
	return EXIT_SUCCESS;
}

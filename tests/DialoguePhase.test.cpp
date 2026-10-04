#include "SD/Scene/DialoguePhase.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
	using SD::Scene::DialoguePhase;
	using SD::Scene::MenuPhase;

	void Expect(bool a_ok, const char* a_message)
	{
		if (!a_ok) { std::cerr << a_message << '\n'; std::exit(EXIT_FAILURE); }
	}

	void ExpectUnknown(const DialoguePhase& a_phase)
	{
		Expect(a_phase.phase == MenuPhase::kUnknown && !a_phase.valid && !a_phase.HasKnownPhase(),
			"unsupported or absent phase stays unknown");
		Expect(a_phase.TopicsLive(), "unknown movie state releases the choice fade");
		Expect(!a_phase.CanArmFade(), "unknown state cannot arm a later greeting fade");
	}
}

int main()
{
	ExpectUnknown(DialoguePhase::FromMovie(std::nullopt, std::nullopt));
	for (const bool allowProgress : { false, true }) {
		const auto voiceOnly = DialoguePhase::FromMovie(std::nullopt, allowProgress);
		ExpectUnknown(voiceOnly);
		Expect(voiceOnly.lineInFlight == !allowProgress, "voice readiness remains independently readable");
		for (const double state : { -1.0, 4.0, 99.0, 0.5, 1.5,
			std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() }) {
			const auto unknown = DialoguePhase::FromMovie(state, allowProgress);
			ExpectUnknown(unknown);
			Expect(unknown.lineInFlight == !allowProgress, "unknown state retains readable voice readiness");
		}
	}

	const MenuPhase phases[]{ MenuPhase::kGreeting, MenuPhase::kTopicList,
		MenuPhase::kTopicClicked, MenuPhase::kTransitioning };
	for (int state = 0; state < 4; ++state) {
		const auto known = DialoguePhase::FromMovie(static_cast<double>(state), std::nullopt);
		Expect(known.valid && known.HasKnownPhase() && known.phase == phases[state],
			"known menu states are usable without a voice flag");
		Expect(known.TopicsLive() == (state == 1), "only the known topic list is live");
		Expect(known.CanArmFade() == (state == 1), "only the known topic list arms fading");
	}

	// An unreadable first frame must not make the following greeting disappear.
	bool listWasLive = false;
	const auto initial = DialoguePhase::FromMovie(std::nullopt, true);
	listWasLive = listWasLive || initial.CanArmFade();
	const auto greeting = DialoguePhase::FromMovie(0.0, false);
	listWasLive = listWasLive || greeting.CanArmFade();
	Expect(!listWasLive, "unknown-to-greeting cannot arm a greeting fade");
	const auto choices = DialoguePhase::FromMovie(1.0, true);
	listWasLive = listWasLive || choices.CanArmFade();
	Expect(listWasLive, "a recognized topic list arms subsequent response fading");
	Expect(!DialoguePhase::FromMovie(2.0, false).TopicsLive(), "a known choice commit may fade");
	Expect(DialoguePhase::FromMovie(99.0, false).TopicsLive(), "lost phase support releases an armed fade");

	std::cout << "DialoguePhase tests passed\n";
}

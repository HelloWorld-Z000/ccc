#include "SD/Dialogue/SceneTrigger.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "SceneTrigger: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	using SD::Dialogue::PairKey;
	using SD::Dialogue::SceneTrigger;

	constexpr float kFrame = 0.1f;

	// Frames until it fires, or -1 within the limit.
	int FramesToFire(SceneTrigger& a_trigger, const SceneTrigger::Settings& a_settings,
		SceneTrigger::Input a_input, int a_limit = 200)
	{
		for (int frame = 1; frame <= a_limit; ++frame) {
			if (a_trigger.Update(a_settings, a_input)) {
				return frame;
			}
		}
		return -1;
	}
}

int main()
{
	const SceneTrigger::Settings on{ true, 1.5f };
	const SceneTrigger::Settings off{ false, 1.5f };
	const auto key = PairKey(0x100, 0x200);

	SceneTrigger::Input standing{ true, key, true, false, kFrame };

	// Standing still in front of an exchange: fires once the wait has passed.
	SceneTrigger trigger;
	const int fired = FramesToFire(trigger, on, standing);
	Expect(fired >= 15 && fired <= 16, "fires after a second and a half of standing still");

	// Off is off.
	SceneTrigger idle;
	Expect(FramesToFire(idle, off, standing) == -1, "the automatic mode does nothing when switched off");

	// Moving restarts the wait.
	SceneTrigger walker;
	for (int i = 0; i < 10; ++i) {
		static_cast<void>(walker.Update(on, standing));
	}
	auto moving = standing;
	moving.still = false;
	static_cast<void>(walker.Update(on, moving));
	Expect(walker.StillFor() == 0.0f, "a step resets the wait");
	Expect(FramesToFire(walker, on, standing) >= 15, "and the full wait runs again");

	// Blocked (combat, a menu, the player's own conversation) also resets.
	SceneTrigger blocked;
	auto fighting = standing;
	fighting.blocked = true;
	Expect(FramesToFire(blocked, on, fighting, 50) == -1, "never fires while blocked");

	// Standing still with nothing to film builds up the wait, so a conversation
	// starting in front of somebody who has been standing there is filmed at once.
	SceneTrigger waiting;
	auto empty = standing;
	empty.candidate = false;
	Expect(FramesToFire(waiting, on, empty, 30) == -1, "nothing to film, nothing filmed");
	Expect(FramesToFire(waiting, on, standing) == 1, "a player already standing still is filmed on the first line");

	// The scene the player walked out of is snoozed, others are not.
	SceneTrigger snoozed;
	snoozed.Snooze(key);
	Expect(FramesToFire(snoozed, on, standing, 100) == -1, "the pair just left is not re-filmed for a while");
	auto other = standing;
	other.key = PairKey(0x300, 0x400);
	Expect(FramesToFire(snoozed, on, other) >= 1, "a different pair is filmed as usual");

	SceneTrigger expired;
	expired.Snooze(key, 2.0f);
	const int afterSnooze = FramesToFire(expired, on, standing);
	Expect(afterSnooze >= 20 && afterSnooze <= 21, "and the snooze runs out");

	// A scene ending restarts the wait, so the next one is a fresh decision.
	SceneTrigger restarted;
	auto nothing = standing;
	nothing.candidate = false;
	Expect(FramesToFire(restarted, on, nothing, 40) == -1, "standing still with nothing to film");
	restarted.Restart();
	Expect(FramesToFire(restarted, on, standing) >= 15, "after a scene ends the full wait runs again");

	// Pair keys do not depend on order.
	Expect(PairKey(1, 2) == PairKey(2, 1), "a pair is a pair whichever spoke first");
	Expect(PairKey(1, 2) != PairKey(1, 3), "different pairs differ");

	// Back-and-forth, newest first. A and B are 1 and 2; 3 is somebody else.
	using SD::Dialogue::Exchanges;
	using SD::Dialogue::HeardTurn;
	constexpr float gap = SD::Dialogue::kReplyGapSeconds;

	const std::array chat{ HeardTurn{ 1, 0.0f }, HeardTurn{ 2, 4.0f }, HeardTurn{ 1, 9.0f } };
	Expect(Exchanges(chat, 1, 2, gap) == 2, "A, B, A is two changes of the floor");

	// A remark, then someone else's line to the player 17 seconds later.
	const std::array remarks{ HeardTurn{ 2, 0.0f, true }, HeardTurn{ 1, 17.0f } };
	Expect(Exchanges(remarks, 1, 2, gap) == 0, "a line to the player is not part of their conversation");

	const std::array spaced{ HeardTurn{ 1, 0.0f }, HeardTurn{ 2, 15.0f }, HeardTurn{ 1, 30.0f } };
	Expect(Exchanges(spaced, 1, 2, gap) == 0, "two remarks fifteen seconds apart are not a reply");

	const std::array pair{ HeardTurn{ 1, 0.0f }, HeardTurn{ 2, 3.0f } };
	Expect(Exchanges(pair, 1, 2, gap) == 1, "one reply is one change, not yet a back-and-forth");

	// The same line heard twice (dialogue hook and subtitles) and a third voice
	// nearby change nothing.
	const std::array noisy{ HeardTurn{ 1, 0.0f }, HeardTurn{ 1, 0.05f }, HeardTurn{ 3, 1.0f },
		HeardTurn{ 2, 4.0f }, HeardTurn{ 1, 8.0f } };
	Expect(Exchanges(noisy, 1, 2, gap) == 2, "duplicates and bystanders neither count nor break the run");

	// A line to the player in the middle ends the run there.
	const std::array interrupted{ HeardTurn{ 1, 0.0f }, HeardTurn{ 2, 3.0f, true }, HeardTurn{ 1, 6.0f },
		HeardTurn{ 2, 9.0f } };
	Expect(Exchanges(interrupted, 1, 2, gap) == 0, "the run stops at a line said to the player");

	// What makes a pair worth filming unasked.
	using SD::Dialogue::StrictPair;
	using SD::Dialogue::StrictPairing;
	Expect(StrictPair(true, false, false, 0) == StrictPairing::kOneScene, "two voices in one scene");
	Expect(StrictPair(true, true, true, 0) == StrictPairing::kOneSceneForYou,
		"the Sovngarde council: a scene addressed to the player is still a scene");
	Expect(StrictPair(true, true, false, 0) == StrictPairing::kOneSceneForYou, "one of the two said to you");
	Expect(StrictPair(false, false, false, 2) == StrictPairing::kBackAndForth, "a back-and-forth outside a scene");
	Expect(StrictPair(false, true, false, 2) == StrictPairing::kNone,
		"outside a scene, a line to the player never counts");
	Expect(StrictPair(false, false, true, 3) == StrictPairing::kNone, "nor as the reply");
	Expect(StrictPair(false, false, false, 1) == StrictPairing::kNone, "one reply is not a conversation");

	std::cout << "SceneTrigger: all tests passed\n";
	return EXIT_SUCCESS;
}

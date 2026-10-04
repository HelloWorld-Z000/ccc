#include "SD/Camera/PlayerVoiceHandoff.h"

#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "PlayerVoiceHandoff: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
}

int main()
{
	SD::Camera::PlayerVoiceHandoff handoff;
	handoff.Reset(10);
	handoff.Update(10, false, false, true);
	Expect(!handoff.Active(), "unvoiced choice must not anticipate a reply");
	handoff.Update(11, true, false, false);
	Expect(!handoff.Active(), "retain the player while their audio plays");
	handoff.Update(11, false, false, false);
	Expect(handoff.Active(), "audio end must anticipate before the NPC speaks");
	for (int frame = 0; frame < 120; ++frame) {
		handoff.Update(11, false, false, false);
		Expect(handoff.Active(), "post-line delay must not return to player");
	}
	handoff.Update(11, false, true, false);
	Expect(handoff.Active(), "reply arrival must preserve handoff and waive player beat");
	handoff.Update(11, false, false, false);
	Expect(!handoff.Active(), "NPC reply ending returns coverage to the player");
	handoff.Update(11, false, false, false);
	Expect(!handoff.Active(), "completed serial must not retrigger");

	handoff.Update(12, true, false, false);
	handoff.Update(13, true, false, false);
	Expect(!handoff.Active(), "superseding a voiced line must not create a false end");
	handoff.Update(13, false, true, false);
	Expect(handoff.Active(), "skip straight into reply must waive extra player beat");
	handoff.Update(14, true, false, false);
	Expect(!handoff.Active(), "new player line clears prior handoff");
	handoff.Update(14, false, false, true);
	Expect(!handoff.Active(), "cancelled or unanswered topic must not strand coverage");

	handoff.Update(15, true, false, false);
	handoff.Update(15, false, false, false);
	handoff.Reset(15);
	handoff.Update(15, true, false, false);
	handoff.Update(15, false, false, false);
	Expect(!handoff.Active(), "close/reopen must not inherit the old voice tail");
	handoff.Update(16, false, false, false);
	Expect(!handoff.Active(), "missed start must not invent a completed player line");
	handoff.Update(17, true, false, false);
	handoff.Update(17, false, false, false);
	Expect(handoff.Active(), "fresh line after reentry should still hand off");
	handoff.Update(17, false, false, true);
	Expect(!handoff.Active(), "topic list returning without an answer releases handoff");
	Expect(!handoff.Holding(), "with no hold set there is never a hold");

	// iPlayerVoiceHold: the camera stays on the player for the hold, timed from
	// the end of their line, then hands off.
	SD::Camera::PlayerVoiceHandoff held;
	held.SetDelay(1.0f);
	held.Reset(20);
	held.Update(21, true, false, false, 0.1f);
	for (int frame = 0; frame < 30; ++frame) {
		held.Update(21, true, false, false, 0.1f);
	}
	Expect(!held.Holding() && !held.Active(), "time spent speaking does not count toward the hold");
	held.Update(21, false, false, false, 0.1f);
	Expect(held.Holding() && !held.Active(), "the hold starts when the voice stops");
	for (int frame = 0; frame < 5; ++frame) {
		held.Update(21, false, false, false, 0.1f);
	}
	Expect(held.Holding() && !held.Active(), "the hold covers the silence before the reply");
	held.Update(21, false, true, false, 0.1f);
	Expect(!held.Holding() && held.Active(), "the reply starting ends the hold at once");
	for (int frame = 0; frame < 5; ++frame) {
		held.Update(21, false, true, false, 0.1f);
		Expect(!held.Holding() && held.Active(), "the camera stays on the NPC for the reply");
	}
	held.Update(21, false, false, false, 0.1f);
	Expect(!held.Holding() && !held.Active(), "the reply ending releases the handoff");

	// No silence at all, which is DBReV with no post-line delay: the reply
	// arrives on the frame the voice stops, and the camera goes with it.
	held.Update(25, true, false, false, 0.1f);
	held.Update(25, false, true, false, 0.1f);
	Expect(!held.Holding() && held.Active(), "a reply on the voice's last frame is not held");

	// A long silence still runs the hold out before the reply.
	held.Update(26, true, false, false, 0.1f);
	held.Update(26, false, false, false, 0.1f);
	for (int frame = 0; frame < 12; ++frame) {
		held.Update(26, false, false, false, 0.1f);
	}
	Expect(!held.Holding() && held.Active(), "the handoff arrives when the hold runs out");
	held.Update(26, false, true, false, 0.1f);
	Expect(held.Active(), "a late reply keeps the handoff");
	held.Update(26, false, false, false, 0.1f);
	Expect(!held.Holding() && !held.Active(), "the reply ending releases a handoff that arrived first");

	held.Update(22, true, false, false, 0.1f);
	held.Update(22, false, false, false, 0.1f);
	held.Update(22, false, false, false, 0.0f);
	held.Update(22, false, false, false, -5.0f);
	Expect(held.Holding(), "zero and negative frame time do not run the hold out");
	held.Update(22, false, false, true, 0.1f);
	Expect(!held.Holding() && !held.Active(), "an unanswered topic releases the hold too");

	held.Update(23, true, false, false, 0.1f);
	held.Update(23, false, false, false, 0.1f);
	for (int frame = 0; frame < 4; ++frame) {
		held.Update(23, false, true, false, 0.1f);
		Expect(held.Active(), "a short reply is still covered from its first frame");
	}
	held.Update(23, false, false, false, 0.1f);
	Expect(!held.Holding() && !held.Active(), "a reply shorter than the hold hands back when it ends");

	held.SetDelay(-1.0f);
	held.Update(24, true, false, false, 0.1f);
	held.Update(24, false, false, false, 0.1f);
	Expect(held.Active() && !held.Holding(), "a negative hold behaves as none");
	std::cout << "PlayerVoiceHandoff tests passed\n";
}

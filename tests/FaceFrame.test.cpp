#include "SD/Camera/FaceFrame.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
	using namespace SD::Camera::FaceFrame;

	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "FaceFrame: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	bool Near(float a, float b, float tolerance = 0.01f)
	{
		return std::abs(a - b) <= tolerance;
	}

	// A head whose local Y is the face, Z is up and X is to the side, pitched
	// forward by a_pitch degrees (like bending over a bench).
	std::array<Vec, 3> Head(float a_pitchDegrees)
	{
		const float p = a_pitchDegrees / kRadToDeg;
		return {
			Vec{ 1.0f, 0.0f, 0.0f },
			Vec{ 0.0f, std::cos(p), -std::sin(p) },
			Vec{ 0.0f, std::sin(p), std::cos(p) },
		};
	}
}

int main()
{
	// Upright, the face axis is found from the body's heading.
	{
		const auto axis = ChooseAxis(Head(0.0f), { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
		Expect(axis.index == 1 && axis.sign > 0.0f, "upright face axis is +Y");
	}

	// Bent fifty degrees over a bench. On raw alignment with the body's forward,
	// the neck axis (Z, now tipped toward the bench) would outscore the face;
	// ruling out the axis along the neck keeps the right answer.
	{
		const auto columns = Head(50.0f);
		const Vec  neck = columns[2];
		const auto axis = ChooseAxis(columns, neck, { 0.0f, 1.0f, 0.0f });
		Expect(axis.index == 1 && axis.sign > 0.0f, "a bowed head still faces along +Y");
	}

	// Mirrored rigs pick the negative axis.
	{
		auto columns = Head(0.0f);
		columns[1] = columns[1] * -1.0f;
		const auto axis = ChooseAxis(columns, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
		Expect(axis.index == 1 && axis.sign < 0.0f, "a mirrored rig faces along -Y");
	}

	// A hint that matches nothing is not trusted.
	{
		const auto axis = ChooseAxis(Head(0.0f), { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f });
		Expect(axis.index < 0, "a hint along the excluded neck axis picks nothing");
		const auto none = ChooseAxis(Head(0.0f), { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
		Expect(none.index < 0, "a degenerate neck picks nothing");
	}

	// Follow ignores sway inside the dead band and chases posture outside it.
	{
		const Vec held{};
		const Vec sway = Follow(held, { 3.0f, 0.0f, -2.0f }, 6.0f, 1.6f, 0.016f);
		Expect(Length(sway) == 0.0f, "breathing and sway do not move the frame");

		Vec offset{};
		for (int frame = 0; frame < 120; ++frame) {
			offset = Follow(offset, { 0.0f, 20.0f, -35.0f }, 6.0f, 1.6f, 1.0f / 60.0f);
		}
		const float gap = Length(Vec{ 0.0f, 20.0f, -35.0f } - offset);
		Expect(gap > 5.0f && gap < 12.0f, "a lean is followed to the edge of the dead band within two seconds");

		const Vec still = Follow(offset, { 0.0f, 20.0f, -35.0f }, 6.0f, 1.6f, 0.0f);
		Expect(Length(still - offset) == 0.0f, "no time, no movement");
	}

	// Limit caps a runaway bone.
	{
		const Vec capped = Limit({ 0.0f, 300.0f, -400.0f }, 63.0f);
		Expect(Near(Length(capped), 63.0f), "an offset past the cap is cut to it");
		const Vec kept = Limit({ 0.0f, 3.0f, -4.0f }, 63.0f);
		Expect(Near(kept.y, 3.0f) && Near(kept.z, -4.0f), "a small offset is untouched");
	}

	// Turn stays a unit vector and moves toward the target.
	{
		Vec facing{ 0.0f, 1.0f, 0.0f };
		facing = Turn(facing, { 1.0f, 0.0f, 0.0f }, 2.0f, 0.1f);
		Expect(Near(Length(facing), 1.0f), "the facing stays a direction");
		Expect(facing.x > 0.0f && facing.y > 0.0f, "the facing turns toward the target");
	}

	// The bias: nothing for a face looking along the eyeline, a capped turn for
	// one looking away, and the camera drops for a face turned down.
	{
		const Bias level = BiasFor({ 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
		Expect(Near(level.yaw, 0.0f) && Near(level.pitch, 0.0f), "a face along the eyeline changes nothing");

		const Bias left = BiasFor({ 0.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f });
		Expect(Near(left.yaw, 45.0f * kYawWeight, 0.1f), "a face turned 45 degrees CCW turns the camera half of it CCW");

		const Bias away = BiasFor({ 0.0f, 1.0f, 0.0f }, { 1.0f, -0.2f, 0.0f });
		Expect(Near(away.yaw, -kMaxYaw), "a face turned far away is capped");

		const float down = 40.0f / kRadToDeg;
		const Bias bowed = BiasFor({ 0.0f, 1.0f, 0.0f }, { 0.0f, std::cos(down), -std::sin(down) });
		Expect(Near(bowed.pitch, -40.0f * kPitchWeight, 0.1f) && Near(bowed.yaw, 0.0f, 0.1f),
			"a face bowed 40 degrees brings the camera down");

		const Bias floor = BiasFor({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.05f, -1.0f });
		Expect(Near(floor.pitch, kMinPitch) && Near(floor.yaw, 0.0f), "a face pointed at the floor keeps the eyeline bearing");

		const Bias up = BiasFor({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.5f, 0.86f });
		Expect(Near(up.pitch, kMaxPitch), "a raised face is capped low");
	}

	std::cout << "FaceFrame tests passed\n";
}

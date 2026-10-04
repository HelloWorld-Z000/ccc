#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace SD::Camera::FaceFrame
{
	// Face tracking for close-ups.
	//
	// Shots hang off a stable point (the root plus a head offset measured once),
	// because the head bone breathes, sways and gestures. That's wrong for
	// close-ups of someone bent over an anvil, whose face is lower, further
	// forward and turned down. So close-range singles add two filtered values from
	// the head bone:
	//
	//   - an offset from the stable point to the head, eased through a dead band
	//     so idle sway is ignored and a posture change is followed;
	//   - a facing, turned into a bearing and height for the camera at the cut
	//     and then held for the shot.
	//
	// Plain arithmetic, no game types, so it can be unit tested.

	struct Vec
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float z{ 0.0f };
	};

	[[nodiscard]] inline Vec operator+(const Vec& a, const Vec& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	[[nodiscard]] inline Vec operator-(const Vec& a, const Vec& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	[[nodiscard]] inline Vec operator*(const Vec& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	[[nodiscard]] inline float Dot(const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	[[nodiscard]] inline float Length(const Vec& a) { return std::sqrt(Dot(a, a)); }

	[[nodiscard]] inline bool Normalize(Vec& a)
	{
		const float length = Length(a);
		if (!std::isfinite(length) || length < 1.0e-4f) {
			return false;
		}
		a = a * (1.0f / length);
		return true;
	}

	inline constexpr float kRadToDeg = 57.29578f;

	// Which of the head bone's local axes points out of the face. Rigs differ and
	// the engine names no "forward", so it's chosen once per subject: the axis
	// most parallel to the neck is ruled out as "up", and of the remaining four
	// signed axes the one best aligned with a hint wins. The hint is the direction
	// to the head's magic node when the rig has one (it sits in front of the
	// mouth), otherwise the body's forward blended with the direction to the other
	// person. Ruling out "up" first matters: with the head pitched over a
	// workbench the neck axis can otherwise outscore the face.
	struct Axis
	{
		int   index{ -1 };    // -1: no axis was clear enough to trust
		float sign{ 1.0f };
		float score{ 0.0f };  // dot with the hint, for the log
		float runnerUp{ 0.0f };
	};

	inline constexpr float kMinAxisScore = 0.30f;
	inline constexpr float kMinAxisMargin = 0.15f;

	[[nodiscard]] inline Axis ChooseAxis(const std::array<Vec, 3>& a_columns, Vec a_up, Vec a_hint)
	{
		Axis out{};
		if (!Normalize(a_up) || !Normalize(a_hint)) {
			return out;
		}

		int   upIndex = -1;
		float upAlign = -1.0f;
		for (int i = 0; i < 3; ++i) {
			const float align = std::abs(Dot(a_columns[i], a_up));
			if (align > upAlign) {
				upAlign = align;
				upIndex = i;
			}
		}

		float best = -2.0f;
		float second = -2.0f;
		for (int i = 0; i < 3; ++i) {
			if (i == upIndex) {
				continue;
			}
			for (const float sign : { 1.0f, -1.0f }) {
				const float score = Dot(a_columns[i] * sign, a_hint);
				if (score > best) {
					second = best;
					best = score;
					out.index = i;
					out.sign = sign;
				} else if (score > second) {
					second = score;
				}
			}
		}

		out.score = best;
		out.runnerUp = second;
		if (best < kMinAxisScore || best - second < kMinAxisMargin) {
			out.index = -1;
		}
		return out;
	}

	// Ease a held offset toward a live one, ignoring motion inside a dead band:
	// only the part beyond the band is followed.
	[[nodiscard]] inline Vec Follow(const Vec& a_held, const Vec& a_live,
		float a_deadband, float a_rate, float a_delta)
	{
		const Vec   difference = a_live - a_held;
		const float distance = Length(difference);
		if (!std::isfinite(distance) || distance <= a_deadband || a_delta <= 0.0f) {
			return a_held;
		}
		const float k = 1.0f - std::exp(-a_rate * a_delta);
		const float excess = distance - a_deadband;
		return a_held + difference * (excess / distance * k);
	}

	// Ease a unit facing toward a live one, renormalised.
	[[nodiscard]] inline Vec Turn(const Vec& a_held, const Vec& a_live, float a_rate, float a_delta)
	{
		if (a_delta <= 0.0f) {
			return a_held;
		}
		const float k = 1.0f - std::exp(-a_rate * a_delta);
		Vec blended = a_held + (a_live - a_held) * k;
		return Normalize(blended) ? blended : a_held;
	}

	// Never further than this from the stable point; beyond it the bone is in an
	// animation the framing shouldn't follow (ragdoll, kill move, sitting).
	[[nodiscard]] inline Vec Limit(const Vec& a_offset, float a_maxLength)
	{
		const float length = Length(a_offset);
		if (!std::isfinite(length)) {
			return {};
		}
		return length > a_maxLength && length > 0.0f ? a_offset * (a_maxLength / length) : a_offset;
	}

	// What the camera does with a facing, decided at the cut.
	//
	// yaw: degrees about +Z from the eyeline toward where the face points
	// (counter-clockwise positive), scaled and capped, so a face turned toward its
	// work is seen from the front.
	//
	// pitch: degrees, negative when the face is turned down. The camera drops by
	// the corresponding height to look up into a lowered face. Capped harder
	// upward than downward.
	struct Bias
	{
		float yaw{ 0.0f };
		float pitch{ 0.0f };
	};

	inline constexpr float kYawWeight = 0.5f;
	inline constexpr float kMaxYaw = 35.0f;
	inline constexpr float kPitchWeight = 0.6f;
	inline constexpr float kMinPitch = -30.0f;
	inline constexpr float kMaxPitch = 10.0f;

	[[nodiscard]] inline Bias BiasFor(const Vec& a_eyeline, const Vec& a_facing)
	{
		Bias out{};
		Vec  eyeline{ a_eyeline.x, a_eyeline.y, 0.0f };
		Vec  facing{ a_facing.x, a_facing.y, 0.0f };
		const float facingZ = std::clamp(a_facing.z, -1.0f, 1.0f);

		// A face pointed nearly straight down has no useful bearing; keep the eyeline
		// and use only the pitch.
		if (Normalize(eyeline) && Length(facing) > 0.25f && Normalize(facing)) {
			const float cross = eyeline.x * facing.y - eyeline.y * facing.x;
			const float yaw = std::atan2(cross, Dot(eyeline, facing)) * kRadToDeg;
			out.yaw = std::clamp(yaw * kYawWeight, -kMaxYaw, kMaxYaw);
		}

		const float pitch = std::asin(facingZ) * kRadToDeg;
		out.pitch = std::clamp(pitch * kPitchWeight, kMinPitch, kMaxPitch);
		return out;
	}
}

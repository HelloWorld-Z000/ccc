#pragma once

namespace SD::Camera
{
	// What kind of body is being framed, as far as composition cares: how big, how
	// high the head is, and whether there's a shoulder to shoot over.
	enum class Build : std::uint8_t
	{
		kHumanoid,  // the assumption every constant in this mod was written under
		kBeast,     // quadrupeds and the like: a head, no usable shoulder
		kLarge,     // dragons, giants, mammoths
		kSmall      // chickens, rabbits, skeevers, mudcrabs
	};

	[[nodiscard]] std::string_view Name(Build a_build) noexcept;

	// Per-build framing adjustments on top of the shot. Scaling gets the camera to
	// the right distance; these get it to the right place.
	struct BuildTuning
	{
		// The tightest fill allowed for this build. 0.85 of a person is a face cut
		// below the chin; 0.85 of a dragon's head is an eye socket, so large heads are
		// shown whole.
		float fillCap;

		// How much of the scaled rise to apply. A 62-unit overhead on a person would
		// become hundreds of units on a dragon, through most ceilings, and large
		// builds read better from at or below the head anyway.
		float riseScale;

		// Degrees added to the angle off the eyeline. A long snout shot head-on is
		// mostly nostril; stepping round shows the length of the head.
		float angleBias;
	};

	[[nodiscard]] BuildTuning Tuning(Build a_build) noexcept;

	// The measurements the shot table assumes, taken per subject. The table is
	// written for a standing human (42-unit head and shoulders, eye at 120, a
	// 68-unit floor, a probe starting 48 units out). On a dragon every one of
	// those is wrong in the same direction, most visibly the probe, which starts
	// inside the animal and finds no room anywhere.
	struct Anatomy
	{
		// Above the actor's root. Sampled from the head node once and then held; the
		// root doesn't animate, the head does.
		float eyeHeight{ 120.0f };

		// Where the head is relative to the root, in the actor's own frame (rotated by
		// heading each frame). For upright bodies the head is above the feet, but a
		// dragon's root is at the hips with the head far in front. Captured once, so
		// the anchor still doesn't follow the head bone's own motion.
		RE::NiPoint3 headOffset{};

		// What a fill fraction is measured against: about a human's head and
		// shoulders, scaled.
		float extent{ 42.0f };

		// The hard floor on standoff, scaled. Never violated.
		float minDistance{ 68.0f };

		// Where a clearance ray starts, measured out from the subject. Must clear the
		// subject's own collision or the ray hits them immediately.
		float probeStart{ 48.0f };

		// The framing scale relative to a standing human (1.0). Not the bulk ratio:
		// head size grows much slower than body size (a dragon's bound is about nine
		// times a person's, its head isn't), so this is bulk raised to a fractional
		// power. See kFrameExponent.
		float scale{ 1.0f };

		// Always zero in practice: the head node is a bone with no geometry (the head
		// mesh is skinned to the body), so its bound is empty. Logged only.
		float headRadius{ 0.0f };

		// The whole-body bound, which is the signal that works (people roughly 70-106,
		// a dragon about 680).
		float radius{ 0.0f };

		// Body-bound ratio, deadbanded so normal human variation reads as 1.0. Used
		// for classification only.
		float bulk{ 1.0f };

		Build build{ Build::kHumanoid };

		// Whether there's a shoulder to shoot over. Set by Fit, since it depends on
		// size as well as the rig (a giant has arms but is still the wrong thing to
		// stand behind).
		bool shoulder{ true };

		// Raw findings from Measure, which Fit reads.
		bool rigged{ true };  // the skeleton has an upper-arm node
		bool head{ true };    // a head node was found at all

		// False when the actor had no 3D. Everything is then the humanoid default.
		bool measured{ false };

		// For the log: the node name the head was found under, and whose body this is.
		std::string_view via{ "default"sv };
		std::string_view name{ "?"sv };
	};

	// Measures an actor: eye height, bound radius, whether the rig has an arm.
	// Walks the skeleton, so it's only called at conversation open and on posture
	// changes. The derived values are filled in by Fit.
	[[nodiscard]] Anatomy Measure(RE::Actor* a_actor);

	// Turns a raw measurement into framing values against a reference radius.
	// Separate from Measure so the player can be the reference: fitted against
	// their own radius they come out at exactly 1.0, and the NPC is sized relative
	// to them. A reference of 0 uses the built-in estimate (only when the player
	// has no 3D).
	void Fit(Anatomy& a_body, float a_referenceRadius);
}

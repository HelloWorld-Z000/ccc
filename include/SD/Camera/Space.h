#pragma once

#include "SD/Camera/SightGeometry.h"

#include <vector>

namespace SD::Camera
{
	struct SubjectSight
	{
		SightState state{ SightState::kUnknown };
		float face{ 0.0f };
		float torso{ 0.0f };
		bool severe{ false };  // no face samples readable, or center outside the frame
	};

	struct SightTarget
	{
		RE::NiPoint3 head{};
		float scale{ 1.0f };
		RE::FormID id{ 0 };
		bool valid{ false };
	};

	struct SightCapsule
	{
		RE::NiPoint3 from{};
		RE::NiPoint3 to{};
		float radius{ 0.0f };
	};

	struct SightActor
	{
		RE::FormID id{ 0 };
		std::array<SightCapsule, 2> capsules{};
		std::size_t count{ 0 };
		RE::NiPoint3 boundCenter{};
		float boundRadius{ 0.0f };
	};

	// A short-lived main-thread snapshot. Build once per candidate search or
	// monitor update; don't keep it across frames or cell changes.
	struct SightContext
	{
		RE::bhkWorld* world{ nullptr };
		std::vector<SightActor> actors;
		bool valid{ false };
	};

	[[nodiscard]] SightContext BuildSightContext();
	[[nodiscard]] SightTarget MeasureSightTarget(RE::Actor* a_actor,
		const RE::NiPoint3& a_fallbackHead, float a_scale);

	// Five face rays converge at the actual lens. Four clear samples including the
	// center admit a shot; two upper-chest samples only affect preference. lens is
	// horizontal FOV in degrees; crop is the fraction removed per edge.
	[[nodiscard]] SubjectSight SubjectVisibility(const RE::NiPoint3& a_camera,
		const RE::NiPoint3& a_lookAt, float a_lens, float a_aspect, float a_crop,
		const SightTarget& a_target, const SightContext& a_context);

	// A small local volume around the lens, including actor bodies. Unlike the
	// legacy placement bundle, foreground away from the lens isn't tested.
	[[nodiscard]] SightState LensClearance(const RE::NiPoint3& a_camera,
		const SightContext& a_context);

	struct Probe
	{
		bool  valid{ false };     // false when no physics world was reachable
		bool  hit{ false };
		float distance{ 0.0f };   // world units to the hit, or the full ray length
	};

	// Casts a sightline ray between two world points.
	[[nodiscard]] Probe Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);

	// Is the view between these two points clear? With No Camera Collision (common
	// in load orders) nothing else stops a shot being placed inside a wall, so
	// occlusion is checked here.
	//
	// Cast from the subject toward the camera, never the other way: a ray that
	// starts inside a wall reports no hit.
	[[nodiscard]] bool Clear(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);

	// How much open space extends from a point along a direction, up to a_max.
	[[nodiscard]] float Clearance(const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction, float a_max);

	// Three rays across the width of the shot rather than one down the middle; a
	// single ray flips fully on and off for a railing or a passing weapon. The
	// bundle is a truncated cone: wide at the subject (roughly their silhouette)
	// and narrow at the camera (the camera's own size), so it answers both "is the
	// foreground clear" and "is there room to stand". Horizontal only: most
	// intrusions (pillars, railings, door frames, people) are horizontal, and the
	// ceiling clamp covers the vertical.
	struct Bundle
	{
		bool  valid{ false };
		float distance{ 0.0f };  // to the NEAREST hit across the bundle, or the full length
		float clear{ 1.0f };     // 0..1, the fraction of rays that reached the far end
	};

	[[nodiscard]] Bundle CastBundle(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		float a_spreadNear, float a_spreadFar);

	// How much of this sightline other people are standing in; 0 is nobody. The
	// Havok probes ignore actors (a ray from someone's head would hit that head),
	// so bystanders are checked geometrically: horizontal distance from the
	// segment against each actor's bound. Cheap enough per candidate. Returns a
	// penalty rather than a veto, since a body clipping the edge of frame is
	// sometimes the better shot.
	[[nodiscard]] float Crowding(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		RE::FormID a_ignoreA, RE::FormID a_ignoreB, float a_radius);
}

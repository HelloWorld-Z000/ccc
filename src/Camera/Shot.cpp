#include "SD/Camera/Shot.h"
#include "SD/Camera/FaceFrame.h"
#include "SD/Camera/ShotAngles.h"

#include "SD/Camera/Space.h"

namespace SD::Camera
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDeg = kPi / 180.0f;

		// How far the camera stops short of whatever is behind it. Kept small so
		// ordinary interiors still have room for composed shots.
		constexpr float kWallMargin = 14.0f;

		// Camera radius, for the far end of a probe bundle. The margin above is axial
		// clearance; this is lateral.
		constexpr float kCameraRadius = 18.0f;

		// Continuity limits depend on the camera's anchor. ShotAngles intersects the
		// shot's own adjustment window with these.
		constexpr float kLineFloorSubject = 8.0f;
		constexpr float kLineFloorMidpoint = 34.0f;

		// Lens palette, in degrees of horizontal field of view. The lens is the main
		// difference between two shots of the same person at the same size: 40 degrees
		// compresses and isolates a face, 95 puts half the room behind it. Skyrim's
		// default is around 75-90, so anything under 60 reads as a camera.
		constexpr float kLensTele = 40.0f;      // across the room; surveillance
		constexpr float kLensLong = 50.0f;      // portrait glass; compresses
		constexpr float kLensPortrait = 60.0f;  // flattering but not obviously long
		constexpr float kLensNormal = 72.0f;    // close to what the player already has
		constexpr float kLensWide = 88.0f;      // involved, a little distended
		constexpr float kLensVeryWide = 100.0f; // the room swallows the people

		struct ShotSpec
		{
			bool  onNpc;         // who the shot is about
			float fill;          // fraction of frame HEIGHT the subject should occupy
			float angleDeg;      // degrees off the eyeline; larger steps further to the side
			float rise;          // world units above the subject's eye level
			bool  overShoulder;  // stand behind the other person, putting them in the corner

			Anchor anchor;
			Aim    aim;
			Move   move;

			// Horizontal field of view for this setup. Distance is solved for the fill at
			// this lens, so the subject takes the same share of frame either way; the lens
			// changes how it looks, not how big it is.
			float lens;

			// Units depend on the move: a fraction of the standoff for push/pull, a
			// fraction of the lens for zoom, world units for crane and tilt, degrees for
			// drift.
			float moveAmount;

			// Where the subject sits in frame, as a fraction of the half-frame (0.33 is a
			// third of the way out from center). Positive headroom puts them above center,
			// where a face belongs; positive lookRoom leaves space in the direction
			// they're looking.
			float headroom;
			float lookRoom;
		};

		// The shot table. The lens and move columns are what make one setup look
		// different from another.
		[[nodiscard]] constexpr ShotSpec SpecFor(ShotType a_type)
		{
			using A = Anchor;
			using M = Move;

			switch (a_type) {
			// ---- NPC coverage -------------------------------------------------
			//                                     onNpc  fill   angle   rise   OTS   anchor      aim         move          lens            amt    head   look
			case ShotType::kOverPlayerShoulder: return { true,  0.42f, 20.0f,   6.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.12f, 0.22f };
			case ShotType::kCloseUp:            return { true,  0.68f, 26.0f,   2.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.16f, 0.14f };
			case ShotType::kMediumNpc:          return { true,  0.40f, 34.0f,   4.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   8.0f,  0.14f, 0.16f };
			case ShotType::kLongNpc:            return { true,  0.16f, 42.0f,  16.0f, false, A::kSubject, Aim::kSubject, M::kCraneUp,  kLensWide,     34.0f, -0.10f, 0.12f };
			case ShotType::kLowAngle:           return { true,  0.55f, 30.0f, -26.0f, false, A::kSubject, Aim::kSubject, M::kTiltUp,   kLensWide,     18.0f, 0.10f, 0.14f };
			case ShotType::kCloseProfile:       return { true,  0.62f, 72.0f,   1.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.14f, 0.20f };
			case ShotType::kCloseLow:           return { true,  0.66f, 44.0f, -18.0f, false, A::kSubject, Aim::kSubject, M::kPushIn,   kLensPortrait, 0.14f, 0.12f, 0.14f };
			case ShotType::kCloseHigh:          return { true,  0.60f, 50.0f,  26.0f, false, A::kSubject, Aim::kSubject, M::kTiltDown, kLensNormal,   14.0f, 0.10f, 0.14f };

			// A loose single on a wide lens. Same person and side as the close-up above,
			// but a very different image.
			case ShotType::kCloseWide:          return { true,  0.30f, 58.0f,   8.0f, false, A::kSubject, Aim::kSubject, M::kPushIn,   kLensWide,     0.12f, 0.08f, 0.18f };

			// The tightest setup: 0.85 fill crops below the chin. On the longest lens in
			// the table, because a wide lens would have to get closer than the 68-unit
			// floor to fill the frame and would end up looser than the plain close-up. At
			// 40 degrees the same fill solves to about 121 units. Locked off: the shot is
			// already at the end of its travel.
			case ShotType::kExtremeClose:       return { true,  0.85f, 22.0f,   1.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensTele,     0.00f, 0.10f, 0.08f };

			// Dirty singles. overShoulder keeps the listener in the corner of frame; the
			// tight fill separates these from the wide OTS.
			case ShotType::kDirtyNpc:           return { true,  0.58f, 15.0f,   3.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.14f, 0.24f };
			case ShotType::kThreeQuarterNpc:    return { true,  0.46f, 40.0f,   5.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   10.0f, 0.14f, 0.18f };
			case ShotType::kMediumProfile:      return { true,  0.34f, 66.0f,   4.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.12f, 0.22f };
			case ShotType::kLowProfile:         return { true,  0.50f, 78.0f, -22.0f, false, A::kSubject, Aim::kSubject, M::kTiltUp,   kLensWide,     16.0f, 0.08f, 0.20f };
			case ShotType::kOverhead:           return { true,  0.28f, 40.0f,  62.0f, false, A::kSubject, Aim::kSubject, M::kCraneDown, kLensWide,    26.0f, 0.00f, 0.10f };

			// Over-the-shoulder shots stand further back than singles (past the other
			// person), so the same rise gives a shallower angle. Hence -26 and 34 here
			// versus +-20 on kCloseLow.
			case ShotType::kOverPlayerShoulderLow:  return { true,  0.46f, 22.0f, -26.0f, true, A::kSubject, Aim::kSubject, M::kPushIn,   kLensPortrait, 0.12f, 0.10f, 0.22f };
			case ShotType::kOverPlayerShoulderHigh: return { true,  0.40f, 24.0f,  34.0f, true, A::kSubject, Aim::kSubject, M::kTiltDown, kLensNormal,   16.0f, 0.10f, 0.22f };
			case ShotType::kOverPlayerShoulderWide: return { true,  0.24f, 28.0f,  12.0f, true, A::kSubject, Aim::kSubject, M::kPullOut,  kLensWide,     0.14f, 0.02f, 0.18f };

			// ---- Player coverage ---------------------------------------------------
			case ShotType::kOverNpcShoulder:    return { false, 0.40f, 20.0f,   6.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.12f, 0.22f };
			case ShotType::kMediumPlayer:       return { false, 0.38f, 34.0f,   4.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   8.0f,  0.14f, 0.16f };
			case ShotType::kHighAngle:          return { false, 0.34f, 30.0f,  38.0f, false, A::kSubject, Aim::kSubject, M::kCraneDown, kLensNormal,  24.0f, 0.06f, 0.14f };
			case ShotType::kClosePlayer:        return { false, 0.60f, 26.0f,   2.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.16f, 0.14f };

			// Mirrors kExtremeClose, including the long lens (see there).
			case ShotType::kExtremeClosePlayer: return { false, 0.85f, 22.0f,   1.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensTele,     0.00f, 0.10f, 0.08f };
			case ShotType::kDirtyPlayer:        return { false, 0.55f, 15.0f,   3.0f, true,  A::kSubject, Aim::kSubject, M::kLocked,   kLensPortrait, 0.00f, 0.14f, 0.24f };
			case ShotType::kPlayerProfile:      return { false, 0.40f, 70.0f,   2.0f, false, A::kSubject, Aim::kSubject, M::kLocked,   kLensLong,     0.00f, 0.12f, 0.22f };
			case ShotType::kPlayerLow:          return { false, 0.52f, 32.0f, -20.0f, false, A::kSubject, Aim::kSubject, M::kTiltUp,   kLensWide,     16.0f, 0.10f, 0.14f };
			case ShotType::kThreeQuarterPlayer: return { false, 0.44f, 40.0f,   5.0f, false, A::kSubject, Aim::kSubject, M::kDrift,    kLensNormal,   10.0f, 0.14f, 0.18f };

			// Matched to their NPC-side counterparts (size, height, lens, move), so a
			// reverse reads as the other half of the same conversation.
			case ShotType::kOverNpcShoulderLow:  return { false, 0.44f, 22.0f, -26.0f, true, A::kSubject, Aim::kSubject, M::kPushIn,   kLensPortrait, 0.12f, 0.10f, 0.22f };
			case ShotType::kOverNpcShoulderHigh: return { false, 0.38f, 24.0f,  34.0f, true, A::kSubject, Aim::kSubject, M::kTiltDown, kLensNormal,   16.0f, 0.10f, 0.22f };
			case ShotType::kOverNpcShoulderWide: return { false, 0.24f, 28.0f,  12.0f, true, A::kSubject, Aim::kSubject, M::kPullOut,  kLensWide,     0.14f, 0.02f, 0.18f };
			case ShotType::kLongPlayer:          return { false, 0.16f, 42.0f,  16.0f, false, A::kSubject, Aim::kSubject, M::kCraneUp,  kLensWide,     34.0f, -0.10f, 0.12f };
			case ShotType::kPlayerOverhead:      return { false, 0.28f, 40.0f,  62.0f, false, A::kSubject, Aim::kSubject, M::kCraneDown, kLensWide,   26.0f, 0.00f, 0.10f };

			// ---- Neutral: the pair -------------------------------------------------
			//
			// Anchored to the midpoint between the two, so framing both and the angle can
			// be set independently.
			case ShotType::kTwoShot:            return { true,  0.20f, 90.0f,   8.0f, false, A::kMidpoint, Aim::kMidpoint, M::kDrift,   kLensNormal,   7.0f,  0.06f, 0.00f };
			case ShotType::kProfile:            return { true,  0.30f, 90.0f,   2.0f, false, A::kMidpoint, Aim::kMidpoint, M::kLocked,  kLensLong,     0.00f, 0.08f, 0.00f };

			// ---- Neutral: the room -------------------------------------------------
			//
			// Anchored to the scene: these stand off in the direction the room opens up
			// and let the two people fall where they fall. Negative headroom puts them in
			// the lower third with space above.
			//
			// angleDeg here is relative to the open direction, not the eyeline (see the
			// scene branch in Solve), so the room shots look at the conversation from
			// different corners.
			case ShotType::kWide:               return { true,  0.11f,   0.0f,  24.0f, false, A::kScene, Aim::kScene, M::kPullOut,  kLensWide,     0.14f, -0.24f, 0.10f };
			case ShotType::kMaster:             return { true,  0.075f, 24.0f,  76.0f, false, A::kScene, Aim::kScene, M::kCraneUp,  kLensVeryWide, 46.0f, -0.30f, 0.14f };
			case ShotType::kGroundLevel:        return { true,  0.24f,  -20.0f, -46.0f, false, A::kScene, Aim::kMidpoint, M::kTiltUp, kLensVeryWide, 24.0f, -0.12f, 0.00f };

			// The long lens across the room. Compared with kMaster, which opens the space
			// up, this one flattens it and reads as watching from a distance.
			case ShotType::kDistant:            return { true,  0.09f,   0.0f,  28.0f, false, A::kScene, Aim::kMidpoint, M::kLocked, kLensTele,     0.00f, -0.08f, 0.00f };
			case ShotType::kDistantLow:         return { true,  0.10f,  32.0f, -24.0f, false, A::kScene, Aim::kMidpoint, M::kDrift,  kLensLong,     6.0f,  -0.06f, 0.00f };

			default:                            return { true,  0.42f, 20.0f,   6.0f, true,  A::kSubject, Aim::kSubject, M::kLocked, kLensPortrait, 0.00f, 0.12f, 0.18f };
			}
		}

		[[nodiscard]] float Length(const RE::NiPoint3& a_v)
		{
			return std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z);
		}

		[[nodiscard]] RE::NiPoint3 Normalized(const RE::NiPoint3& a_v, bool& a_ok)
		{
			const float length = Length(a_v);
			a_ok = length > 1.0e-3f;
			return a_ok ? RE::NiPoint3{ a_v.x / length, a_v.y / length, a_v.z / length } : RE::NiPoint3{};
		}

		[[nodiscard]] RE::NiPoint3 Cross(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		}

		[[nodiscard]] RE::NiPoint3 RotateAboutZ(const RE::NiPoint3& a_v, float a_radians)
		{
			const float c = std::cos(a_radians);
			const float s = std::sin(a_radians);
			return { a_v.x * c - a_v.y * s, a_v.x * s + a_v.y * c, a_v.z };
		}

		// The head's share of a subject's extent; the extreme close-ups are sized
		// against it. 0.55 of a person's 42 units is about 23, crown to just under the
		// chin.
		constexpr float kHeadShare = 0.55f;

		// Distance at which a subject of the given extent fills a_fill of the frame
		// height. Skyrim's FOV is horizontal, so the vertical is derived through the
		// aspect ratio. Uses the subject's own size so a close-up on a dragon frames
		// its head, not a nostril.
		[[nodiscard]] float DistanceForFill(float a_fill, float a_fovDegrees, float a_aspect,
			const Anatomy& a_body)
		{
			// Same lens limits Solve clamps to. If these differed, the standoff would be
			// solved for one FOV and rendered at another.
			const float horizontal = std::clamp(a_fovDegrees,
									 static_cast<float>(kMinLens),
									 static_cast<float>(kMaxLens)) * kDeg;
			const float aspect = std::clamp(a_aspect, 1.0f, 3.0f);
			const float vertical = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);

			const float fill = std::clamp(a_fill, 0.04f, 0.95f);
			const float halfAngle = vertical * fill * 0.5f;
			const float t = std::tan(halfAngle);
			if (!(t > 1.0e-4f)) {
				return 200.0f;
			}
			// The ceiling is well past anything an interior can use, and scales with the
			// subject: long lenses on small fills legitimately want over a thousand units,
			// more for a dragon.
			return std::clamp((a_body.extent * 0.5f) / t,
				a_body.minDistance, 2400.0f * std::max(a_body.scale, 1.0f));
		}

		[[nodiscard]] float Ease(float a_t)
		{
			const float t = std::clamp(a_t, 0.0f, 1.0f);
			return 1.0f - (1.0f - t) * (1.0f - t);
		}

		// Which setups the picker may draw, and how often each is drawn relative to
		// the others (0-100). Kept separate from `enabled` so switching a setup off
		// and on again keeps its weight. Weight 0 and disabled behave the same at
		// every draw site. Per-shot defaults come from AuthoredWeight.
		struct SelectionSettings
		{
			std::array<std::atomic<bool>, static_cast<std::size_t>(ShotType::kCount)> enabled{};
			std::array<std::atomic<std::uint8_t>, static_cast<std::size_t>(ShotType::kCount)> weights{};

			SelectionSettings()
			{
				for (std::size_t i = 0; i < enabled.size(); ++i) {
					enabled[i].store(true, std::memory_order_relaxed);
					weights[i].store(static_cast<std::uint8_t>(AuthoredWeight(static_cast<ShotType>(i))),
						std::memory_order_relaxed);
				}
			}
		};
		// Read by the settings panel and the camera hooks concurrently.
		SelectionSettings selectionSettings{};

		// The live field of view for each setup, in degrees, seeded from the table so
		// the slider shows the lens actually in use.
		std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> lensDegrees = [] {
			std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i] = static_cast<std::uint8_t>(SpecFor(static_cast<ShotType>(i)).lens);
			}
			return out;
		}();

		// What 100 on the amount slider means for each move. The table's amounts use
		// the move's own units, so the slider is a percentage of each move's full
		// travel instead; 60 means the same amount of movement whatever the move, and
		// switching moves keeps the intensity. World-unit entries are scaled by the
		// subject's size at solve time.
		[[nodiscard]] constexpr float FullScale(Move a_move)
		{
			switch (a_move) {
			case Move::kPushIn:
			case Move::kPullOut:    return 0.40f;   // fraction of the standoff
			case Move::kZoomIn:
			case Move::kZoomOut:    return 0.40f;   // fraction of the lens
			case Move::kCraneUp:
			case Move::kCraneDown:
			case Move::kTiltUp:
			case Move::kTiltDown:   return 120.0f;  // world units
			case Move::kTruckLeft:
			case Move::kTruckRight: return 120.0f;  // world units
			case Move::kDrift:
			case Move::kOrbitLeft:
			case Move::kOrbitRight: return 45.0f;   // degrees of arc
			case Move::kLocked:
			default:                return 0.0f;
			}
		}

		// The table's own amount on the 0-100 scale. What a fresh install starts at
		// and what the per-shot Default button restores.
		[[nodiscard]] constexpr int AuthoredStrength(ShotType a_type)
		{
			const auto  spec = SpecFor(a_type);
			const float full = FullScale(spec.move);
			if (!(full > 0.0f)) {
				// A locked setup has no amount to convert; give the slider a sensible value
				// for when someone switches it to a move.
				return 35;
			}
			const float pct = (spec.moveAmount / full) * 100.0f;
			return static_cast<int>(pct < 0.0f ? 0.0f : (pct > 100.0f ? 100.0f : pct));
		}

		// Default move duration, in hundredths of a second.
		constexpr int kDefaultMoveTime = 420;

		std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> moveChoice = [] {
			std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i] = static_cast<std::uint8_t>(SpecFor(static_cast<ShotType>(i)).move);
			}
			return out;
		}();

		std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> moveStrength = [] {
			std::array<std::uint8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			for (std::size_t i = 0; i < out.size(); ++i) {
				out[i] = static_cast<std::uint8_t>(AuthoredStrength(static_cast<ShotType>(i)));
			}
			return out;
		}();

		std::array<std::uint16_t, static_cast<std::size_t>(ShotType::kCount)> moveTime = [] {
			std::array<std::uint16_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			out.fill(static_cast<std::uint16_t>(kDefaultMoveTime));
			return out;
		}();

		// Lighting rig per setup, as an index into Scene::AllRigs. Stored in the ini
		// as a name so adding a rig doesn't shift every setup after it. -1 means not
		// resolved yet (rig 0 is a real rig, Off), so an unread setup falls back to
		// its shipped rig, not to darkness.
		std::array<std::int8_t, static_cast<std::size_t>(ShotType::kCount)> lightRig = [] {
			std::array<std::int8_t, static_cast<std::size_t>(ShotType::kCount)> out{};
			out.fill(static_cast<std::int8_t>(-1));
			return out;
		}();

		// Per-setup light nudge in camera space. Usually zero.
		std::array<std::int16_t, static_cast<std::size_t>(ShotType::kCount)> lightOffX{};
		std::array<std::int16_t, static_cast<std::size_t>(ShotType::kCount)> lightOffY{};
		std::array<std::int16_t, static_cast<std::size_t>(ShotType::kCount)> lightOffZ{};

		// How much room there is in one direction, measured outward from the subject
		// (a ray that starts inside a wall reports no hit, so an inward test would
		// pass a buried camera). The probe starts outside the subject's own collision,
		// scaled by Anatomy; a fixed human-sized start makes every direction look
		// blocked for a dragon.
		//
		// `clear` is how much of the shot's width was unobstructed, which separates a
		// wide shot with a stall across a third of the frame from a clean one.
		struct Room
		{
			float distance{ 0.0f };
			float clear{ 1.0f };
		};

		[[nodiscard]] Room RoomAlong(const RE::NiPoint3& a_subject, const RE::NiPoint3& a_direction,
			float a_rise, float a_wanted, float a_probeStart, float a_extent)
		{
			if (a_wanted <= a_probeStart) {
				return { a_wanted, 1.0f };
			}

			const RE::NiPoint3 from{
				a_subject.x + a_direction.x * a_probeStart,
				a_subject.y + a_direction.y * a_probeStart,
				a_subject.z + a_direction.z * a_probeStart + a_rise * 0.35f
			};
			const RE::NiPoint3 to{
				a_subject.x + a_direction.x * a_wanted,
				a_subject.y + a_direction.y * a_wanted,
				a_subject.z + a_direction.z * a_wanted + a_rise
			};

			// Wide at the subject, narrow at the camera: the near spread covers the
			// subject's silhouette, the far spread is the camera's own size.
			const float nearSpread = std::max(a_extent * 0.5f, 12.0f);
			const auto  probe = CastBundle(from, to, nearSpread, kCameraRadius);

			if (!probe.valid) {
				return { a_wanted, 1.0f };  // no physics world; do not veto every shot
			}
			if (probe.clear >= 1.0f) {
				return { a_wanted, 1.0f };
			}

			// The margin scales with the subject, like every other world-unit value.
			const float margin = kWallMargin * std::max(a_extent / 42.0f, 1.0f);
			return {
				std::max(a_probeStart + probe.distance - margin, 0.0f),
				probe.clear
			};
		}

		// Placement proposal on the center sightline only, so a side ray through
		// harmless foreground can't shorten a protected shot. This doesn't certify
		// visibility; the face samples and lens volume still decide that.
		[[nodiscard]] Room NarrowRoom(const RE::NiPoint3& a_subject, const RE::NiPoint3& a_direction,
			float a_rise, float a_wanted, float a_probeStart, float a_extent)
		{
			if (a_wanted <= a_probeStart) {
				return { a_wanted, 1.0f };
			}
			const RE::NiPoint3 from{
				a_subject.x + a_direction.x * a_probeStart,
				a_subject.y + a_direction.y * a_probeStart,
				a_subject.z + a_direction.z * a_probeStart + a_rise * (a_probeStart / a_wanted)
			};
			const RE::NiPoint3 to{
				a_subject.x + a_direction.x * a_wanted,
				a_subject.y + a_direction.y * a_wanted,
				a_subject.z + a_direction.z * a_wanted + a_rise
			};
			const auto probe = Cast(from, to);
			if (!probe.valid || !probe.hit) {
				return { a_wanted, 1.0f };
			}
			const float fullLength = Length({ to.x - from.x, to.y - from.y, to.z - from.z });
			const float fraction = fullLength > 0.001f ? std::clamp(probe.distance / fullLength, 0.0f, 1.0f) : 0.0f;
			const float margin = kWallMargin * std::max(a_extent / 42.0f, 1.0f);
			return { std::max(a_probeStart + (a_wanted - a_probeStart) * fraction - margin, 0.0f), 0.0f };
		}

		// The room along a bearing: measured, or remembered from the cut. Every
		// RoomAlong call on a solve path goes through here, so Hold Placement holds
		// every axis at once. The remembered `clear` is 1.0: it only feeds quality,
		// which is only read at a cut, where holdPlacement is always false.
		[[nodiscard]] Room RoomHere(const Subjects& a_subjects, const RE::NiPoint3& a_subject,
			const RE::NiPoint3& a_direction, float a_rise, float a_wanted, float a_probeStart,
			float a_extent)
		{
			if (a_subjects.holdPlacement && a_subjects.heldRoom > kUnheld) {
				return a_subjects.heldRoom >= kOpenRoom ?
					Room{ a_wanted, 1.0f } :
					Room{ a_subjects.heldRoom, 1.0f };
			}
			return RoomAlong(a_subject, a_direction, a_rise, a_wanted, a_probeStart, a_extent);
		}

		// What Pose::room should report. A held frame reports the held value rather
		// than what RoomHere returned this frame, so a pull-out move doesn't turn
		// "nothing in the way" into a cap at wherever the move had reached.
		[[nodiscard]] float RememberRoom(const Subjects& a_subjects, float a_room, float a_clear)
		{
			if (a_subjects.holdPlacement && a_subjects.heldRoom > kUnheld) {
				return a_subjects.heldRoom;
			}
			return a_clear >= 1.0f ? kOpenRoom : a_room;
		}

		// Where the subject lands in frame once the camera is placed. Returns the aim
		// point, which isn't the subject: to put a face on the upper third the camera
		// has to point below it.
		[[nodiscard]] RE::NiPoint3 Compose(const RE::NiPoint3& a_position, const RE::NiPoint3& a_target,
			const RE::NiPoint3& a_facing, float a_lensDeg, float a_aspect, float a_headroom, float a_lookRoom)
		{
			bool       ok = false;
			const auto forward = Normalized(
				{ a_target.x - a_position.x, a_target.y - a_position.y, a_target.z - a_position.z }, ok);
			if (!ok) {
				return a_target;
			}

			const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(forward, worldUp), ok);
			if (!ok) {
				return a_target;  // looking straight down; no meaningful horizon to compose against
			}
			const auto up = Cross(right, forward);

			const float distance = Length(
				RE::NiPoint3{ a_target.x - a_position.x, a_target.y - a_position.y, a_target.z - a_position.z });

			const float horizontal = std::clamp(a_lensDeg, 30.0f, 120.0f) * kDeg;
			const float aspect = std::clamp(a_aspect, 1.0f, 3.0f);
			const float vertical = 2.0f * std::atan(std::tan(horizontal * 0.5f) / aspect);

			const float halfWidth = distance * std::tan(horizontal * 0.5f);
			const float halfHeight = distance * std::tan(vertical * 0.5f);

			// Which way the subject faces in screen terms. Derived rather than passed in,
			// since the camera can be on either side of the eyeline.
			const float facing = right.x * a_facing.x + right.y * a_facing.y + right.z * a_facing.z;
			const float lookSign = facing > 0.0f ? -1.0f : 1.0f;

			const float offsetX = a_lookRoom * lookSign * halfWidth;
			const float offsetY = a_headroom * halfHeight;

			return {
				a_target.x - right.x * offsetX - up.x * offsetY,
				a_target.y - right.y * offsetX - up.y * offsetY,
				a_target.z - right.z * offsetX - up.z * offsetY
			};
		}

		// Placement quality, 0..1, from three kinds of compromise:
		//
		// size   - had to stand closer than the framing asked, so the subject is
		//          bigger than intended. Weighted highest: it changes what the shot is.
		// clear  - the view along the bearing is partly blocked.
		// angle  - had to step away from its own bearing to find room. Limited to
		//          twelve degrees; beyond that another enabled shot should be used.
		//
		// Refused shots never get here, so this only compares shots that work.
		[[nodiscard]] float Score(float a_wanted, float a_used, float a_clear, float a_offset)
		{
			const float size = a_wanted > 1.0f ?
				std::clamp(a_used / a_wanted, 0.0f, 1.0f) : 1.0f;
			const float clear = std::clamp(a_clear, 0.0f, 1.0f);
			const float angle = 1.0f - std::clamp(std::abs(a_offset) / ShotAngles::kMaxAdjustment, 0.0f, 1.0f);

			return std::clamp(0.45f * size + 0.32f * clear + 0.23f * angle, 0.0f, 1.0f);
		}

		// What's actually visible from the finished pose, 0..1. The room checks above
		// reason about a bearing, which covers a plain single, but not:
		//
		//   - a slide, which moves the camera sideways after the bearing was probed;
		//   - an over-the-shoulder, which needs the foreground shoulder visible;
		//   - a two-shot, which needs both people.
		//
		// Rays go from each person toward the camera (outward, for the same reason as
		// RoomAlong). Penalties multiply rather than veto: a shot that lost its
		// foreground body is worse, not impossible.
		[[nodiscard]] float Visibility(const Pose& a_pose, const RE::NiPoint3& a_subject,
			const RE::NiPoint3& a_other, const ShotSpec& a_spec, float a_truck,
			const Subjects& a_subjects)
		{
			// Skipped on held frames, like RoomHere: it's several raycasts and an actor
			// walk that only feed quality, which nothing reads while holding.
			if (a_subjects.holdPlacement) {
				return 1.0f;
			}

			float sight = 1.0f;

			if (a_truck != 0.0f && !Clear(a_subject, a_pose.position)) {
				sight *= 0.35f;
			}

			const bool needsOther = a_spec.overShoulder ||
				a_spec.anchor != Anchor::kSubject || a_spec.aim != Aim::kSubject;
			if (needsOther && !Clear(a_other, a_pose.position)) {
				sight *= a_spec.overShoulder ? 0.5f : 0.6f;
			}

			if (a_subjects.avoidCrowds) {
				// Width of the shot near the lens. A person at arm's length from the camera
				// fills the frame; the same person beside the subject is background.
				constexpr float kCrowdReach = 70.0f;
				const float     crowd = Crowding(a_subject, a_pose.position,
						a_subjects.npcId, a_subjects.playerId, kCrowdReach);
				sight *= 1.0f - 0.75f * crowd;
			}

			return std::clamp(sight, 0.0f, 1.0f);
		}

		// How fast the camera may move back out, in units per second. RoomAlong is a
		// single ray, so a railing or someone walking past flips it on and off; this
		// turns that into a shallow dip instead of a pop. Pulling in is never limited,
		// since nothing else keeps the lens out of walls (this load order runs No
		// Camera Collision). Well above the speed of the authored dolly moves.
		constexpr float kStandoffRecovery = 300.0f;

		// Slides the whole camera sideways without turning it. Position and aim move
		// by the same vector, so the subject drifts across frame; that's the
		// difference from an orbit. Applied after composition because every other move
		// re-aims at the subject.
		void ApplyTruck(Pose& a_pose, float a_units)
		{
			if (a_units == 0.0f) {
				return;
			}

			bool       ok = false;
			const auto forward = Normalized(
				{ a_pose.lookAt.x - a_pose.position.x,
					a_pose.lookAt.y - a_pose.position.y,
					a_pose.lookAt.z - a_pose.position.z },
				ok);
			if (!ok) {
				return;
			}

			const RE::NiPoint3 worldUp{ 0.0f, 0.0f, 1.0f };
			const auto         right = Normalized(Cross(forward, worldUp), ok);
			if (!ok) {
				return;
			}

			a_pose.position.x += right.x * a_units;
			a_pose.position.y += right.y * a_units;
			a_pose.position.z += right.z * a_units;
			a_pose.lookAt.x += right.x * a_units;
			a_pose.lookAt.y += right.y * a_units;
			a_pose.lookAt.z += right.z * a_units;
		}

		// The first frame of a shot is unheld, so a cut lands at its true distance
		// immediately; only the correction is rate-limited.
		[[nodiscard]] float LimitStandoff(const Subjects& a_subjects, float a_wanted)
		{
			if (a_subjects.heldStandoff <= kUnheld || !(a_subjects.delta > 0.0f)) {
				return a_wanted;
			}
			if (a_wanted <= a_subjects.heldStandoff) {
				return a_wanted;  // in, at full speed, always
			}
			return std::min(a_wanted, a_subjects.heldStandoff + kStandoffRecovery * a_subjects.delta);
		}
	}

	// Display names only; Key() below is the settings contract.
	//
	// Names describe what's on screen rather than film terminology ("Head And
	// Shoulders" rather than "Medium", "Both Of You" rather than "Two-Shot").
	// Shoulder shots are named by whose shoulder it is: "Over Your Shoulder" is a
	// shot of them, with the camera behind you.
	//
	// Seven names repeat across the two sides on purpose (Close Up, Extreme Close
	// Up, Head And Shoulders, Three Quarters, From Below, Full Figure, From High
	// Above): they're the same framing from opposite sides of the eyeline. The
	// panel headings and SubjectName() in the log tell them apart.
	std::string_view Name(ShotType a_type) noexcept
	{
		switch (a_type) {
		// Shots of the NPC. For the shoulder shots the camera is behind the player,
		// hence "your".
		case ShotType::kOverPlayerShoulder:     return "Over Your Shoulder"sv;
		case ShotType::kOverPlayerShoulderLow:  return "Over Your Shoulder (Low)"sv;
		case ShotType::kOverPlayerShoulderHigh: return "Over Your Shoulder (High)"sv;
		case ShotType::kOverPlayerShoulderWide: return "Over Your Shoulder (Wide)"sv;
		case ShotType::kDirtyNpc:           return "Over Your Shoulder (Tight)"sv;
		case ShotType::kThreeQuarterNpc:    return "Three Quarters"sv;
		case ShotType::kCloseUp:            return "Close Up"sv;
		case ShotType::kExtremeClose:       return "Extreme Close Up"sv;
		case ShotType::kCloseLow:           return "Close Up (Low)"sv;
		case ShotType::kCloseHigh:          return "Close Up (High)"sv;
		case ShotType::kCloseProfile:       return "Close Up (Side On)"sv;
		case ShotType::kCloseWide:          return "Close Up (Wide)"sv;
		case ShotType::kMediumNpc:          return "Head And Shoulders"sv;
		case ShotType::kMediumProfile:      return "Head And Shoulders (Side On)"sv;
		case ShotType::kLowAngle:           return "From Below"sv;
		case ShotType::kLowProfile:         return "From Below (Side On)"sv;
		case ShotType::kLongNpc:            return "Full Figure"sv;
		case ShotType::kOverhead:           return "From High Above"sv;

		// Shots of the player.
		case ShotType::kOverNpcShoulder:        return "Over Their Shoulder"sv;
		case ShotType::kOverNpcShoulderLow:     return "Over Their Shoulder (Low)"sv;
		case ShotType::kOverNpcShoulderHigh:    return "Over Their Shoulder (High)"sv;
		case ShotType::kOverNpcShoulderWide:    return "Over Their Shoulder (Wide)"sv;
		case ShotType::kDirtyPlayer:        return "Over Their Shoulder (Tight)"sv;
		case ShotType::kThreeQuarterPlayer: return "Three Quarters"sv;
		case ShotType::kClosePlayer:        return "Close Up"sv;
		case ShotType::kExtremeClosePlayer: return "Extreme Close Up"sv;
		case ShotType::kMediumPlayer:       return "Head And Shoulders"sv;
		case ShotType::kPlayerProfile:      return "Side On"sv;
		case ShotType::kPlayerLow:          return "From Below"sv;
		case ShotType::kHighAngle:          return "From Above"sv;
		case ShotType::kLongPlayer:         return "Full Figure"sv;
		case ShotType::kPlayerOverhead:     return "From High Above"sv;

		// Shots of the pair and of the room.
		case ShotType::kTwoShot:            return "Both Of You"sv;
		case ShotType::kProfile:            return "Both Of You (Side On)"sv;
		case ShotType::kWide:               return "Wide"sv;
		case ShotType::kMaster:             return "The Whole Room"sv;
		case ShotType::kGroundLevel:        return "From The Floor"sv;
		case ShotType::kDistant:            return "From Far Off"sv;
		case ShotType::kDistantLow:         return "From Far Off (Low)"sv;
		default:                            return "unknown"sv;
		}
	}

	// Who the shot is of, in one word. For the log, which has no panel heading to
	// tell "Close Up" on one side from "Close Up" on the other.
	std::string_view SubjectName(ShotType a_type) noexcept
	{
		if (IsNeutral(a_type)) {
			return "room"sv;
		}
		return FavoursNpc(a_type) ? "them"sv : "you"sv;
	}

	// Never rename these. They're keys in the player's settings file, and a rename
	// silently re-enables whatever the player turned off.
	const char* Key(ShotType a_type) noexcept
	{
		switch (a_type) {
		case ShotType::kOverPlayerShoulder: return "bOverPlayerShoulder";
		case ShotType::kOverNpcShoulder:    return "bOverNpcShoulder";
		case ShotType::kCloseUp:            return "bCloseUp";
		case ShotType::kMediumNpc:          return "bMediumNpc";
		case ShotType::kMediumPlayer:       return "bMediumPlayer";
		case ShotType::kLongNpc:            return "bLongNpc";
		case ShotType::kCloseProfile:       return "bCloseProfile";
		case ShotType::kCloseLow:           return "bCloseLow";
		case ShotType::kCloseHigh:          return "bCloseHigh";
		case ShotType::kCloseWide:          return "bCloseWide";
		case ShotType::kTwoShot:            return "bTwoShot";
		case ShotType::kProfile:            return "bProfile";
		case ShotType::kLowAngle:           return "bLowAngle";
		case ShotType::kHighAngle:          return "bHighAngle";
		case ShotType::kWide:               return "bWide";
		case ShotType::kDistant:            return "bDistant";
		case ShotType::kExtremeClose:       return "bExtremeClose";
		case ShotType::kExtremeClosePlayer: return "bExtremeClosePlayer";
		case ShotType::kDirtyNpc:           return "bDirtyNpc";
		case ShotType::kThreeQuarterNpc:    return "bThreeQuarterNpc";
		case ShotType::kMediumProfile:      return "bMediumProfile";
		case ShotType::kLowProfile:         return "bLowProfile";
		case ShotType::kOverhead:           return "bOverhead";
		case ShotType::kClosePlayer:        return "bClosePlayer";
		case ShotType::kDirtyPlayer:        return "bDirtyPlayer";
		case ShotType::kPlayerProfile:      return "bPlayerProfile";
		case ShotType::kPlayerLow:          return "bPlayerLow";
		case ShotType::kThreeQuarterPlayer: return "bThreeQuarterPlayer";
		case ShotType::kOverPlayerShoulderLow:  return "bOverPlayerShoulderLow";
		case ShotType::kOverPlayerShoulderHigh: return "bOverPlayerShoulderHigh";
		case ShotType::kOverPlayerShoulderWide: return "bOverPlayerShoulderWide";
		case ShotType::kOverNpcShoulderLow:  return "bOverNpcShoulderLow";
		case ShotType::kOverNpcShoulderHigh: return "bOverNpcShoulderHigh";
		case ShotType::kOverNpcShoulderWide: return "bOverNpcShoulderWide";
		case ShotType::kLongPlayer:         return "bLongPlayer";
		case ShotType::kPlayerOverhead:     return "bPlayerOverhead";
		case ShotType::kMaster:             return "bMaster";
		case ShotType::kGroundLevel:        return "bGroundLevel";
		case ShotType::kDistantLow:         return "bDistantLow";
		default:                            return "bUnknown";
		}
	}

	namespace
	{
		// Derived from Key() so there's only one hand-maintained list of names. The
		// prefix is a parameter because the lighting rig is stored as a string, not an
		// integer.
		const char* TypedKey(ShotType a_type, const char* a_prefix, const char* a_suffix,
			std::array<std::string, static_cast<std::size_t>(ShotType::kCount)>& a_cache)
		{
			if (a_cache[0].empty()) {
				for (std::size_t i = 0; i < a_cache.size(); ++i) {
					const char* base = Key(static_cast<ShotType>(i));
					a_cache[i] = std::string{ a_prefix } +
								 (base && *base ? base + 1 : "Unknown") + a_suffix;
				}
			}

			const auto index = static_cast<std::size_t>(a_type);
			return index < a_cache.size() ? a_cache[index].c_str() : "iUnknown";
		}

		const char* SuffixedKey(ShotType a_type, const char* a_suffix,
			std::array<std::string, static_cast<std::size_t>(ShotType::kCount)>& a_cache)
		{
			return TypedKey(a_type, "i", a_suffix, a_cache);
		}
	}

	const char* WeightKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Weight", cache);
	}

	const char* LensKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Fov", cache);
	}

	const char* MoveKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Move", cache);
	}

	const char* MoveAmountKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "MoveAmount", cache);
	}

	const char* MoveTimeKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "MoveTime", cache);
	}

	// Read once, to migrate. See SetMove.
	const char* ZoomKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "Zoom", cache);
	}

	const char* LightKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return TypedKey(a_type, "s", "Light", cache);
	}

	const char* LightXKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "LightX", cache);
	}

	const char* LightYKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "LightY", cache);
	}

	const char* LightZKey(ShotType a_type) noexcept
	{
		static std::array<std::string, static_cast<std::size_t>(ShotType::kCount)> cache;
		return SuffixedKey(a_type, "LightZ", cache);
	}

	const char* AuthoredLight(ShotType a_type) noexcept
	{
		// Default rig per setup. Two rules: a face-modelling rig is only worth it when
		// the face is big in frame (the wides and masters use Natural or nothing,
		// since a key from across the room just lights a patch of floor), and the rig
		// should suit the angle (Rembrandt on profiles, Hard on low angles and on the
		// extreme close-ups, which only appear on intensity-100 lines). All of these
		// are just defaults.
		switch (a_type) {
		// The tightest and the lowest angles get Hard.
		case ShotType::kExtremeClose:
		case ShotType::kExtremeClosePlayer:
		case ShotType::kCloseLow:
		case ShotType::kLowAngle:
		case ShotType::kLowProfile:
		case ShotType::kPlayerLow:
			return "hard";

		// The close range, where a face is big enough for fill light to matter.
		case ShotType::kCloseUp:
		case ShotType::kClosePlayer:
		case ShotType::kCloseHigh:
		case ShotType::kHighAngle:
		case ShotType::kCloseProfile:
		case ShotType::kProfile:
		case ShotType::kDirtyNpc:
		case ShotType::kDirtyPlayer:
			return "soft";

		// The room and the mostly-room shots. A key on any of them is a bright patch
		// on the floor.
		case ShotType::kDistant:
		case ShotType::kDistantLow:
		case ShotType::kMaster:
			return "off";

		default:
			return "natural";
		}
	}

	bool FavoursNpc(ShotType a_type) noexcept
	{
		return SpecFor(a_type).onNpc;
	}

	std::string_view MoveName(ShotType a_type) noexcept
	{
		switch (SpecFor(a_type).move) {
		case Move::kLocked:    return "locked"sv;
		case Move::kPushIn:    return "push-in"sv;
		case Move::kPullOut:   return "pull-out"sv;
		case Move::kCraneUp:   return "crane-up"sv;
		case Move::kCraneDown: return "crane-down"sv;
		case Move::kTiltUp:    return "tilt-up"sv;
		case Move::kTiltDown:  return "tilt-down"sv;
		case Move::kDrift:     return "drift"sv;
		case Move::kZoomIn:    return "zoom-in"sv;
		case Move::kZoomOut:   return "zoom-out"sv;
		default:               return "locked"sv;
		}
	}

	float LensOf(ShotType a_type) noexcept
	{
		// The live lens, not the authored one, since the cut log prints this.
		// AuthoredLens() returns the original.
		return static_cast<float>(Shot::Lens(a_type));
	}

	float FillOf(ShotType a_type) noexcept
	{
		return SpecFor(a_type).fill;
	}

	bool OverShoulder(ShotType a_type) noexcept
	{
		return SpecFor(a_type).overShoulder;
	}

	bool FollowsFace(ShotType a_type) noexcept
	{
		const auto spec = SpecFor(a_type);
		return spec.anchor == Anchor::kSubject && spec.aim == Aim::kSubject &&
			!spec.overShoulder && spec.fill >= 0.5f;
	}

	bool IsNeutral(ShotType a_type) noexcept
	{
		switch (a_type) {
		case ShotType::kTwoShot:
		case ShotType::kProfile:
		case ShotType::kWide:
		case ShotType::kDistant:
		case ShotType::kMaster:
		case ShotType::kGroundLevel:
		case ShotType::kDistantLow:
			return true;
		default:
			return false;
		}
	}

	void Shot::SetEnabled(ShotType a_type, bool a_enabled) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < selectionSettings.enabled.size()) {
			selectionSettings.enabled[index].store(a_enabled, std::memory_order_relaxed);
		}
	}

	bool Shot::Enabled(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < selectionSettings.enabled.size() &&
			selectionSettings.enabled[index].load(std::memory_order_relaxed);
	}

	void Shot::SetWeight(ShotType a_type, int a_weight) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < selectionSettings.weights.size()) {
			selectionSettings.weights[index].store(static_cast<std::uint8_t>(std::clamp(a_weight, 0, 100)),
				std::memory_order_relaxed);
		}
	}

	int Shot::Weight(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < selectionSettings.weights.size() ?
			selectionSettings.weights[index].load(std::memory_order_relaxed) : 0;
	}

	void Shot::SetLens(ShotType a_type, int a_degrees) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < lensDegrees.size()) {
			lensDegrees[index] =
				static_cast<std::uint8_t>(std::clamp(a_degrees, kMinLens, kMaxLens));
		}
	}

	int Shot::Lens(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lensDegrees.size() ?
			lensDegrees[index] :
			static_cast<int>(SpecFor(a_type).lens);
	}

	float AuthoredLens(ShotType a_type) noexcept
	{
		return SpecFor(a_type).lens;
	}

	int AuthoredWeight(ShotType a_type) noexcept
	{
		// The staple setups of filmed dialogue (the shoulder pair, the two mediums,
		// the two tight over-the-shoulders, the three-quarters, and the close-up on
		// the speaker). Everything else is an accent against these. A separate switch
		// rather than another ShotSpec column, which would mean writing "50"
		// thirty-one times.
		switch (a_type) {
		case ShotType::kCloseUp:
		case ShotType::kMediumNpc:
		case ShotType::kOverPlayerShoulder:
		case ShotType::kDirtyNpc:
		case ShotType::kThreeQuarterNpc:
		case ShotType::kMediumPlayer:
		case ShotType::kOverNpcShoulder:
		case ShotType::kDirtyPlayer:
			return kDefaultWeight * 2;
		default:
			return kDefaultWeight;
		}
	}

	void Shot::SetMove(ShotType a_type, Move a_move) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < moveChoice.size() && a_move < Move::kCount) {
			moveChoice[index] = static_cast<std::uint8_t>(a_move);
		}
	}

	Move Shot::MoveOf(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < moveChoice.size() ? static_cast<Move>(moveChoice[index]) : Move::kLocked;
	}

	void Shot::SetMoveAmount(ShotType a_type, int a_strength) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < moveStrength.size()) {
			moveStrength[index] = static_cast<std::uint8_t>(std::clamp(a_strength, 0, 100));
		}
	}

	int Shot::MoveAmount(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < moveStrength.size() ? moveStrength[index] : 0;
	}

	void Shot::SetMoveTime(ShotType a_type, int a_hundredths) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < moveTime.size()) {
			moveTime[index] = static_cast<std::uint16_t>(std::clamp(a_hundredths, 30, 900));
		}
	}

	int Shot::MoveTime(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < moveTime.size() ? moveTime[index] : kDefaultMoveTime;
	}

	void Shot::SetLight(ShotType a_type, int a_look) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < lightRig.size()) {
			lightRig[index] = static_cast<std::int8_t>(std::clamp(a_look, -1, 127));
		}
	}

	int Shot::LightOf(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightRig.size() ? lightRig[index] : -1;
	}

	void Shot::SetLightOffset(ShotType a_type, int a_x, int a_y, int a_z) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		if (index < lightOffX.size()) {
			lightOffX[index] = static_cast<std::int16_t>(std::clamp(a_x, -400, 400));
			lightOffY[index] = static_cast<std::int16_t>(std::clamp(a_y, -400, 400));
			lightOffZ[index] = static_cast<std::int16_t>(std::clamp(a_z, -400, 400));
		}
	}

	int Shot::LightOffsetX(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightOffX.size() ? lightOffX[index] : 0;
	}

	int Shot::LightOffsetY(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightOffY.size() ? lightOffY[index] : 0;
	}

	int Shot::LightOffsetZ(ShotType a_type) noexcept
	{
		const auto index = static_cast<std::size_t>(a_type);
		return index < lightOffZ.size() ? lightOffZ[index] : 0;
	}

	Move Shot::AuthoredMove(ShotType a_type) noexcept
	{
		return SpecFor(a_type).move;
	}

	int Shot::AuthoredMoveAmount(ShotType a_type) noexcept
	{
		return AuthoredStrength(a_type);
	}

	int Shot::AuthoredMoveTime(ShotType) noexcept
	{
		return kDefaultMoveTime;
	}

	std::string_view MoveLabel(Move a_move) noexcept
	{
		switch (a_move) {
		case Move::kLocked:     return "Locked off"sv;
		case Move::kPushIn:     return "Push in"sv;
		case Move::kPullOut:    return "Pull out"sv;
		case Move::kCraneUp:    return "Crane up"sv;
		case Move::kCraneDown:  return "Crane down"sv;
		case Move::kTiltUp:     return "Tilt up"sv;
		case Move::kTiltDown:   return "Tilt down"sv;
		case Move::kDrift:      return "Drift"sv;
		case Move::kZoomIn:     return "Zoom in"sv;
		case Move::kZoomOut:    return "Zoom out"sv;
		case Move::kOrbitLeft:  return "Orbit left"sv;
		case Move::kOrbitRight: return "Orbit right"sv;
		case Move::kTruckLeft:  return "Slide left"sv;
		case Move::kTruckRight: return "Slide right"sv;
		default:                return "?"sv;
		}
	}


	void CheckVisibility(ShotType a_type, Pose& a_pose, const Subjects& a_subjects)
	{
		a_pose.visibility = {};
		a_pose.lensClearance = SightState::kUnknown;
		if (!a_pose.valid) {
			return;
		}

		SightContext localContext{};
		const SightContext* context = a_subjects.sightContext;
		if (!context) {
			localContext = BuildSightContext();
			context = &localContext;
		}
		a_pose.lensClearance = LensClearance(a_pose.position, *context);
		if (a_pose.lensClearance == SightState::kBlocked) {
			a_pose.visibility.state = SightState::kBlocked;
			a_pose.visibility.severe = true;
			return;
		}

		const auto spec = SpecFor(a_type);
		const float lens = a_pose.lens > 0.0f ? a_pose.lens : a_subjects.fovDegrees;
		const auto& subject = spec.onNpc ? a_subjects.npcSight : a_subjects.playerSight;
		a_pose.visibility = SubjectVisibility(a_pose.position, a_pose.lookAt, lens,
			a_subjects.aspect, a_subjects.cropFractionPerEdge, subject, *context);

		// A two-person or room setup must show both faces. The listener in an
		// over-the-shoulder is optional foreground: only covering the main subject's
		// protected samples disqualifies it.
		if (spec.anchor != Anchor::kSubject || spec.aim != Aim::kSubject) {
			const auto& other = spec.onNpc ? a_subjects.playerSight : a_subjects.npcSight;
			const auto sight = SubjectVisibility(a_pose.position, a_pose.lookAt, lens,
				a_subjects.aspect, a_subjects.cropFractionPerEdge, other, *context);
			a_pose.visibility.face = std::min(a_pose.visibility.face, sight.face);
			a_pose.visibility.torso = std::min(a_pose.visibility.torso, sight.torso);
			a_pose.visibility.severe = a_pose.visibility.severe || sight.severe;
			if (sight.state == SightState::kBlocked || a_pose.visibility.state == SightState::kBlocked) {
				a_pose.visibility.state = SightState::kBlocked;
			} else if (sight.state != SightState::kClear || a_pose.visibility.state != SightState::kClear) {
				a_pose.visibility.state = SightState::kUnknown;
			}
		}
		if (a_pose.lensClearance != SightState::kClear && a_pose.visibility.state == SightState::kClear) {
			a_pose.visibility.state = SightState::kUnknown;
		}
	}

	Pose Solve(ShotType a_type, const Subjects& a_subjects)
	{
		Pose       pose{};
		if (!Shot::Enabled(a_type) || Shot::Weight(a_type) <= 0) {
			return pose;
		}
		// A carried placement can't reintroduce a wide sweep through the held branch,
		// which skips candidate generation on purpose.
		if (!std::isfinite(a_subjects.heldSweep) ||
			(a_subjects.heldSweep > kUnheld && !ShotAngles::AllowedAdjustment(a_subjects.heldSweep))) {
			return pose;
		}
		const auto spec = SpecFor(a_type);

		const RE::NiPoint3 subject = spec.onNpc ? a_subjects.npcHead : a_subjects.playerHead;
		const RE::NiPoint3 other = spec.onNpc ? a_subjects.playerHead : a_subjects.npcHead;

		// Whose measurements the framing uses: the subject's for subject-anchored
		// setups, the larger of the two for midpoint and scene setups (otherwise a
		// two-shot of a person and a dragon would frame the person and a shin).
		const Anatomy& body = spec.anchor == Anchor::kSubject ?
			(spec.onNpc ? a_subjects.npc : a_subjects.player) :
			(a_subjects.npc.extent >= a_subjects.player.extent ? a_subjects.npc : a_subjects.player);

		const RE::NiPoint3 midpoint{
			(a_subjects.playerHead.x + a_subjects.npcHead.x) * 0.5f,
			(a_subjects.playerHead.y + a_subjects.npcHead.y) * 0.5f,
			(a_subjects.playerHead.z + a_subjects.npcHead.z) * 0.5f
		};

		// The base direction runs from the subject toward the person they're talking
		// to; looking back along it shows their face.
		const float lineSide = ShotAngles::LineSide(a_subjects.side, spec.onNpc, a_subjects.true180);
		bool       ok = false;
		const auto toOther = Normalized(
			{ other.x - subject.x, other.y - subject.y, other.z - subject.z }, ok);
		if (!ok) {
			return pose;
		}

		const float separation = Length(
			RE::NiPoint3{ other.x - subject.x, other.y - subject.y, other.z - subject.z });

		// Close-ups frame the face (see FaceFrame.h). The eyeline stays measured
		// between the stable points, so the line doesn't move with a head. What moves
		// is the point this shot stands off from and aims at, plus how far the bearing
		// turns toward the face and how far the camera drops for a lowered face (both
		// decided at the cut and held).
		const auto& face = spec.onNpc ? a_subjects.npcFace : a_subjects.playerFace;
		const bool  faced = a_subjects.followFace && face.valid && FollowsFace(a_type);
		RE::NiPoint3 framed = subject;
		float        faceYaw = 0.0f;
		float        facePitch = 0.0f;
		if (faced) {
			framed = subject + face.offset;
			if (a_subjects.heldFaceYaw > kUnheld && a_subjects.heldFacePitch > kUnheld) {
				faceYaw = a_subjects.heldFaceYaw;
				facePitch = a_subjects.heldFacePitch;
			} else {
				const auto bias = FaceFrame::BiasFor({ toOther.x, toOther.y, toOther.z },
					{ face.facing.x, face.facing.y, face.facing.z });
				faceYaw = bias.yaw;
				facePitch = bias.pitch;
			}
		}
		const auto stampFace = [&](Pose& a_pose) {
			a_pose.faceYaw = faced ? faceYaw : kUnheld;
			a_pose.facePitch = faced ? facePitch : kUnheld;
		};

		// How much a placement is worth for where it stands relative to the face (see
		// Subjects::avoidBackOfHead): 1 in front or beside, falling toward zero
		// behind. Only for setups aimed at their subject.
		const auto facingWorth = [&](const Pose& a_pose) {
			if (!a_subjects.avoidBackOfHead || spec.aim != Aim::kSubject) {
				return 1.0f;
			}
			RE::NiPoint3 forward = face.valid ? face.facing :
				(spec.onNpc ? a_subjects.npcForward : a_subjects.playerForward);
			forward.z = 0.0f;
			RE::NiPoint3 toCamera{ a_pose.position.x - subject.x, a_pose.position.y - subject.y, 0.0f };
			bool forwardOk = false;
			bool cameraOk = false;
			forward = Normalized(forward, forwardOk);
			toCamera = Normalized(toCamera, cameraOk);
			if (!forwardOk || !cameraOk) {
				return 1.0f;
			}
			const float dot = forward.x * toCamera.x + forward.y * toCamera.y;
			return dot >= 0.0f ? 1.0f : std::max(0.05f, 1.0f + dot * 1.5f);
		};

		// The move, eased over the life of the shot.
		const float p = Ease(a_subjects.progress);

		// The setup's live field of view, clamped to the range DistanceForFill solves
		// over before the distance is taken from it; otherwise the standoff would be
		// solved for one FOV and rendered at another. The post-move clamp below is
		// wider on purpose, since a zoom may narrow the rendered lens once the
		// standoff is fixed.
		float lens = static_cast<float>(
			std::clamp(Shot::Lens(a_type), kMinLens, kMaxLens));

		// Per-build adjustments. A humanoid gets 1.0, 1.0, 0, which changes nothing.
		const auto tuning = Tuning(body.build);

		// Distance is solved at the base lens, before any zoom, so a zoom magnifies
		// away from the size the shot asked for. The fill is capped per build (0.85 on
		// a dragon would frame the underside of its jaw).
		//
		// The extreme close-ups are measured against the head (kHeadShare of the
		// extent) rather than head and shoulders, so they actually crop below the
		// chin.
		Anatomy sized = body;
		if (a_type == ShotType::kExtremeClose || a_type == ShotType::kExtremeClosePlayer) {
			sized.extent *= kHeadShare;
		}
		float distance = DistanceForFill(
			std::min(spec.fill, tuning.fillCap), lens, a_subjects.aspect, sized);

		// Rise is in world units, so it scales with the subject, but only partly, by
		// the build's riseScale. Everything else in the table is relative.
		float rise = spec.rise * body.scale * tuning.riseScale;

		// Drop for a lowered face, by the height that puts the lens on its line.
		if (faced) {
			rise += distance * std::sin(facePitch * kDeg);
		}

		// Clamped under the measured ceiling. Upward only: a low angle's floor is
		// where the people are standing. riseScale is a framing choice; this is a
		// physical limit.
		if (rise > 0.0f && a_subjects.ceiling > 1.0f) {
			constexpr float kCeilingMargin = 24.0f;
			rise = std::min(rise, std::max(a_subjects.ceiling - kCeilingMargin, 0.0f));
		}

		// Step further off the eyeline for a long head, signed with the angle so it
		// never crosses.
		float angle = spec.angleDeg +
			(spec.angleDeg < 0.0f ? -tuning.angleBias : tuning.angleBias);
		float aimLift = 0.0f;

		// Turn toward where the face points. toOther is turned by angle * lineSide, so
		// the face yaw is applied the same way and stays on the allowed side of the
		// line.
		if (faced) {
			angle += faceYaw * lineSide;
			if (a_subjects.enforceLine) {
				angle = std::clamp(angle, kLineFloorSubject, ShotAngles::kLineCeiling);
			}

			// Never below the floor the subject stands on (subject is the stable head
			// point, eyeHeight above the root).
			const float ground = subject.z - body.eyeHeight;
			rise = std::max(rise, ground + 30.0f * body.scale - framed.z);
		}

		// The move is the configured one, not the table's. The amount is 0-100 of this
		// move's full travel, so switching move types keeps the intensity. Unless this
		// shot has a move override (the persuasion beat's push).
		const bool  overridden = a_subjects.moveOverride != Move::kCount;
		const Move  move = overridden ? a_subjects.moveOverride : Shot::MoveOf(a_type);
		const float strength = overridden ?
			std::clamp(a_subjects.moveOverrideStrength, 0.0f, 1.0f) :
			static_cast<float>(Shot::MoveAmount(a_type)) / 100.0f;
		const float travel = FullScale(move) * strength;

		// World-unit moves scale with the subject, like rise. Fractions and degrees
		// are already relative.
		const float units = travel * body.scale * tuning.riseScale;

		// Signed lateral slide, applied after the aim is composed. Zero for every
		// other move.
		float truck = 0.0f;

		switch (move) {
		case Move::kLocked:                                     break;
		case Move::kPushIn:    distance *= 1.0f - travel * p;   break;
		case Move::kPullOut:   distance *= 1.0f + travel * p;   break;
		case Move::kZoomIn:    lens *= 1.0f - travel * p;       break;
		case Move::kZoomOut:   lens *= 1.0f + travel * p;       break;
		case Move::kCraneUp:   rise += units * p;               break;
		case Move::kCraneDown: rise -= units * p;               break;
		case Move::kTiltUp:    aimLift = units * p;             break;
		case Move::kTiltDown:  aimLift = -units * p;            break;

		// An orbit arcs around the subject while still pointing at them, which is just
		// a change of angle over the shot. kDrift is the same move without a
		// direction, kept so saved configs still work.
		case Move::kDrift:
		case Move::kOrbitRight: angle += travel * p;            break;
		case Move::kOrbitLeft:  angle -= travel * p;            break;

		// A slide doesn't re-aim, so it's applied after composition (ApplyTruck).
		case Move::kTruckLeft:  truck = -units * p;             break;
		case Move::kTruckRight: truck = units * p;              break;
		default:                                                break;
		}

		lens = std::clamp(lens, 30.0f, 120.0f);
		pose.lens = lens;

		// An over-the-shoulder has to stand beyond the other person, or there's no
		// shoulder in frame.
		if (spec.overShoulder) {
			distance = std::max(distance, separation + 60.0f);
		}

		// Where the camera stands and what it points at are set separately.
		const RE::NiPoint3 anchorPoint = spec.anchor == Anchor::kSubject ? framed : midpoint;
		const RE::NiPoint3 aimPoint = spec.aim == Aim::kSubject ? framed : midpoint;
		const RE::NiPoint3 target{ aimPoint.x, aimPoint.y, aimPoint.z + 3.0f + aimLift };

		if (a_subjects.protectSubject) {
			// Compose and verify each bearing before choosing it, so a blocked angle can't
			// hide a readable neighbour of the same setup. Kept separate so saved legacy
			// placement keeps its exact bundle scoring and hold behavior.
			const bool held = a_subjects.heldSweep > kUnheld;
			const bool check = !held || a_subjects.checkVisibility;
			SightContext localContext{};
			Subjects checkedSubjects = a_subjects;
			if (check && !checkedSubjects.sightContext) {
				localContext = BuildSightContext();
				checkedSubjects.sightContext = &localContext;
			}

			const bool scene = spec.anchor == Anchor::kScene;
			const float wanted = scene ?
				std::clamp(std::min(distance, a_subjects.openDistance * 0.9f), 200.0f, 2400.0f) : distance;
			const auto baseDirection = scene ? a_subjects.openDirection : toOther;
			ShotAngles::Candidates candidates{};
			std::size_t count = 1;
			if (held) {
				candidates[0] = angle + a_subjects.heldSweep;
			} else {
				const float floor = spec.anchor == Anchor::kMidpoint ? kLineFloorMidpoint : kLineFloorSubject;
				count = ShotAngles::MakeCandidates(angle, floor, a_subjects.enforceLine && !scene, candidates);
			}

			Pose best{};
			Pose rejected{};
			float bestScore = -1.0f;
			for (std::size_t i = 0; i < count; ++i) {
				const float offset = candidates[i] - angle;
				const auto direction = RotateAboutZ(baseDirection, candidates[i] * kDeg * (scene ? 1.0f : lineSide));
				const auto compose = [&](float available, float rememberedRoom) {
					const float use = LimitStandoff(a_subjects,
						std::max(std::min(wanted, available), body.minDistance));
					Pose result{};
					result.position = {
						anchorPoint.x + direction.x * use,
						anchorPoint.y + direction.y * use,
						anchorPoint.z + direction.z * use + rise
					};
					result.lookAt = Compose(result.position, target, toOther, lens, a_subjects.aspect,
						spec.headroom, spec.lookRoom);
					ApplyTruck(result, truck);
					result.lens = lens;
					result.sweep = offset;
					result.standoff = use;
					result.room = rememberedRoom;
					result.valid = true;
					if (check) {
						CheckVisibility(a_type, result, checkedSubjects);
					}
					const float sight = check ?
						0.8f * result.visibility.face + 0.2f * result.visibility.torso : 1.0f;
					result.quality = Score(wanted, use, sight, offset) * facingWorth(result);
					stampFace(result);
					return result;
				};
				// The placement anchor and the final sightline differ, especially during a
				// slide, so test the actual pose first. A proposal ray through harmless
				// foreground never vetoes it.
				const float room = held && a_subjects.holdPlacement && a_subjects.heldRoom > kUnheld ?
					a_subjects.heldRoom : kOpenRoom;
				auto candidate = compose(room >= kOpenRoom ? wanted : room, room);
				if (held) {
					// Placement is kept even on a blocked or unknown reading, so the Director can
					// time a cut without the camera pumping.
					return candidate;
				}
				if ((candidate.visibility.state != SightState::kClear ||
					(a_subjects.requireFullFace && candidate.visibility.face < 0.99f)) && spec.anchor == Anchor::kSubject) {
					const auto shorter = NarrowRoom(anchorPoint, direction, rise, wanted, body.probeStart, body.extent);
					if (shorter.distance >= body.minDistance && shorter.distance < wanted - 1.0f) {
						candidate = compose(shorter.distance, shorter.distance);
					}
				}
				if (candidate.visibility.state != SightState::kClear ||
					(a_subjects.requireFullFace && candidate.visibility.face < 0.99f)) {
					rejected = candidate;
					rejected.valid = false;
					rejected.quality = 0.0f;
					continue;
				}
				if (candidate.quality > bestScore) {
					bestScore = candidate.quality;
					best = candidate;
				}
				// At most nine placements. A near-perfect angle already beats any other.
				if (bestScore >= 0.985f) {
					break;
				}
			}
			return best.valid ? best : rejected;
		}

		// Room shots: stand off in the direction the space opens up and let the people
		// fall where they fall. angleDeg is relative to the open direction here, so
		// several room shots look from different corners. The 180-degree rule doesn't
		// apply: there's no subject to be on the wrong side of.
		if (spec.anchor == Anchor::kScene) {
			// What the framing wants, capped by the room. Same ceiling as DistanceForFill,
			// so outdoors a long lens gets real distance while a corridor still pulls the
			// shot in.
			const float reach = std::clamp(std::min(distance, a_subjects.openDistance * 0.9f),
				200.0f, 2400.0f);

			// A room shot stands where the space opens, at its own angle off that
			// direction. Kept as a one-element loop so the held-angle bookkeeping matches
			// the other path.
			const std::array<float, 1> swings{
				a_subjects.heldSweep > kUnheld ? a_subjects.heldSweep : 0.0f
			};

			for (const float swing : swings) {
				const auto  direction = RotateAboutZ(a_subjects.openDirection, (angle + swing) * kDeg);
				const Room  room = RoomHere(a_subjects, anchorPoint, direction, rise, reach,
					body.probeStart, body.extent);
				if (room.distance < body.minDistance) {
					continue;
				}

				const float use = LimitStandoff(a_subjects, std::min(reach, room.distance));
				pose.position = {
					anchorPoint.x + direction.x * use,
					anchorPoint.y + direction.y * use,
					anchorPoint.z + direction.z * use + rise
				};
				pose.lookAt = Compose(pose.position, target, toOther, lens, a_subjects.aspect,
					spec.headroom, spec.lookRoom);
				ApplyTruck(pose, truck);
				pose.sweep = swing;
				pose.standoff = use;
				pose.room = RememberRoom(a_subjects, room.distance, room.clear);
				pose.valid = true;

				// A body crossing a room shot costs less than on a single, but both people
				// still have to be visible.
				const float sight = Visibility(pose, subject, other, spec, truck, a_subjects);
				pose.quality = Score(reach, use, room.clear * sight, swing);
				return pose;
			}

			return pose;
		}

		// Sweep for an angle with room, keeping the size the shot asked for. Distance
		// is only reduced as a last resort and never past the floor. Only small
		// adjustments around the intended bearing, line rule on or off.
		const float lineFloor = spec.anchor == Anchor::kMidpoint ?
			kLineFloorMidpoint : kLineFloorSubject;

		float        bestRoom = 0.0f;
		float        bestClear = 1.0f;
		float        bestOffset = 0.0f;
		RE::NiPoint3 bestDirection = RotateAboutZ(toOther, angle * kDeg * lineSide);

		if (a_subjects.heldSweep > kUnheld) {
			// The angle was already chosen at the cut. Hold Placement decides whether the
			// room along it is re-measured (see Subjects::holdPlacement); the angle itself
			// isn't re-chosen either way.
			bestOffset = a_subjects.heldSweep;
			bestDirection = RotateAboutZ(toOther, (angle + bestOffset) * kDeg * lineSide);
			const Room held = RoomHere(a_subjects, anchorPoint, bestDirection, rise, distance,
				body.probeStart, body.extent);
			bestRoom = held.distance;
			bestClear = held.clear;
		} else {
			// Best, not first that fits: the first bearing with barely enough room could
			// have a railing across half the frame while the next one is clear. Candidates
			// are de-duplicated by ShotAngles, so a setup near the floor probes only a few
			// bearings.
			ShotAngles::Candidates candidates{};
			const std::size_t                     count =
				ShotAngles::MakeCandidates(angle, lineFloor, a_subjects.enforceLine, candidates);

			float bestScore = -1.0f;

			for (std::size_t i = 0; i < count; ++i) {
				const float swept = candidates[i] * kDeg * lineSide;
				const auto  direction = RotateAboutZ(toOther, swept);
				const Room  room = RoomAlong(anchorPoint, direction, rise, distance, body.probeStart, body.extent);

				if (room.distance < body.minDistance) {
					continue;  // cannot stand here at all
				}

				const float offset = candidates[i] - angle;
				const float score = Score(distance, std::min(distance, room.distance), room.clear, offset);
				if (score > bestScore) {
					bestScore = score;
					bestRoom = room.distance;
					bestClear = room.clear;
					bestOffset = offset;
					bestDirection = direction;
				}
			}

			// Nothing placed. Fall through to the refusal with bestRoom still zero.
			if (bestScore < 0.0f) {
				bestRoom = 0.0f;
			}
		}

		// No bearing has room without putting the lens inside someone. Refuse; the
		// caller tries a tighter shot, which needs less room. On a held angle this
		// also hands the Director its remembered pose, which holds still instead of
		// swinging to a neighbour.
		if (bestRoom < body.minDistance) {
			return pose;
		}

		// A max, not std::clamp: a push-in can bring `distance` below the floor, which
		// would invert the clamp's bounds (undefined behaviour). The upper bound was
		// never reachable anyway.
		const float use = LimitStandoff(a_subjects,
			std::max(std::min(distance, bestRoom), body.minDistance));

		pose.position = {
			anchorPoint.x + bestDirection.x * use,
			anchorPoint.y + bestDirection.y * use,
			anchorPoint.z + bestDirection.z * use + rise
		};
		pose.lookAt = Compose(pose.position, target, toOther, lens, a_subjects.aspect,
			spec.headroom, spec.lookRoom);
		ApplyTruck(pose, truck);
		pose.sweep = bestOffset;
		pose.standoff = use;
		pose.room = RememberRoom(a_subjects, bestRoom, bestClear);
		pose.valid = true;
		stampFace(pose);

		// Scored against what the shot asked for, not what it settled on, or every
		// placement would get full marks.
		const float sight = Visibility(pose, framed, other, spec, truck, a_subjects);
		pose.quality = Score(distance, use, bestClear * sight, bestOffset) * facingWorth(pose);
		return pose;
	}

	float SideFor(ShotType a_type, const Subjects& a_subjects) noexcept
	{
		return ShotAngles::LineSide(a_subjects.side, SpecFor(a_type).onNpc, a_subjects.true180);
	}
}

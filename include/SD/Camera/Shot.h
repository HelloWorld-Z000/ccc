#pragma once

#include "SD/Camera/Anatomy.h"
#include "SD/Camera/Space.h"

namespace SD::Camera
{
	enum class ShotType : std::uint8_t
	{
		// NPC coverage, used while they're talking.
		kOverPlayerShoulder = 0,  // their face, your shoulder in the corner
		kCloseUp,                 // their face fills the frame
		kMediumNpc,               // head and shoulders, clean
		kLongNpc,                 // full figure, room around them
		kLowAngle,                // close, from below eye level

		// Variations inside the close range, where most of a conversation is spent.
		kCloseProfile,            // tight and side-on, their face in profile
		kCloseLow,                // tight, below the eyeline, looking up
		kCloseHigh,               // tight, above the eyeline, looking down
		kCloseWide,               // loose single, shoulders and some room

		// Only offered on lines the writer marked at high intensity. Tighter than
		// kCloseUp; crops below the chin.
		kExtremeClose,

		// A close single with a sliver of the listener still in frame (a "dirty"
		// single). Tighter than the over-the-shoulders.
		kDirtyNpc,

		// Between an over-the-shoulder and a profile; the most common angle in filmed
		// conversation.
		kThreeQuarterNpc,

		// Side-on variants at sizes the close range doesn't cover.
		kMediumProfile,
		kLowProfile,

		// Steeply above, looking down. Unlike kCloseHigh (a tilt), this reads as a
		// vantage point.
		kOverhead,

		// Height and size variants of the over-the-shoulder.
		kOverPlayerShoulderLow,   // ducked under the shoulder, looking up
		kOverPlayerShoulderHigh,  // raised above it, looking down
		kOverPlayerShoulderWide,  // loose, both bodies and the room around them

		// Player coverage, used on the player's turn. Mirrors the NPC side so a
		// reverse is as varied as the shot it answers.
		kOverNpcShoulder,
		kMediumPlayer,
		kHighAngle,
		kClosePlayer,

		// The player's counterpart to kExtremeClose.
		kExtremeClosePlayer,

		kDirtyPlayer,
		kPlayerProfile,
		kPlayerLow,
		kThreeQuarterPlayer,

		// Full figure and overhead on the player's side too, so the two halves can be
		// cut against each other at matching sizes.
		kOverNpcShoulderLow,
		kOverNpcShoulderHigh,
		kOverNpcShoulderWide,
		kLongPlayer,
		kPlayerOverhead,

		// Neutral.
		kTwoShot,
		kProfile,
		kWide,
		kDistant,

		// The room. kMaster is the whole space from up and back, the widest setup;
		// kGroundLevel is near the floor looking up at the two of them.
		kMaster,
		kGroundLevel,
		kDistantLow,

		kCount
	};

	// How the camera's position is derived.
	enum class Anchor : std::uint8_t
	{
		kSubject,   // orbit the person the shot is about
		kMidpoint,  // orbit the point between the two of them
		kScene,     // placed against the room, hooked to nobody
	};

	// What the camera points at, set separately from where it stands.
	enum class Aim : std::uint8_t
	{
		kSubject,
		kMidpoint,
		kScene,  // the space itself; the participants sit off-centre by composition
	};

	// What the camera does over the life of the shot. The table only supplies the
	// default; each setup's move is a player setting (presets set them the same
	// way). A dolly (kPushIn) and a zoom (kZoomIn) are different images: a dolly
	// changes perspective, a zoom doesn't.
	//
	// The ordering is the ini storage format, so only ever append. Inserting a
	// value in the middle would change everyone's saved choices.
	enum class Move : std::uint8_t
	{
		kLocked,     // on sticks; nothing moves. Makes the moving shots read.
		kPushIn,
		kPullOut,
		kCraneUp,    // the camera physically rises
		kCraneDown,
		kTiltUp,     // the aim rises; the camera stays put
		kTiltDown,
		kDrift,      // a slow lateral arc around the anchor
		kZoomIn,     // the lens tightens; perspective does not change
		kZoomOut,

		// Two ways to go sideways. An orbit arcs around the subject and keeps pointing
		// at them, so the background slides behind them. A truck slides the camera
		// without turning, so the subject drifts across and out of frame. kDrift is
		// the old name for an orbit, kept for saved configs.
		kOrbitLeft,
		kOrbitRight,
		kTruckLeft,
		kTruckRight,

		kCount
	};

	// For the menu and the log. A short phrase.
	[[nodiscard]] std::string_view MoveLabel(Move a_move) noexcept;

	[[nodiscard]] std::string_view Name(ShotType a_type) noexcept;

	[[nodiscard]] bool             FavoursNpc(ShotType a_type) noexcept;

	// Who the shot is of ("them", "you" or "room"). Name() relies on the menu's
	// headings to say which side a shot belongs to; the log has no headings, so it
	// prints this beside the name.
	[[nodiscard]] std::string_view SubjectName(ShotType a_type) noexcept;

	[[nodiscard]] std::string_view MoveName(ShotType a_type) noexcept;
	[[nodiscard]] float            LensOf(ShotType a_type) noexcept;

	// How much of the frame height the subject fills. Tighter setups need less
	// room, so the fallback search orders by this.
	[[nodiscard]] float FillOf(ShotType a_type) noexcept;

	// The [Shots] ini key for this setup's on/off. Separate from Name(), which is
	// display text and can change; a key must never change once shipped. Returns a
	// C string because it goes straight to the profile API.
	[[nodiscard]] const char* Key(ShotType a_type) noexcept;

	// The ini key for this setup's weight. Separate from Key() so configs from
	// before weights existed keep their choices.
	[[nodiscard]] const char* WeightKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* LensKey(ShotType a_type) noexcept;

	// iNameMove / iNameMoveAmount / iNameMoveTime. ZoomKey is only used to read a
	// pre-1.3 config once so an existing zoom choice carries over.
	[[nodiscard]] const char* MoveKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* MoveAmountKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* MoveTimeKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* ZoomKey(ShotType a_type) noexcept;

	// sNameLight: the lighting look for this setup, by name (looks can be added to
	// the table, so an index in the file could end up pointing at the wrong one).
	// Only read when [Lighting] bPerShot is on.
	[[nodiscard]] const char* LightKey(ShotType a_type) noexcept;

	// iNameLightX / Y / Z: this setup's light nudge in camera space, on top of the
	// global one. Same axes as Scene::KeyLight::SetOffset.
	[[nodiscard]] const char* LightXKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* LightYKey(ShotType a_type) noexcept;
	[[nodiscard]] const char* LightZKey(ShotType a_type) noexcept;

	// The look this setup ships with, as a Scene::LookSpec key. Returned as a name
	// so this header doesn't depend on the lighting code.
	[[nodiscard]] const char* AuthoredLight(ShotType a_type) noexcept;

	// The normal weight. Accents ship at this, staples at twice it.
	inline constexpr int kDefaultWeight = 50;

	// What this setup's weight ships at: 100 for the eight staples, 50 for
	// everything else. Every pool lists each setup once and uses the weight as it
	// reads, so the slider value is what actually competes.
	[[nodiscard]] int AuthoredWeight(ShotType a_type) noexcept;

	// The field-of-view range every setup can be tuned across. Outside it the math
	// breaks down: below about 20 the standoff exceeds any interior, above 150
	// it's a fisheye. DistanceForFill clamps to the same range, and the two must
	// agree.
	inline constexpr int kMinLens = 20;
	inline constexpr int kMaxLens = 150;

	// The table's lens for this setup, ignoring tuning. Used by the menu's reset.
	[[nodiscard]] float AuthoredLens(ShotType a_type) noexcept;

	// Shots that belong to neither party (the room, or both people). Always valid
	// coverage, so the camera can sit on the room while the player reads the topic
	// list.
	[[nodiscard]] bool IsNeutral(ShotType a_type) noexcept;

	// Whether this setup stands behind the other party and puts them in the corner
	// of frame. Exposed because the shoulder belongs to whoever isn't the subject:
	// over the player's shoulder at a dragon works, over a dragon's shoulder at
	// the player doesn't.
	[[nodiscard]] bool OverShoulder(ShotType a_type) noexcept;

	// "Nothing held; search and report." A real sentinel because 0 is a valid
	// value for both fields that use it.
	inline constexpr float kUnheld = -1.0e9f;

	// "Measured, and nothing was in the way as far as the shot asked." RoomAlong
	// only probes as far as the shot wants, so a clear bearing reports exactly
	// that distance; remembering the number would cap any later pull-out move at
	// it. A clear bearing is stored as this instead, and a held frame treats it
	// like a clear probe.
	inline constexpr float kOpenRoom = 1.0e9f;

	struct Pose
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 lookAt{};
		bool         valid{ false };

		// Visibility is separate from placement: a held pose can still be placeable
		// while someone walks in front of the subject, and the Director needs that
		// result without the camera moving.
		SubjectSight visibility{};
		SightState   lensClearance{ SightState::kUnknown };

		// The angle this solve settled on, so the Director can hold it for the shot.
		// kUnheld means this path doesn't take part. An invalid solve leaves both
		// alone.
		float sweep{ kUnheld };
		float standoff{ kUnheld };

		// How much room the chosen bearing had, so only the cut measures it. kOpenRoom
		// means nothing was in the way. The Director keeps the first value for the
		// life of the shot.
		float room{ kUnheld };

		// How well the shot placed, 0..1; zero on a refusal. Lower when it had to come
		// closer than the framing wanted, step away from its own angle, or the view
		// along it is partly blocked. See Score() in Shot.cpp.
		float quality{ 0.0f };

		// The bearing and height taken from the subject's face, held for the shot like
		// the sweep. kUnheld when the setup doesn't follow the face or there was no
		// face. See FaceFrame.h.
		float faceYaw{ kUnheld };
		float facePitch{ kUnheld };

		// Horizontal field of view this shot wants, in degrees. 0 means no opinion
		// (use the player's own). The lens is a per-shot property: the same size and
		// angle on a 45 and a 95 are different images.
		float lens{ 0.0f };
	};

	struct Subjects
	{
		RE::NiPoint3 playerHead{};
		RE::NiPoint3 npcHead{};
		float        fovDegrees{ 75.0f };
		float        aspect{ 1.78f };

		// Which side of the eyeline the camera may stand on (the 180-degree rule).
		float side{ 1.0f };

		// 0..1 through the current shot, driving the move.
		float progress{ 0.0f };

		RE::NiPoint3 openDirection{ 1.0f, 0.0f, 0.0f };
		float        openDistance{ 400.0f };

		// Seconds since the last frame. Only the standoff limiter uses it.
		float delta{ 0.0f };

		// Clear space above the conversation in world units, measured when it stages.
		// Rises are clamped against it so overheads don't go through ceilings. 0 means
		// not measured (no clamp).
		float ceiling{ 0.0f };

		// The two participants, so the crowd test can tell them from bystanders.
		// Nobody else is skipped, followers included.
		RE::FormID npcId{ 0 };
		RE::FormID playerId{ 0 };

		// Whether the camera is held to one side of the eyeline. See kLineFloor.
		bool enforceLine{ true };

		// Keep both subjects' shots on one side of the eyeline. See
		// ShotAngles::LineSide.
		bool true180{ false };

		// Whether a body across the sightline lowers a shot's score. Off skips the
		// crowd probe entirely.
		bool avoidCrowds{ true };

		// Opt-in subject protection replaces the old full-width clearance score. The
		// actor snapshot can be shared across all candidates in a decision. Animated
		// sight targets are separate from the stable composition anchors.
		bool                protectSubject{ false };
		bool                requireFullFace{ false };  // returning from first person
		bool                checkVisibility{ true };
		const SightContext* sightContext{ nullptr };
		SightTarget         npcSight{};
		SightTarget         playerSight{};
		float               cropFractionPerEdge{ 0.0f };

		// Measurements of the people being framed. Shots pick theirs by spec.onNpc;
		// scene and midpoint setups use the larger of the two.
		Anatomy npc{};
		Anatomy player{};

		// The angle the sweep settled on, held for the life of the shot. Without this,
		// two bearings with near-equal room trade places from frame to frame and the
		// camera jumps between them. The full search still runs when the shot is
		// chosen (and for every candidate, since Subjects is rebuilt each frame). If
		// the held angle later gets blocked, Solve refuses and the Director holds the
		// last good pose.
		float heldSweep{ kUnheld };

		// Last frame's standoff, for the asymmetric rate limit. See LimitStandoff in
		// Shot.cpp.
		float heldStandoff{ kUnheld };

		// Use the room measured at the cut instead of re-measuring every frame.
		//
		// heldSweep stops the camera choosing a new angle mid-shot, but the distance
		// along that angle is still re-solved each frame, so something passing behind
		// the lens pulls it in and back out. With holdPlacement set, a shot that has
		// placed reuses the room from its cut and casts nothing.
		//
		// The cut itself is unaffected: this is false on the first frame of every
		// shot, so the full sweep, wall margin and refusal still decide where the
		// camera stands. A held shot can't be refused by geometry that arrives later,
		// and a subject who walks somewhere new takes the camera along at a fixed
		// bearing and distance.
		//
		// Subject protection also uses this to freeze placement, but its visibility
		// check still runs and can request an obstruction cut.
		bool  holdPlacement{ false };
		float heldRoom{ kUnheld };

		// The face, for close-ups (see FaceFrame.h). offset runs from the stable head
		// point to where the head bone has settled, eased through a dead band; facing
		// is the eased direction out of the face. Only read by close-range singles
		// with followFace on.
		struct Face
		{
			RE::NiPoint3 offset{};
			RE::NiPoint3 facing{};
			bool         valid{ false };
		};
		Face npcFace{};
		Face playerFace{};
		bool followFace{ true };

		// The face bearing and height the shot cut on, held like heldSweep.
		float heldFaceYaw{ kUnheld };
		float heldFacePitch{ kUnheld };

		// A move that replaces the setup's own for this shot, with its strength as a
		// share of the move's full travel. kCount means none. Set only on the render
		// path (the persuasion beat's push).
		Move  moveOverride{ Move::kCount };
		float moveOverrideStrength{ 0.0f };

		// Avoid shooting from behind. In the player's conversation the NPC faces the
		// player, so every bearing off the line between them looks at a face. Two NPCs
		// in a scene don't necessarily face each other, so with this on, a single
		// whose camera stands behind the face scores much lower. The forwards are body
		// headings, used when the face isn't tracked.
		bool         avoidBackOfHead{ false };
		RE::NiPoint3 npcForward{};
		RE::NiPoint3 playerForward{};
	};

	// Whether a setup frames the face closely enough to follow it: a clean,
	// subject-anchored single at half the frame or tighter. Over-the-shoulders are
	// excluded, since turning them would lose the shoulder.
	[[nodiscard]] bool FollowsFace(ShotType a_type) noexcept;

	// Solves a camera pose. Distance comes from how much of the frame the subject
	// should fill, not from how far apart the two people stand.
	//
	// With subject protection, an unheld sweep only admits verified clear poses.
	// Held poses keep their placement and report visibility separately.
	[[nodiscard]] Pose Solve(ShotType a_type, const Subjects& a_subjects);

	// The angle sign Solve uses for this setup (+1 or -1). The key light uses it
	// to stay on the camera's side of the line.
	[[nodiscard]] float SideFor(ShotType a_type, const Subjects& a_subjects) noexcept;

	// Recheck the actual lens and required subjects without choosing a bearing or
	// changing placement or quality. Always runs, even when Solve skipped the
	// check or is holding placement.
	void CheckVisibility(ShotType a_type, Pose& a_pose, const Subjects& a_subjects);

	// Per-setup settings the player can change: on/off, weight, lens, move and
	// lighting.
	class Shot
	{
	public:

		// Which setups the picker may draw. Unknown IDs are disabled. Zero weight also
		// excludes a setup from selection, solving and cached-pose reuse.
		static void               SetEnabled(ShotType a_type, bool a_enabled) noexcept;
		[[nodiscard]] static bool Enabled(ShotType a_type) noexcept;

		// How often this setup is drawn relative to the others in its pool, 0-100.
		static void              SetWeight(ShotType a_type, int a_weight) noexcept;
		[[nodiscard]] static int Weight(ShotType a_type) noexcept;

		// Field of view for this setup, in degrees (kMinLens..kMaxLens), seeded from
		// the table.
		static void              SetLens(ShotType a_type, int a_degrees) noexcept;
		[[nodiscard]] static int Lens(ShotType a_type) noexcept;

		// What this setup does over the shot, chosen by the player and seeded from the
		// table. A saved iNameZoom of 1 or 2 migrates to kZoomIn/kZoomOut on first
		// load.
		static void               SetMove(ShotType a_type, Move a_move) noexcept;
		[[nodiscard]] static Move MoveOf(ShotType a_type) noexcept;

		// How much of the move happens, 0-100 of that move's full travel (see
		// FullScale), so it means the same thing for every move and switching moves
		// keeps the intensity.
		static void              SetMoveAmount(ShotType a_type, int a_strength) noexcept;
		[[nodiscard]] static int MoveAmount(ShotType a_type) noexcept;

		// How long the move takes, in hundredths of a second.
		static void              SetMoveTime(ShotType a_type, int a_hundredths) noexcept;
		[[nodiscard]] static int MoveTime(ShotType a_type) noexcept;

		// The look this setup uses, as an index into Scene::AllLooks. -1 means not
		// resolved yet (look 0 is a real look, Off), so an unread setup falls back to
		// its shipped look rather than darkness.
		static void              SetLight(ShotType a_type, int a_look) noexcept;
		[[nodiscard]] static int LightOf(ShotType a_type) noexcept;

		// This setup's light nudge in camera space, added to the global one.
		static void              SetLightOffset(ShotType a_type, int a_x, int a_y, int a_z) noexcept;
		[[nodiscard]] static int LightOffsetX(ShotType a_type) noexcept;
		[[nodiscard]] static int LightOffsetY(ShotType a_type) noexcept;
		[[nodiscard]] static int LightOffsetZ(ShotType a_type) noexcept;

		// What the table ships this setup with, for the per-shot Default button.
		[[nodiscard]] static Move AuthoredMove(ShotType a_type) noexcept;
		[[nodiscard]] static int  AuthoredMoveAmount(ShotType a_type) noexcept;
		[[nodiscard]] static int  AuthoredMoveTime(ShotType a_type) noexcept;
	};
}

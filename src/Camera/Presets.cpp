#include "SD/Camera/Presets.h"

#include "SD/Camera/Director.h"
#include "SD/Core/Config.h"
#include "SD/Core/Logging.h"
#include "SD/Scene/LightRig.h"

namespace SD::Camera
{
	namespace
	{
		// Three looks, each with two to four setups per side of the exchange and a
		// cadence of two to five lines. Longer shot lists start overlapping and stop
		// having a character, and cutting on every line makes every look feel the
		// same.
		//
		// Every list is built in pairs, so a reverse can answer the shot it follows at
		// a matching size.

		// ---- Standard ----------------------------------------------------------
		//
		// Shoulder, single, reverse, and a shot of the pair. Classic coverage.
		constexpr std::array kStandard{
			ShotType::kOverPlayerShoulder, ShotType::kCloseUp,
			ShotType::kMediumNpc, ShotType::kThreeQuarterNpc,

			ShotType::kOverNpcShoulder, ShotType::kClosePlayer,
			ShotType::kMediumPlayer, ShotType::kThreeQuarterPlayer,

			ShotType::kTwoShot, ShotType::kProfile,
		};

		// ---- Close -------------------------------------------------------------
		//
		// The default look, and the shortest list: a shoulder shot to place the two of
		// you, a close single and an extreme close-up on each side, and one rarely
		// drawn room shot.
		//
		// Every default in the mod points at this list (see DefaultPreset), so a fresh
		// profile reads as Close with nothing in SD_user.ini.
		constexpr std::array kClose{
			ShotType::kOverPlayerShoulder, ShotType::kCloseUp, ShotType::kExtremeClose,

			ShotType::kClosePlayer, ShotType::kExtremeClosePlayer,

			ShotType::kDistant,
		};

		// ---- Room --------------------------------------------------------------
		//
		// The space first, the speaker second.
		//
		// The medium and the three-quarter are there because every other setup here
		// needs `roomy`; without them this preset would collapse to one shot per side
		// in corridors and small rooms. Weighted low so they stay out of the way when
		// the room opens up.
		constexpr std::array kRoom{
			ShotType::kLongNpc, ShotType::kOverhead,
			ShotType::kMediumNpc, ShotType::kThreeQuarterNpc,

			ShotType::kLongPlayer, ShotType::kPlayerOverhead,
			ShotType::kMediumPlayer, ShotType::kThreeQuarterPlayer,

			ShotType::kMaster, ShotType::kWide,
			ShotType::kDistant, ShotType::kTwoShot,
		};

		// ---- Lenses ------------------------------------------------------------
		//
		// Standard runs 50-72, Close 40-62, Room 45-95. Every setup a preset uses is
		// listed, even where it matches the table (a static_assert below enforces it).
		//
		// Distance is solved for the fill at the chosen lens and then floored at the
		// subject's minimum, so a tight fill on wide glass gets clamped and renders
		// looser than intended. Roughly: 66 degrees at 0.85 fill, 80 at 0.68, no limit
		// below 0.46. Over-the-shoulders are exempt.

		constexpr std::array kStandardLens{
			Lens{ ShotType::kOverPlayerShoulder, 55 }, Lens{ ShotType::kOverNpcShoulder, 55 },
			Lens{ ShotType::kCloseUp, 50 },            Lens{ ShotType::kClosePlayer, 50 },
			Lens{ ShotType::kMediumNpc, 62 },          Lens{ ShotType::kMediumPlayer, 62 },
			Lens{ ShotType::kThreeQuarterNpc, 58 },    Lens{ ShotType::kThreeQuarterPlayer, 58 },
			Lens{ ShotType::kTwoShot, 72 },            Lens{ ShotType::kProfile, 60 },
		};

		// Long lenses (35-50) throughout, which is what makes this look compressed
		// rather than just near. Keep the extremes at 66 or below (see the note
		// above). The distant shot shares the extreme close-up's lens, so it
		// compresses the room in the same way.
		constexpr std::array kCloseLens{
			Lens{ ShotType::kOverPlayerShoulder, 40 },
			Lens{ ShotType::kCloseUp, 50 },
			Lens{ ShotType::kExtremeClose, 40 },
			Lens{ ShotType::kClosePlayer, 40 },
			Lens{ ShotType::kExtremeClosePlayer, 35 },
			Lens{ ShotType::kDistant, 40 },
		};

		// The distant shot is the odd one out at 45: a long lens across a room reads
		// as watching from afar.
		constexpr std::array kRoomLens{
			Lens{ ShotType::kMaster, 95 },             Lens{ ShotType::kWide, 88 },
			Lens{ ShotType::kDistant, 45 },            Lens{ ShotType::kTwoShot, 80 },
			Lens{ ShotType::kLongNpc, 80 },            Lens{ ShotType::kLongPlayer, 80 },
			Lens{ ShotType::kOverhead, 85 },           Lens{ ShotType::kPlayerOverhead, 85 },
			Lens{ ShotType::kMediumNpc, 70 },          Lens{ ShotType::kMediumPlayer, 70 },
			Lens{ ShotType::kThreeQuarterNpc, 72 },    Lens{ ShotType::kThreeQuarterPlayer, 72 },
		};

		// ---- Weights -----------------------------------------------------------
		//
		// The table's weights are for the whole vocabulary and mean little once a
		// preset has cut the list down. Weight also decides how often the room appears
		// at all, since Coverage rolls the environmental pool's total against the
		// coverage pool's.

		constexpr std::array kStandardWeight{
			Weight{ ShotType::kOverPlayerShoulder, 100 }, Weight{ ShotType::kOverNpcShoulder, 100 },
			Weight{ ShotType::kCloseUp, 80 },             Weight{ ShotType::kClosePlayer, 80 },
			Weight{ ShotType::kThreeQuarterNpc, 60 },     Weight{ ShotType::kThreeQuarterPlayer, 60 },
			Weight{ ShotType::kMediumNpc, 45 },           Weight{ ShotType::kMediumPlayer, 45 },
			Weight{ ShotType::kTwoShot, 25 },             Weight{ ShotType::kProfile, 15 },
		};

		// The extremes outweigh the closes because they only compete on the few lines
		// with raised intensity (5-8% of authored responses). The room is at 10
		// against a coverage total of 250, so about one line in twenty-six goes wide.
		constexpr std::array kCloseWeight{
			Weight{ ShotType::kOverPlayerShoulder, 34 },
			Weight{ ShotType::kCloseUp, 36 },
			Weight{ ShotType::kExtremeClose, 75 },
			Weight{ ShotType::kClosePlayer, 30 },
			Weight{ ShotType::kExtremeClosePlayer, 75 },
			Weight{ ShotType::kDistant, 10 },
		};

		// The room outweighs the faces, which is the point of this look.
		constexpr std::array kRoomWeight{
			Weight{ ShotType::kMaster, 85 },            Weight{ ShotType::kWide, 80 },
			Weight{ ShotType::kLongNpc, 70 },           Weight{ ShotType::kLongPlayer, 70 },
			Weight{ ShotType::kDistant, 55 },           Weight{ ShotType::kTwoShot, 45 },
			Weight{ ShotType::kOverhead, 40 },          Weight{ ShotType::kPlayerOverhead, 40 },
			Weight{ ShotType::kThreeQuarterNpc, 40 },   Weight{ ShotType::kThreeQuarterPlayer, 40 },
			Weight{ ShotType::kMediumNpc, 35 },         Weight{ ShotType::kMediumPlayer, 35 },
		};

		// ---- Moves -------------------------------------------------------------
		//
		// Each preset gives a baseline and lists its exceptions; anything unlisted
		// gets the baseline.

		// Locked off, except both close-ups, which creep (so the pair still match).
		constexpr std::array kStandardMotion{
			Motion{ ShotType::kCloseUp, Move::kPushIn, 14, 500 },
			Motion{ ShotType::kClosePlayer, Move::kPushIn, 14, 500 },
			Motion{ ShotType::kTwoShot, Move::kDrift, 10, 700 },
		};

		// Five of the six setups move, each differently. The shoulder shot stays still
		// because it's the one that places the two of you. The NPC single creeps in,
		// the extreme zooms without moving, the reverse pulls its lens back, the
		// player's extreme pushes in hard, and the one wide pulls out.
		//
		// The locked setup stores amount 0 and time 400, like any shot with no move.
		// Nothing uses them, but preset comparison reads them, so they're stated.
		constexpr std::array kCloseMotion{
			Motion{ ShotType::kOverPlayerShoulder, Move::kLocked, 0, 400 },
			Motion{ ShotType::kCloseUp, Move::kPushIn, 40, 800 },
			Motion{ ShotType::kExtremeClose, Move::kZoomIn, 40, 900 },
			Motion{ ShotType::kClosePlayer, Move::kZoomOut, 20, 500 },
			Motion{ ShotType::kExtremeClosePlayer, Move::kPushIn, 75, 700 },
			Motion{ ShotType::kDistant, Move::kPullOut, 55, 800 },
		};

		// The wides move and the singles don't (the opposite of Standard), which makes
		// the room feel like the subject.
		constexpr std::array kRoomMotion{
			Motion{ ShotType::kMaster, Move::kCraneUp, 40, 900 },
			Motion{ ShotType::kWide, Move::kPullOut, 16, 800 },
			Motion{ ShotType::kLongNpc, Move::kCraneUp, 25, 700 },
			Motion{ ShotType::kLongPlayer, Move::kCraneUp, 25, 700 },
			Motion{ ShotType::kTwoShot, Move::kDrift, 12, 800 },
		};

		[[nodiscard]] constexpr bool Names(std::span<const ShotType> a_shots, ShotType a_type)
		{
			for (const auto shot : a_shots) {
				if (shot == a_type) {
					return true;
				}
			}
			return false;
		}

		// Every setup in a preset's list must have a lens and a weight entry;
		// otherwise it falls back to the table's values, which are usually wrong for
		// the look.
		template <typename T>
		[[nodiscard]] constexpr bool EveryShotIsNamed(
			std::span<const ShotType> a_shots, std::span<const T> a_entries)
		{
			for (const auto shot : a_shots) {
				bool found = false;
				for (const auto& entry : a_entries) {
					if (entry.shot == shot) {
						found = true;
						break;
					}
				}
				if (!found) {
					return false;
				}
			}
			return true;
		}

		// An exception naming a setup the preset doesn't use would do nothing while
		// looking like intent.
		template <typename T>
		[[nodiscard]] constexpr bool AllNamedAreUsed(
			std::span<const ShotType> a_shots, std::span<const T> a_entries)
		{
			for (const auto& entry : a_entries) {
				if (!Names(a_shots, entry.shot)) {
					return false;
				}
			}
			return true;
		}

		// ---- Lighting ----------------------------------------------------------
		//
		// Anything not listed uses the setup's own default, so a preset only lists
		// where it disagrees with the shot table. Standard lists nothing: it is the
		// look the defaults describe.
		constexpr std::array<Light, 0> kStandardLight{};

		// Close is warm and soft throughout; in a look this tight the light on the
		// face carries the mood. The extremes stay Hard, since they only appear on
		// high-intensity lines.
		constexpr std::array kCloseLight{
			Light{ ShotType::kOverPlayerShoulder, "soft" },
			Light{ ShotType::kCloseUp, "soft" },
			Light{ ShotType::kClosePlayer, "soft" },
			Light{ ShotType::kExtremeClose, "hard" },
			Light{ ShotType::kExtremeClosePlayer, "hard" },

			// Nothing on the one wide: a key from across a room just lights a patch of
			// floor.
			Light{ ShotType::kDistant, "off" },
		};

		// Room lights almost nothing; the space is already lit. Only the two setups
		// where a face is still big enough get anything.
		constexpr std::array kRoomLight{
			Light{ ShotType::kMaster, "off" },
			Light{ ShotType::kWide, "off" },
			Light{ ShotType::kDistant, "off" },
			Light{ ShotType::kOverhead, "off" },
			Light{ ShotType::kPlayerOverhead, "off" },
			Light{ ShotType::kLongNpc, "natural" },
			Light{ ShotType::kLongPlayer, "natural" },
			Light{ ShotType::kTwoShot, "natural" },
			Light{ ShotType::kMediumNpc, "natural" },
			Light{ ShotType::kMediumPlayer, "natural" },
			Light{ ShotType::kThreeQuarterNpc, "natural" },
			Light{ ShotType::kThreeQuarterPlayer, "natural" },
		};

		// The Close style is the mod's defaults. Camera::Tunables' initializers,
		// Director::ReadTuning's fallbacks and config/SD.ini must all match it, or the
		// Presets page opens with nothing selected on a fresh install.
		//
		//                            min   max  cutMin cutMax  perLine short  spk    choose
		constexpr Style kCloseStyle{ 240, 900, 3, 6, true, true, false, false };

		constexpr std::array kAll{
			Preset{ "standard", "Standard",
				"Over the shoulder, then close on whoever is talking.",
				"Ten angles, mostly locked off. Both close-ups creep slowly forward "
				"and the shot of the pair drifts; everything else is on sticks.",
				Style{ 240, 800, 2, 3, true, true, false, false },
				kStandard,
				Move::kLocked, 0, 400, kStandardMotion, kStandardLens, kStandardWeight, kStandardLight },

			Preset{ "close", "Close",
				"Tight on faces, and never far from one.",
				"Six angles: a shoulder to place you, a close single and an extreme "
				"on each side, and one shot from across the room, drawn rarely.",
				kCloseStyle,
				kClose,
				Move::kLocked, 0, 400, kCloseMotion, kCloseLens, kCloseWeight, kCloseLight },

			Preset{ "room", "Room",
				"Where you are, more than who is talking.",
				"Masters, wides and full figures on very wide glass, with cranes on "
				"the biggest of them. Holds each angle longer and cuts less often.",
				Style{ 300, 1000, 3, 5, true, true, false, false },
				kRoom,
				Move::kLocked, 0, 600, kRoomMotion, kRoomLens, kRoomWeight, kRoomLight },
		};

		// Index of "close" in kAll, checked below so reordering the table can't
		// silently change the default.
		constexpr std::size_t kDefaultPresetIndex = 1;
		static_assert(kDefaultPresetIndex < kAll.size() &&
				std::string_view{ kAll[kDefaultPresetIndex].key } == "close",
			"DefaultPreset must be Close: the shipped ini, the C++ fallbacks and the "
			"menu reset are all written to agree with it.");

		// And its style must be the one above.
		static_assert(kAll[kDefaultPresetIndex].style.cutEveryMin == kCloseStyle.cutEveryMin &&
				kAll[kDefaultPresetIndex].style.cutEveryMax == kCloseStyle.cutEveryMax &&
				kAll[kDefaultPresetIndex].style.perLineAngleChange &&
				kAll[kDefaultPresetIndex].style.holdOnShortLines &&
				!kAll[kDefaultPresetIndex].style.timedCutsWhileSpeaking &&
				!kAll[kDefaultPresetIndex].style.timedCutsWhileChoosing,
			"The shipped defaults are per-line angle changes at 3-6 lines, short lines "
			"ignored, and both timers off.");

		static_assert(EveryShotIsNamed<Lens>(kStandard, kStandardLens) &&
				EveryShotIsNamed<Lens>(kClose, kCloseLens) &&
				EveryShotIsNamed<Lens>(kRoom, kRoomLens),
			"Every setup a preset shoots with needs its lens named, or it silently "
			"takes the shot table's default in the middle of a look that does not "
			"want it.");

		static_assert(EveryShotIsNamed<Weight>(kStandard, kStandardWeight) &&
				EveryShotIsNamed<Weight>(kClose, kCloseWeight) &&
				EveryShotIsNamed<Weight>(kRoom, kRoomWeight),
			"Every setup a preset shoots with needs its how-often named, or it "
			"inherits a staple-or-accent tier authored against all thirty-nine "
			"setups and meaningless inside a list of ten.");

		static_assert(AllNamedAreUsed<Lens>(kStandard, kStandardLens) &&
				AllNamedAreUsed<Lens>(kClose, kCloseLens) &&
				AllNamedAreUsed<Lens>(kRoom, kRoomLens),
			"A preset names a lens for a setup it does not use. Nothing will read "
			"it.");

		static_assert(AllNamedAreUsed<Weight>(kStandard, kStandardWeight) &&
				AllNamedAreUsed<Weight>(kClose, kCloseWeight) &&
				AllNamedAreUsed<Weight>(kRoom, kRoomWeight),
			"A preset names a how-often for a setup it does not use. Nothing will "
			"read it.");

		static_assert(AllNamedAreUsed<Motion>(kStandard, kStandardMotion) &&
				AllNamedAreUsed<Motion>(kClose, kCloseMotion) &&
				AllNamedAreUsed<Motion>(kRoom, kRoomMotion),
			"A preset names a move for a setup it does not use. Nothing will read "
			"it.");

		// A preset writes the whole shot table, not just its own list; otherwise
		// applying a narrow preset after a broad one would leave the broad one's
		// extras enabled.
		[[nodiscard]] bool InList(std::span<const ShotType> a_shots, ShotType a_type)
		{
			for (const auto shot : a_shots) {
				if (shot == a_type) {
					return true;
				}
			}
			return false;
		}

		// What this preset wants this shot doing: its own exception if it has one,
		// otherwise the baseline. Shared by apply and drift so they can't disagree.
		[[nodiscard]] Motion MotionFor(const Preset& a_preset, ShotType a_type)
		{
			return PresetMotion(a_preset, a_type);
		}
	}

	Motion PresetMotion(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& m : a_preset.motion) {
			if (m.shot == a_type) {
				return m;
			}
		}
		return Motion{ a_type, a_preset.baseMove, a_preset.baseAmount, a_preset.baseTime };
	}

	// Falls back to the table, not a per-preset baseline lens; a single lens for
	// every setup would flatten the difference between a close-up and a master.
	int PresetLens(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& l : a_preset.lenses) {
			if (l.shot == a_type) {
				return std::clamp(l.degrees, kMinLens, kMaxLens);
			}
		}
		return static_cast<int>(AuthoredLens(a_type));
	}

	int PresetWeight(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& w : a_preset.weights) {
			if (w.shot == a_type) {
				return std::clamp(w.weight, 0, 100);
			}
		}
		return AuthoredWeight(a_type);
	}

	const char* PresetLight(const Preset& a_preset, ShotType a_type)
	{
		for (const auto& l : a_preset.lights) {
			if (l.shot == a_type && l.look && *l.look) {
				return l.look;
			}
		}
		return AuthoredLight(a_type);
	}
	std::span<const Preset> AllPresets()
	{
		return kAll;
	}

	const Preset& DefaultPreset()
	{
		return kAll[kDefaultPresetIndex];
	}

	bool PresetUses(const Preset& a_preset, ShotType a_type)
	{
		return InList(a_preset.shots, a_type);
	}

	const Preset* FindPreset(std::string_view a_key)
	{
		for (const auto& preset : kAll) {
			if (a_key == preset.key) {
				return &preset;
			}
		}
		return nullptr;
	}

	// Compared against live state, not the ini. Reading the ini meant a profile
	// read per value (each a hooked file operation under MO2), which made the
	// Presets page sluggish. Live state is also what the camera actually runs on.
	int PresetDrift(const Preset& a_preset)
	{
		const auto& s = a_preset.style;
		const auto  live = Director::GetTunables();
		int         drift = 0;

		const auto differs = [&drift](auto a_have, auto a_want) {
			if (a_have != a_want) {
				++drift;
			}
		};

		differs(live.minShotTime, s.minShotTime);
		differs(live.maxShotTime, s.maxShotTime);
		differs(live.cutEveryMin, s.cutEveryMin);
		differs(live.cutEveryMax, s.cutEveryMax);
		differs(live.perLineAngleChange, s.perLineAngleChange);
		differs(live.holdOnShortLines, s.holdOnShortLines);
		differs(live.timedCutsWhileSpeaking, s.timedCutsWhileSpeaking);
		differs(live.timedCutsWhileChoosing, s.timedCutsWhileChoosing);

		for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
			const auto type = static_cast<ShotType>(i);
			const bool want = InList(a_preset.shots, type);
			differs(Shot::Enabled(type), want);

			// A shot the preset doesn't use has no opinion about how it moves; counting it
			// would report drift nobody can see.
			if (!want) {
				continue;
			}

			// The move counts, or a preset would read as unchanged after its moves were
			// retuned.
			const auto wanted = MotionFor(a_preset, type);
			differs(Shot::MoveOf(type), wanted.move);
			differs(Shot::MoveAmount(type), wanted.amount);
			differs(Shot::MoveTime(type), wanted.time);

			// Same for the lens, which is where these looks differ most.
			differs(Shot::Lens(type), PresetLens(a_preset, type));

			// And the weight: a look with its room shots at zero isn't that look.
			differs(Shot::Weight(type), PresetWeight(a_preset, type));

			// And the lighting, compared as indices (what's live) so spelling differences
			// don't count.
			differs(Shot::LightOf(type), Scene::FindLook(PresetLight(a_preset, type)));
		}

		return drift;
	}

	const Preset* ActivePreset()
	{
		for (const auto& preset : kAll) {
			if (PresetDrift(preset) == 0) {
				return &preset;
			}
		}
		return nullptr;
	}

	void ApplyPreset(const Preset& a_preset)
	{
		const auto& s = a_preset.style;

		Config::SetInt("Direction", "iMinShotTime", s.minShotTime);
		Config::SetInt("Direction", "iMaxShotTime", s.maxShotTime);
		Config::SetInt("Direction", "iCutEveryMin", s.cutEveryMin);
		Config::SetInt("Direction", "iCutEveryMax", s.cutEveryMax);
		Config::SetBool("Direction", "bPerLineAngleChange", s.perLineAngleChange);

		// bCoverPlayerTurn isn't a style and isn't written by presets.
		Config::SetBool("Direction", "bHoldOnShortLines", s.holdOnShortLines);
		Config::SetBool("Direction", "bTimedCutsWhileSpeaking", s.timedCutsWhileSpeaking);
		Config::SetBool("Direction", "bTimedCutsWhileChoosing", s.timedCutsWhileChoosing);

		int on = 0;
		int moving = 0;
		int narrowest = kMaxLens;
		int widest = kMinLens;
		for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
			const auto type = static_cast<ShotType>(i);
			const bool want = InList(a_preset.shots, type);
			Config::SetBool("Shots", Key(type), want);
			Shot::SetEnabled(type, want);
			if (!want) {
				continue;
			}
			++on;

			// Moves are written only for setups the preset uses, so applying it doesn't
			// overwrite tuning on switched-off shots.
			const auto m = MotionFor(a_preset, type);
			Config::SetInt("Shots", MoveKey(type), static_cast<int>(m.move));
			Config::SetInt("Shots", MoveAmountKey(type), m.amount);
			Config::SetInt("Shots", MoveTimeKey(type), m.time);
			Shot::SetMove(type, m.move);
			Shot::SetMoveAmount(type, m.amount);
			Shot::SetMoveTime(type, m.time);
			if (m.move != Move::kLocked && m.amount > 0) {
				++moving;
			}

			// Lenses are written for every setup the preset uses, including unnamed ones
			// (which get the table's lens back), so applying a look always gives the same
			// result.
			const int lens = PresetLens(a_preset, type);
			Config::SetInt("Shots", LensKey(type), lens);
			Shot::SetLens(type, lens);
			narrowest = std::min(narrowest, lens);
			widest = std::max(widest, lens);

			// And the weight, which also decides whether the camera ever leaves the two
			// faces for the room setups.
			const int weight = PresetWeight(a_preset, type);
			Config::SetInt("Shots", WeightKey(type), weight);
			Shot::SetWeight(type, weight);

			// And the lighting, written as a name and resolved right away so it applies to
			// the current conversation.
			const char* rig = PresetLight(a_preset, type);
			Config::SetString("Shots", LightKey(type), rig);
			const int rigIndex = Scene::FindLook(rig);
			Shot::SetLight(type, rigIndex >= 0 ? rigIndex : Scene::DefaultLook());
		}

		// Apply live as well as writing, so the preset takes effect in the current
		// conversation.
		Director::LoadSettings();

		// The lens range is in the log line because it's the part of a look that can't
		// be inferred from anything else.
		Log::Info(Log::Category::kCamera,
			"Preset '{}' applied: {} setup(s) on, {} of them moving, {}-{} degrees, "
			"shot {:.2f}-{:.2f}s."sv,
			a_preset.key, on, moving, narrowest, widest,
			static_cast<double>(s.minShotTime) / 100.0,
			static_cast<double>(s.maxShotTime) / 100.0);
	}

	// ---- Saved presets -----------------------------------------------------
	//
	// Each slot is one string rather than hundreds of keys, so SD.ini stays
	// readable. The name has its own key; the payload is one line that isn't meant
	// for hand-editing.
	//
	// Versioned: a shot added to the middle of the enum would shift every entry,
	// so the reader checks the version and refuses rather than applying a
	// scrambled set.
	namespace
	{
		// A slot has to hold everything a preset can write, or it can't reproduce the
		// look. Version 2 added the per-setup lens, version 3 the weight. Older
		// payloads still load; fields they don't carry are left as they are.
		constexpr int         kSlotVersion = 3;
		constexpr std::size_t kSlotStride = 6;  // enabled, move, amount, time, lens, weight

		// Values per setup for each version. Index is the version.
		constexpr std::array<std::size_t, 4> kStrideForVersion{ 0, 4, 5, kSlotStride };

		[[nodiscard]] const char* SlotNameKey(int a_index)
		{
			static std::array<std::string, kCustomSlots> cache;
			auto&                                       key = cache[static_cast<std::size_t>(a_index)];
			if (key.empty()) {
				key = "sSlot" + std::to_string(a_index + 1) + "Name";
			}
			return key.c_str();
		}

		[[nodiscard]] const char* SlotDataKey(int a_index)
		{
			static std::array<std::string, kCustomSlots> cache;
			auto&                                       key = cache[static_cast<std::size_t>(a_index)];
			if (key.empty()) {
				key = "sSlot" + std::to_string(a_index + 1);
			}
			return key.c_str();
		}

		// Lighting is saved in its own key, by name, one per setup in shot order. The
		// payload is integers, and storing a rig index would break when rigs are
		// added. A missing key means an older slot with no lighting, which is left
		// alone.
		[[nodiscard]] const char* SlotLightKey(int a_index)
		{
			static std::array<std::string, kCustomSlots> cache;
			auto&                                       key = cache[static_cast<std::size_t>(a_index)];
			if (key.empty()) {
				key = "sSlot" + std::to_string(a_index + 1) + "Lights";
			}
			return key.c_str();
		}

		[[nodiscard]] std::vector<std::string> SplitNames(std::string_view a_text, char a_delim)
		{
			std::vector<std::string> out;
			std::size_t              start = 0;
			while (start <= a_text.size()) {
				const auto end = a_text.find(a_delim, start);
				const auto piece = a_text.substr(start,
					end == std::string_view::npos ? std::string_view::npos : end - start);
				out.emplace_back(piece);
				if (end == std::string_view::npos) {
					break;
				}
				start = end + 1;
			}
			return out;
		}

		[[nodiscard]] bool ValidSlot(int a_index)
		{
			return a_index >= 0 && a_index < kCustomSlots;
		}

		// Everything a slot holds, from live state, as one string. Used both for
		// saving and for checking whether a slot is the one running, so the two can't
		// disagree.
		[[nodiscard]] std::string Snapshot()
		{
			const auto  live = Director::GetTunables();
			std::string data = std::to_string(kSlotVersion);

			const auto add = [&data](int a_value) {
				data += ',';
				data += std::to_string(a_value);
			};

			add(live.minShotTime);
			add(live.maxShotTime);
			add(live.cutEveryMin);
			add(live.cutEveryMax);
			// Slot field 5 held bCutOnLineEnd up to 1.3 and holds bPerLineAngleChange from
			// 1.4. Same offset and type, so the version didn't change.
			add(live.perLineAngleChange ? 1 : 0);
			add(live.holdOnShortLines ? 1 : 0);
			add(live.timedCutsWhileSpeaking ? 1 : 0);
			add(live.timedCutsWhileChoosing ? 1 : 0);

			// The shot count is stored so a truncated line can be told apart from one
			// written by a build with a different table.
			add(static_cast<int>(ShotType::kCount));

			for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
				const auto type = static_cast<ShotType>(i);
				add(Shot::Enabled(type) ? 1 : 0);
				add(static_cast<int>(Shot::MoveOf(type)));
				add(Shot::MoveAmount(type));
				add(Shot::MoveTime(type));
				add(Shot::Lens(type));
				add(Shot::Weight(type));
			}

			return data;
		}

		[[nodiscard]] std::vector<int> SplitInts(std::string_view a_text, char a_sep)
		{
			std::vector<int> out;
			int              value = 0;
			bool             any = false;
			bool             negative = false;
			for (const char c : a_text) {
				if (c == a_sep) {
					out.push_back(negative ? -value : value);
					value = 0;
					any = false;
					negative = false;
				} else if (c == '-' && !any) {
					negative = true;
				} else if (c >= '0' && c <= '9') {
					value = value * 10 + (c - '0');
					any = true;
				}
			}
			out.push_back(negative ? -value : value);
			return out;
		}
	}

	CustomSlot ReadCustomSlot(int a_index)
	{
		CustomSlot slot{};
		if (!ValidSlot(a_index)) {
			return slot;
		}

		slot.index = a_index;
		slot.name = Config::String("CustomPresets", SlotNameKey(a_index), "");
		slot.used = !slot.name.empty() &&
			!Config::String("CustomPresets", SlotDataKey(a_index), "").empty();
		return slot;
	}

	void SaveCustomSlot(int a_index, std::string_view a_name)
	{
		if (!ValidSlot(a_index)) {
			return;
		}

		const auto data = Snapshot();
		Config::SetString("CustomPresets", SlotDataKey(a_index), data.c_str());
		Config::SetString("CustomPresets", SlotNameKey(a_index), std::string{ a_name }.c_str());

		// The rigs, by name, in shot order. See SlotLightKey.
		std::string lights;
		const auto  rigs = Scene::AllLooks();
		for (std::size_t i = 0; i < static_cast<std::size_t>(ShotType::kCount); ++i) {
			if (i != 0) {
				lights += ',';
			}
			const auto type = static_cast<ShotType>(i);
			const int  rig = Shot::LightOf(type);
			lights += rig >= 0 && static_cast<std::size_t>(rig) < rigs.size() ?
						  rigs[static_cast<std::size_t>(rig)].key :
						  AuthoredLight(type);
		}
		Config::SetString("CustomPresets", SlotLightKey(a_index), lights.c_str());

		Log::Info(Log::Category::kCamera,
			"Saved your settings to slot {} as '{}'."sv, a_index + 1, a_name);
	}

	void ApplyCustomSlot(int a_index)
	{
		if (!ValidSlot(a_index)) {
			return;
		}

		const auto raw = Config::String("CustomPresets", SlotDataKey(a_index), "");
		if (raw.empty()) {
			return;
		}

		const auto values = SplitInts(raw, ',');

		// 1 version + 8 settings + 1 count, then a fixed run per shot.
		constexpr std::size_t kHeader = 10;
		const int  version = values.empty() ? 0 : values[0];
		const bool known = version > 0 &&
			static_cast<std::size_t>(version) < kStrideForVersion.size();
		if (values.size() < kHeader || !known) {
			Log::Warn(Log::Category::kCamera,
				"Slot {} was saved by a different version and cannot be applied."sv,
				a_index + 1);
			return;
		}

		const std::size_t stride = kStrideForVersion[static_cast<std::size_t>(version)];

		// Two separate failures with separate messages: a wrong shot count, or a value
		// that came back truncated by the config reader.
		const auto count = static_cast<std::size_t>(values[9]);
		if (count != static_cast<std::size_t>(ShotType::kCount)) {
			Log::Warn(Log::Category::kCamera,
				"Slot {} was saved with {} shots and this build has {}; not applied."sv,
				a_index + 1, count, static_cast<std::size_t>(ShotType::kCount));
			return;
		}

		const auto needed = kHeader + count * stride;
		if (values.size() < needed) {
			Log::Warn(Log::Category::kCamera,
				"Slot {} is short: {} value(s) read, {} expected, {} characters on disk. "
				"The line was truncated rather than mis-saved."sv,
				a_index + 1, values.size(), needed, raw.size());
			return;
		}

		Config::SetInt("Direction", "iMinShotTime", values[1]);
		Config::SetInt("Direction", "iMaxShotTime", values[2]);
		Config::SetInt("Direction", "iCutEveryMin", values[3]);
		Config::SetInt("Direction", "iCutEveryMax", values[4]);
		Config::SetBool("Direction", "bPerLineAngleChange", values[5] != 0);
		Config::SetBool("Direction", "bHoldOnShortLines", values[6] != 0);
		Config::SetBool("Direction", "bTimedCutsWhileSpeaking", values[7] != 0);
		Config::SetBool("Direction", "bTimedCutsWhileChoosing", values[8] != 0);

		// bCoverPlayerTurn isn't touched, as in ApplyPreset.

		int on = 0;
		for (std::size_t i = 0; i < count; ++i) {
			const auto type = static_cast<ShotType>(i);
			const auto base = kHeader + i * stride;

			const bool enabled = values[base] != 0;
			const int  move = std::clamp(values[base + 1], 0, static_cast<int>(Move::kCount) - 1);
			const int  amount = std::clamp(values[base + 2], 0, 100);
			const int  time = std::clamp(values[base + 3], 30, 900);

			Config::SetBool("Shots", Key(type), enabled);
			Config::SetInt("Shots", MoveKey(type), move);
			Config::SetInt("Shots", MoveAmountKey(type), amount);
			Config::SetInt("Shots", MoveTimeKey(type), time);

			Shot::SetEnabled(type, enabled);
			Shot::SetMove(type, static_cast<Move>(move));
			Shot::SetMoveAmount(type, amount);
			Shot::SetMoveTime(type, time);

			// Only the fields the payload actually carries; older slots leave the rest as
			// they are.
			if (stride > 4) {
				const int lens = std::clamp(values[base + 4], kMinLens, kMaxLens);
				Config::SetInt("Shots", LensKey(type), lens);
				Shot::SetLens(type, lens);
			}

			if (stride > 5) {
				const int weight = std::clamp(values[base + 5], 0, 100);
				Config::SetInt("Shots", WeightKey(type), weight);
				Shot::SetWeight(type, weight);
			}

			on += enabled ? 1 : 0;
		}

		// The rigs, if the slot has them. Only applied when the list length matches
		// this build, so rigs never land on the wrong setups by position.
		const auto rawLights = Config::String("CustomPresets", SlotLightKey(a_index), "");
		if (!rawLights.empty()) {
			const auto names = SplitNames(rawLights, ',');
			if (names.size() == static_cast<std::size_t>(ShotType::kCount)) {
				for (std::size_t i = 0; i < names.size(); ++i) {
					const auto type = static_cast<ShotType>(i);
					const int  rig = Scene::FindLook(names[i]);
					if (rig < 0) {
						continue;  // an unknown rig leaves that setup as it was
					}
					Config::SetString("Shots", LightKey(type), names[i].c_str());
					Shot::SetLight(type, rig);
				}
			} else {
				Log::Warn(Log::Category::kCamera,
					"Slot {} carries {} lighting rig(s) against this build's {}; lighting not applied."sv,
					a_index + 1, names.size(), static_cast<std::size_t>(ShotType::kCount));
			}
		}

		// Applied live, like a built-in preset.
		Director::LoadSettings();

		// A legacy slot re-saves itself once it's used. ActiveCustomSlot compares the
		// stored line against a fresh snapshot, which an older payload can never
		// match, so without this the slot would never show as in use.
		if (stride < kSlotStride) {
			SaveCustomSlot(a_index, Config::String("CustomPresets", SlotNameKey(a_index), ""));
			Log::Info(Log::Category::kCamera,
				"Slot {} was saved by an older build and has been re-saved, keeping "
				"whatever was live for the settings it did not carry."sv,
				a_index + 1);
		}

		Log::Info(Log::Category::kCamera,
			"Applied your slot {} ('{}'): {} setup(s) on."sv, a_index + 1,
			Config::String("CustomPresets", SlotNameKey(a_index), ""), on);
	}

	int ActiveCustomSlot()
	{
		const auto now = Snapshot();
		for (int i = 0; i < kCustomSlots; ++i) {
			const auto stored = Config::String("CustomPresets", SlotDataKey(i), "");
			if (!stored.empty() && stored == now &&
				!Config::String("CustomPresets", SlotNameKey(i), "").empty()) {
				return i;
			}
		}
		return -1;
	}

	void RenameCustomSlot(int a_index, std::string_view a_name)
	{
		if (!ValidSlot(a_index)) {
			return;
		}
		Config::SetString("CustomPresets", SlotNameKey(a_index), std::string{ a_name }.c_str());
	}

	void DeleteCustomSlot(int a_index)
	{
		if (!ValidSlot(a_index)) {
			return;
		}
		// Clear all three keys; otherwise an empty name or a leftover lighting list
		// would carry into whatever is saved into the slot next.
		Config::SetString("CustomPresets", SlotDataKey(a_index), "");
		Config::SetString("CustomPresets", SlotNameKey(a_index), "");
		Config::SetString("CustomPresets", SlotLightKey(a_index), "");
		Log::Info(Log::Category::kCamera, "Cleared your slot {}."sv, a_index + 1);
	}

	void ApplyPendingPreset()
	{
		const auto wanted = Config::String("Presets", "sApply", "");
		if (wanted.empty()) {
			return;
		}

		// Built-ins first, then installed presets by name or file name.
		if (const auto* preset = FindPreset(wanted)) {
			ApplyPreset(*preset);
		} else {
			RescanPresetFiles(true);
			if (const auto* file = FindInstalledPreset(wanted)) {
				ApplyInstalledPreset(*file);
			} else {
				Log::Warn(Log::Category::kCamera,
					"[Presets] sApply names '{}', which is not a built-in or installed preset. Ignored."sv, wanted);
			}
		}

		// Cleared so it applies once; otherwise it would overwrite the player's tuning
		// on every launch.
		Config::SetString("Presets", "sApply", "");
	}
}

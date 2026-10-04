#include "SD/Camera/Anatomy.h"

#include "SD/Core/Logging.h"

namespace SD::Camera
{
	namespace
	{
		// The humanoid baseline the shot table was written against; each value is
		// multiplied by the measured scale.
		constexpr float kHumanExtent = 42.0f;
		constexpr float kHumanMinDistance = 68.0f;
		constexpr float kHumanProbeStart = 48.0f;
		constexpr float kHumanEyeHeight = 120.0f;

		// Fallback reference radius, only used when the player has no body bound
		// (normally the player is the reference; see Fit). Close to measured player
		// values.
		constexpr float kFallbackRadius = 75.0f;

		// The body bound includes everything an actor wears and carries, so normal
		// people vary by up to about 1.4x from clothing alone. Anything inside this
		// band is treated as exactly person-sized; only clear outliers (a dragon is
		// around 8.8x) are scaled.
		constexpr float kPersonBandLow = 0.60f;
		constexpr float kPersonBandHigh = 1.85f;

		// Head size grows much slower than body size. A dragon's bound is about 8.8x a
		// person's but its head is more like 3-4x, so framing uses bulk raised to this
		// power (8.8x becomes 3.6x). Calibrated on one dragon; the log prints both
		// values so it can be checked.
		constexpr float kFrameExponent = 0.6f;

		// Beyond these a subject isn't person-sized. Outside the person band on both
		// sides by construction.
		constexpr float kLargeBulk = 2.2f;
		constexpr float kSmallBulk = 0.5f;

		// A bad measurement should give an odd shot, not a camera thousands of units
		// away.
		constexpr float kMinScale = 0.15f;
		constexpr float kMaxScale = 8.0f;

		// Exact head node names, tried before searching. The first is the humanoid
		// convention; the rest are common creature skeleton names. The substring
		// search below handles the rest.
		constexpr std::array kHeadNodes{
			"NPC Head [Head]"sv,
			"NPC Head"sv,
			"Head"sv,
			"HEAD"sv,
			"NPCHead"sv,
			"Head [Head]"sv,
		};

		// A rig with an arm has a shoulder to shoot over. Asked of the skeleton rather
		// than inferred from size.
		constexpr std::array kArmNodes{
			"NPC L UpperArm [LUar]"sv,
			"NPC R UpperArm [RUar]"sv,
			"NPC L Clavicle [LClv]"sv,
		};

		// Logged for the first few actors of a session only.
		constexpr int kReportBudget = 12;
		int           reported{ 0 };

		// ASCII only on purpose: node names are ASCII, and std::tolower is
		// locale-dependent (a Turkish locale breaks "Head" vs "head").
		[[nodiscard]] constexpr char Lower(char a_c) noexcept
		{
			return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c - 'A' + 'a') : a_c;
		}

		[[nodiscard]] bool ContainsNoCase(std::string_view a_haystack, std::string_view a_needle)
		{
			if (a_needle.empty() || a_haystack.size() < a_needle.size()) {
				return false;
			}
			const auto limit = a_haystack.size() - a_needle.size();
			for (std::size_t i = 0; i <= limit; ++i) {
				bool match = true;
				for (std::size_t j = 0; j < a_needle.size(); ++j) {
					if (Lower(a_haystack[i + j]) != Lower(a_needle[j])) {
						match = false;
						break;
					}
				}
				if (match) {
					return true;
				}
			}
			return false;
		}

		// The highest node whose name looks like a head. Skeletons have several
		// candidates (neck, head-tracking marker, helmet attachment), and on upright
		// bodies the head is the top one. The head-tracking node is excluded by name.
		void FindHead(RE::NiAVObject* a_object, RE::NiAVObject*& a_best, float& a_bestZ, int a_depth)
		{
			if (!a_object || a_depth > 12) {
				return;
			}

			if (const char* raw = a_object->name.c_str()) {
				const std::string_view name{ raw };
				if (ContainsNoCase(name, "head"sv) && !ContainsNoCase(name, "headtrack"sv)) {
					const float z = a_object->world.translate.z;
					if (!a_best || z > a_bestZ) {
						a_best = a_object;
						a_bestZ = z;
					}
				}
			}

			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					FindHead(child.get(), a_best, a_bestZ, a_depth + 1);
				}
			}
		}
	}

	BuildTuning Tuning(Build a_build) noexcept
	{
		switch (a_build) {
		// Dragons, giants, mammoths. fillCap 0.50 keeps the whole head in frame;
		// riseScale 0.30 keeps overheads under indoor ceilings; angleBias 16 moves off
		// the snout to a three-quarter view.
		case Build::kLarge: return { 0.50f, 0.30f, 16.0f };

		// Wolves, bears, sabre cats: the same long-head issue at lower strength.
		// They're close to person-sized, so rise is nearly right as authored.
		case Build::kBeast: return { 0.68f, 0.80f, 10.0f };

		// Chickens, rabbits, skeevers. Tight framing is fine.
		case Build::kSmall: return { 0.90f, 1.00f, 0.0f };

		// Must stay unchanged: every ordinary conversation uses this, and the shot
		// table was written against these values.
		case Build::kHumanoid:
		default: return { 1.00f, 1.00f, 0.0f };
		}
	}

	std::string_view Name(Build a_build) noexcept
	{
		switch (a_build) {
		case Build::kHumanoid: return "humanoid"sv;
		case Build::kBeast:    return "beast"sv;
		case Build::kLarge:    return "large"sv;
		case Build::kSmall:    return "small"sv;
		default:               return "?"sv;
		}
	}

	Anatomy Measure(RE::Actor* a_actor)
	{
		Anatomy out{};

		auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
		if (!root) {
			// Humanoid defaults, unmeasured. Better than refusing to stage.
			return out;
		}

		const float rootZ = root->world.translate.z;

		// Named nodes first; one lookup is cheaper than walking the tree.
		RE::NiAVObject* head = nullptr;
		std::string_view via = "search"sv;
		for (const auto& name : kHeadNodes) {
			if (auto* node = root->GetObjectByName(name)) {
				head = node;
				via = name;
				break;
			}
		}

		if (!head) {
			float bestZ = 0.0f;
			FindHead(root, head, bestZ, 0);
		}

		if (head) {
			// Clamped much wider than a person's range, since a dragon's head is far
			// higher and a chicken's far lower.
			const auto& hp = head->world.translate;
			out.eyeHeight = std::clamp(hp.z - rootZ, 6.0f, 900.0f);

			// The horizontal offset, moved into the actor's own frame so it survives
			// turning. Near zero on anything upright.
			const float heading = a_actor->GetAngleZ();
			const float c = std::cos(-heading);
			const float s = std::sin(-heading);
			const float dx = hp.x - root->world.translate.x;
			const float dy = hp.y - root->world.translate.y;
			out.headOffset = {
				dx * c - dy * s,
				dx * s + dy * c,
				out.eyeHeight
			};

			out.headRadius = head->worldBound.radius;
		}

		out.radius = root->worldBound.radius;

		// Whether the rig has an arm. Separate from size; a giant has arms but isn't
		// person-sized.
		out.rigged = false;
		for (const auto& name : kArmNodes) {
			if (root->GetObjectByName(name)) {
				out.rigged = true;
				break;
			}
		}

		out.head = head != nullptr;
		out.via = via;
		out.name = a_actor->GetName() ? a_actor->GetName() : "?";
		out.measured = true;
		return out;
	}

	void Fit(Anatomy& a_body, float a_referenceRadius)
	{
		const bool  fallback = !(a_referenceRadius > 1.0f);
		const float reference = fallback ? kFallbackRadius : a_referenceRadius;

		const float raw = a_body.radius > 1.0f ? a_body.radius / reference : 1.0f;
		a_body.bulk = (raw > kPersonBandLow && raw < kPersonBandHigh) ?
			1.0f :
			std::clamp(raw, kMinScale, kMaxScale);

		// Bulk classifies; its fractional power frames. See kFrameExponent.
		a_body.scale = a_body.bulk == 1.0f ? 1.0f : std::pow(a_body.bulk, kFrameExponent);

		a_body.extent = kHumanExtent * a_body.scale;
		a_body.minDistance = kHumanMinDistance * a_body.scale;
		a_body.probeStart = kHumanProbeStart * a_body.scale;

		// Size outranks the skeleton: a giant has a shoulder, but shooting over it
		// would put the camera thirty feet up. Uses the raw bulk ratio, since this is
		// a classification.
		if (a_body.bulk >= kLargeBulk) {
			a_body.build = Build::kLarge;
		} else if (a_body.bulk <= kSmallBulk) {
			a_body.build = Build::kSmall;
		} else if (a_body.rigged && a_body.head) {
			a_body.build = Build::kHumanoid;
		} else {
			a_body.build = Build::kBeast;
		}

		// Only a humanoid rig at humanoid size has a shoulder to shoot over.
		a_body.shoulder = a_body.rigged && a_body.build == Build::kHumanoid;

		if (reported < kReportBudget) {
			++reported;
			Log::Info(Log::Category::kStaging,
				"Body {}: {} | body r{:.0f} / ref {:.0f}{} -> bulk {:.2f} frame {:.2f} | "
				"head via {} at +{:.0f},{:.0f} up {:.0f} (bone r{:.1f}, always 0) | "
				"extent {:.0f} floor {:.0f} probe {:.0f} | fillCap {:.2f} rise x{:.2f} angle +{:.0f} | shoulder {}"sv,
				a_body.name, Name(a_body.build), a_body.radius, reference,
				fallback ? " (ESTIMATE - player unmeasurable)"sv : ""sv,
				a_body.bulk, a_body.scale, a_body.via,
				a_body.headOffset.x, a_body.headOffset.y, a_body.eyeHeight, a_body.headRadius,
				a_body.extent, a_body.minDistance, a_body.probeStart,
				Tuning(a_body.build).fillCap, Tuning(a_body.build).riseScale,
				Tuning(a_body.build).angleBias,
				a_body.shoulder ? "yes"sv : "no"sv);
		}
	}
}

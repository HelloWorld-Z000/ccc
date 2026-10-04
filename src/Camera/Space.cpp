#include "SD/Camera/Space.h"

#include "SD/Core/Logging.h"

namespace SD::Camera
{
	namespace
	{
		Log::OnceFlag firstProbe;

		[[nodiscard]] RE::bhkWorld* World()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* cell = player ? player->GetParentCell() : nullptr;
			return cell ? cell->GetbhkWorld() : nullptr;
		}

		[[nodiscard]] float Length(const RE::NiPoint3& a_v)
		{
			return std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z);
		}

		[[nodiscard]] SightGeometry::Point G(const RE::NiPoint3& a_p) { return { a_p.x, a_p.y, a_p.z }; }
		[[nodiscard]] RE::NiPoint3 P(SightGeometry::Point a_p) { return { a_p.x, a_p.y, a_p.z }; }

		[[nodiscard]] Probe CastWorld(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			Probe probe{};

			if (!a_world || !SightGeometry::Finite(G(a_from)) || !SightGeometry::Finite(G(a_to))) {
				return probe;
			}

			const RE::NiPoint3 delta{ a_to.x - a_from.x, a_to.y - a_from.y, a_to.z - a_from.z };
			const float        full = Length(delta);
			if (!std::isfinite(full) || !(full > 0.01f)) {
				return probe;
			}

			const float havokScale = RE::bhkWorld::GetWorldScale();
			if (!std::isfinite(havokScale) || havokScale <= 0.0f) {
				return probe;
			}
			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4{ a_from.x * havokScale, a_from.y * havokScale, a_from.z * havokScale, 0.0f };
			pick.rayInput.to = RE::hkVector4{ a_to.x * havokScale, a_to.y * havokScale, a_to.z * havokScale, 0.0f };

			// Keep the established LOS filter. Actor visibility is also tested with narrow
			// capsules; the collision-layer behaviour still needs checking in game with
			// installed skeleton and collision mods.
			pick.rayInput.filterInfo.filter = static_cast<std::uint32_t>(RE::COL_LAYER::kLOS) | (1u << 16);

			// Keep the established main-thread PickObject path. Its bool return isn't a
			// documented success signal, so the exposed failure flag is used.
			a_world->PickObject(pick);
			if (pick.pickFailed || !std::isfinite(pick.rayOutput.hitFraction) ||
				pick.rayOutput.hitFraction < 0.0f || pick.rayOutput.hitFraction > 1.0f) {
				return probe;
			}

			probe.valid = true;
			probe.hit = pick.rayOutput.HasHit();
			probe.distance = probe.hit ? full * pick.rayOutput.hitFraction : full;

			if (firstProbe.Take()) {
				Log::Info(Log::Category::kStaging,
					"First space probe: ray {:.1f}u, {} at {:.1f}u. A distance wildly unlike the ray length means the Havok scale is wrong."sv,
					full, probe.hit ? "hit"sv : "clear"sv, probe.distance);
			}

			return probe;
		}

		[[nodiscard]] bool ContainsHead(std::string_view a_name)
		{
			std::string lower{ a_name };
			for (auto& c : lower) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return lower.find("head") != std::string::npos && lower.find("headtrack") == std::string::npos;
		}

		void FindHead(RE::NiAVObject* a_object, RE::NiAVObject*& a_best, int a_depth)
		{
			if (!a_object || a_depth > 12) {
				return;
			}
			if (const auto* name = a_object->name.c_str(); name && ContainsHead(name) &&
				SightGeometry::Finite(G(a_object->world.translate)) &&
				(!a_best || a_object->world.translate.z > a_best->world.translate.z)) {
				a_best = a_object;
			}
			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					FindHead(child.get(), a_best, a_depth + 1);
				}
			}
		}

		[[nodiscard]] RE::NiAVObject* Head(RE::NiAVObject* a_root)
		{
			if (!a_root) {
				return nullptr;
			}
			constexpr std::array names{ "NPC Head [Head]"sv, "NPC Head"sv, "Head"sv, "HEAD"sv, "NPCHead"sv, "Head [Head]"sv };
			for (const auto name : names) {
				if (auto* node = a_root->GetObjectByName(name)) {
					return node;
				}
			}
			RE::NiAVObject* head = nullptr;
			FindHead(a_root, head, 0);
			return head;
		}

		[[nodiscard]] SightState ActorsAlong(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
			const SightContext& a_context, RE::FormID a_subject, float a_padding = 0.0f)
		{
			bool unknown = !a_context.valid;
			for (const auto& actor : a_context.actors) {
				if (a_subject != 0 && actor.id == a_subject) {
					continue;
				}
				if (actor.count == 0) {
					// Missing skeleton data is uncertain only where its broad bound overlaps the
					// segment; that bound is never treated as a blocker.
					unknown |= SightGeometry::SegmentHitsCapsule(G(a_from), G(a_to), G(actor.boundCenter),
						G(actor.boundCenter), actor.boundRadius + a_padding);
					continue;
				}
				for (std::size_t i = 0; i < actor.count; ++i) {
					const auto& capsule = actor.capsules[i];
					if (SightGeometry::SegmentHitsCapsule(G(a_from), G(a_to), G(capsule.from),
						G(capsule.to), capsule.radius + a_padding)) {
						return SightState::kBlocked;
					}
				}
			}
			return unknown ? SightState::kUnknown : SightState::kClear;
		}

		[[nodiscard]] SightState SampleSight(const RE::NiPoint3& a_sample, const RE::NiPoint3& a_camera,
			RE::FormID a_subject, const SightContext& a_context)
		{
			const auto actors = ActorsAlong(a_sample, a_camera, a_context, a_subject);
			if (actors == SightState::kBlocked) {
				return actors;
			}
			const auto probe = CastWorld(a_context.world, a_sample, a_camera);
			if (probe.valid && probe.hit) {
				return SightState::kBlocked;
			}
			return probe.valid && actors == SightState::kClear ? SightState::kClear : SightState::kUnknown;
		}
	}

	Probe Cast(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		return CastWorld(World(), a_from, a_to);
	}

	SightContext BuildSightContext()
	{
		SightContext context{};
		context.world = World();
		auto* lists = RE::ProcessLists::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		context.valid = lists != nullptr && player != nullptr;
		if (!context.valid) {
			return context;
		}
		context.actors.reserve(lists->highActorHandles.size() + 1);
		const auto* player3D = player->Get3D(false);
		const float measuredReference = player3D ? player3D->worldBound.radius : 0.0f;
		const float reference = std::isfinite(measuredReference) && measuredReference > 1.0f ? measuredReference : 75.0f;

		const auto append = [&](RE::Actor* a_actor) {
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return;  // unloaded geometry cannot cover rendered pixels
			}
			const auto id = a_actor->GetFormID();
			if (std::any_of(context.actors.begin(), context.actors.end(), [id](const auto& a_entry) { return a_entry.id == id; })) {
				return;
			}
			SightActor entry{};
			entry.id = id;
			entry.boundCenter = root->worldBound.center;
			entry.boundRadius = root->worldBound.radius;
			if (!SightGeometry::Finite(G(entry.boundCenter)) || !std::isfinite(entry.boundRadius) || entry.boundRadius <= 0.0f) {
				context.valid = false;
				return;
			}
			auto* head = Head(root);
			if (head && SightGeometry::Finite(G(head->world.translate))) {
				// Same wardrobe deadband as Anatomy. Bone bounds are zero and never used as a
				// head size.
				const float bulk = entry.boundRadius / reference;
				const float scale = bulk > 0.60f && bulk < 1.85f ? 1.0f : std::pow(std::clamp(bulk, 0.15f, 8.0f), 0.6f);
				const auto hp = head->world.translate;
				entry.capsules[entry.count++] = { hp, hp, 8.0f * scale };
				auto* pelvis = root->GetObjectByName("NPC Pelvis [Pelv]"sv);
				auto* chest = root->GetObjectByName("NPC Spine2 [Spn2]"sv);
				if (!chest) {
					chest = root->GetObjectByName("NPC Spine1 [Spn1]"sv);
				}
				const RE::NiPoint3 upper = chest ? chest->world.translate : RE::NiPoint3{ hp.x, hp.y, hp.z - 23.0f * scale };
				const RE::NiPoint3 lower = pelvis ? pelvis->world.translate : root->world.translate;
				if (SightGeometry::Finite(G(upper)) && SightGeometry::Finite(G(lower))) {
					entry.capsules[entry.count++] = { lower, upper, 15.0f * scale };
				}
			}
			// Dead actors still block. The player is added explicitly below, since the
			// high-process actor list may not include them.
			context.actors.push_back(entry);
		};
		for (auto& handle : lists->highActorHandles) {
			if (auto actor = handle.get()) {
				append(actor.get());
			}
		}
		append(player);
		return context;
	}

	SightTarget MeasureSightTarget(RE::Actor* a_actor, const RE::NiPoint3& a_fallbackHead, float a_scale)
	{
		SightTarget target{};
		target.head = a_fallbackHead;
		target.id = a_actor ? a_actor->GetFormID() : 0;
		if (!std::isfinite(a_scale) || a_scale <= 0.0f) {
			return target;
		}
		target.scale = std::clamp(a_scale, 0.15f, 8.0f);
		if (auto* head = Head(a_actor ? a_actor->Get3D(false) : nullptr)) {
			target.head = head->world.translate;
			target.valid = SightGeometry::Finite(G(target.head));
		}
		// The stable composition fallback is useful to the caller, but isn't evidence
		// of an animated face when the skeleton isn't available.
		return target;
	}

	SubjectSight SubjectVisibility(const RE::NiPoint3& a_camera, const RE::NiPoint3& a_lookAt,
		float a_lens, float a_aspect, float a_crop, const SightTarget& a_target, const SightContext& a_context)
	{
		SubjectSight result{};
		if (!a_target.valid || !SightGeometry::Finite(G(a_target.head)) ||
			!std::isfinite(a_target.scale) || a_target.scale <= 0.0f) {
			return result;
		}
		const auto frame = SightGeometry::MakeFrame(G(a_camera), G(a_lookAt), a_lens, a_aspect, a_crop);
		if (!frame.valid) {
			return result;
		}
		const auto head = G(a_target.head);
		const auto right = SightGeometry::Mul(frame.right, 6.5f * a_target.scale);
		const auto up = SightGeometry::Mul(frame.up, 8.0f * a_target.scale);
		const std::array samples{ head, SightGeometry::Add(head, right), SightGeometry::Sub(head, right),
			SightGeometry::Add(head, up), SightGeometry::Sub(head, up) };
		std::array<SightState, 5> readings{};
		std::size_t clear = 0;
		std::size_t blocked = 0;
		for (std::size_t i = 0; i < samples.size(); ++i) {
			readings[i] = SightGeometry::InFrame(frame, samples[i]) ?
				SampleSight(P(samples[i]), a_camera, a_target.id, a_context) : SightState::kBlocked;
			clear += readings[i] == SightState::kClear;
			blocked += readings[i] == SightState::kBlocked;
		}
		result.face = static_cast<float>(clear) / 5.0f;
		result.state = SightGeometry::FaceState(readings);
		result.severe = blocked == samples.size() || !SightGeometry::InFrame(frame, head);
		if (result.state != SightState::kClear) {
			return result;
		}

		// Chest coverage only affects preference: something below a readable face is
		// allowed. Samples outside the usable frame get no bonus.
		const SightGeometry::Point chest{ head.x, head.y, head.z - 23.0f * a_target.scale };
		const auto halfWidth = SightGeometry::Mul(frame.right, 9.0f * a_target.scale);
		for (const auto sample : { SightGeometry::Add(chest, halfWidth), SightGeometry::Sub(chest, halfWidth) }) {
			if (SightGeometry::InFrame(frame, sample) &&
				SampleSight(P(sample), a_camera, a_target.id, a_context) == SightState::kClear) {
				result.torso += 0.5f;
			}
		}
		return result;
	}

	SightState LensClearance(const RE::NiPoint3& a_camera, const SightContext& a_context)
	{
		if (!SightGeometry::Finite(G(a_camera))) {
			return SightState::kUnknown;
		}
		constexpr float radius = 6.0f;
		const auto actors = ActorsAlong(a_camera, a_camera, a_context, 0, radius);
		if (actors == SightState::kBlocked) {
			return actors;
		}
		bool unknown = actors == SightState::kUnknown;
		constexpr std::array<SightGeometry::Point, 6> directions{
			SightGeometry::Point{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }
		};
		for (const auto direction : directions) {
			// Cast inward so near-surface penetration is caught even if a ray from the
			// lens would miss. The subject-to-lens rays cover a camera embedded deeper.
			const auto from = P(SightGeometry::Add(G(a_camera), SightGeometry::Mul(direction, radius)));
			const auto probe = CastWorld(a_context.world, from, a_camera);
			if (probe.valid && probe.hit) {
				return SightState::kBlocked;
			}
			unknown |= !probe.valid;
		}
		return unknown ? SightState::kUnknown : SightState::kClear;
	}

	bool Clear(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		const auto probe = Cast(a_from, a_to);
		if (!probe.valid) {
			return true;  // no physics world to consult; do not block the shot
		}
		return !probe.hit;
	}

	float Clearance(const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction, float a_max)
	{
		const RE::NiPoint3 target{
			a_origin.x + a_direction.x * a_max,
			a_origin.y + a_direction.y * a_max,
			a_origin.z + a_direction.z * a_max
		};

		const auto probe = Cast(a_origin, target);
		if (!probe.valid) {
			return a_max;  // unknown; assume open rather than refuse every shot
		}
		return probe.distance;
	}

	Bundle CastBundle(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		float a_spreadNear, float a_spreadFar)
	{
		Bundle bundle{};

		const RE::NiPoint3 delta{ a_to.x - a_from.x, a_to.y - a_from.y, a_to.z - a_from.z };
		const float        full = Length(delta);
		if (!(full > 1.0f)) {
			return bundle;
		}

		// The horizontal perpendicular, in the XY plane (the offsets are horizontal by
		// design). A near-vertical sightline has no useful one, so it degrades to the
		// center ray alone.
		const float flat = std::sqrt(delta.x * delta.x + delta.y * delta.y);
		const bool  hasPerp = flat > 1.0e-3f;
		const RE::NiPoint3 perp = hasPerp ?
			RE::NiPoint3{ -delta.y / flat, delta.x / flat, 0.0f } :
			RE::NiPoint3{ 0.0f, 0.0f, 0.0f };

		float nearest = full;
		int   cast = 0;
		int   reached = 0;

		const auto one = [&](float a_offset) {
			const RE::NiPoint3 from{
				a_from.x + perp.x * a_offset * a_spreadNear,
				a_from.y + perp.y * a_offset * a_spreadNear,
				a_from.z
			};
			const RE::NiPoint3 to{
				a_to.x + perp.x * a_offset * a_spreadFar,
				a_to.y + perp.y * a_offset * a_spreadFar,
				a_to.z
			};

			const auto probe = Cast(from, to);
			if (!probe.valid) {
				return;
			}

			++cast;
			bundle.valid = true;
			if (probe.hit) {
				nearest = std::min(nearest, probe.distance);
			} else {
				++reached;
			}
		};

		one(0.0f);
		if (hasPerp) {
			one(1.0f);
			one(-1.0f);
		}

		if (cast == 0) {
			return bundle;  // no physics world; caller decides what unknown means
		}

		bundle.distance = nearest;
		bundle.clear = static_cast<float>(reached) / static_cast<float>(cast);
		return bundle;
	}

	float Crowding(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to,
		RE::FormID a_ignoreA, RE::FormID a_ignoreB, float a_radius)
	{
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return 0.0f;
		}

		const RE::NiPoint3 axis{ a_to.x - a_from.x, a_to.y - a_from.y, 0.0f };
		const float        span = std::sqrt(axis.x * axis.x + axis.y * axis.y);
		if (!(span > 1.0f)) {
			return 0.0f;
		}

		const RE::NiPoint3 unit{ axis.x / span, axis.y / span, 0.0f };

		float worst = 0.0f;

		for (auto& handle : lists->highActorHandles) {
			auto actor = handle.get();
			if (!actor) {
				continue;
			}

			const auto id = actor->GetFormID();
			if (id == a_ignoreA || id == a_ignoreB) {
				continue;
			}
			if (actor->IsDead() || !actor->Is3DLoaded()) {
				continue;
			}

			const auto here = actor->GetPosition();

			// How far along the sightline they stand. Behind the camera or past the
			// subject isn't in the way.
			const float along = (here.x - a_from.x) * unit.x + (here.y - a_from.y) * unit.y;
			if (along < a_radius || along > span - a_radius) {
				continue;
			}

			// On a different level (a floor below, a gantry above).
			if (std::abs(here.z - a_from.z) > 180.0f) {
				continue;
			}

			const float offX = (here.x - a_from.x) - unit.x * along;
			const float offY = (here.y - a_from.y) - unit.y * along;
			const float lateral = std::sqrt(offX * offX + offY * offY);

			// Their own width plus the shot's width where they stand. Someone at arm's
			// length from the lens blocks far more of the frame than someone next to the
			// subject.
			auto* ref3D = actor->Get3D();
			const float body = ref3D ? std::max(ref3D->worldBound.radius, 24.0f) : 34.0f;
			const float reach = body + a_radius * (1.0f - along / span);

			if (lateral >= reach) {
				continue;
			}

			worst = std::max(worst, 1.0f - lateral / reach);
		}

		return std::clamp(worst, 0.0f, 1.0f);
	}
}

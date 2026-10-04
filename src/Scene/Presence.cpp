#include "SD/Scene/Presence.h"

#include "SD/Core/Logging.h"

namespace SD::Scene
{
	namespace
	{
		bool            engaged{ false };
		bool            restoreHeadTracking{ false };
		RE::ActorHandle partner{};
		float           graphCountdown{ 0.0f };
		Log::OnceFlag   reported;

		// The target is set every frame (a cheap pointer write); the graph variable
		// only a few times a second, since it goes through the behaviour graph.
		constexpr float kGraphInterval = 0.2f;

		[[nodiscard]] RE::HighProcessData* HighOf(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process ? process->high : nullptr;
		}
	}

	void Presence::Engage(RE::Actor* a_npc)
	{
		if (engaged || !a_npc) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		// Two things are needed: the graph variable allows the head rotation at all,
		// and the target says where to look. The target alone does nothing.
		bool previous = false;
		if (player->GetGraphVariableBool("bHeadTracking", previous)) {
			restoreHeadTracking = !previous;
		}
		player->SetGraphVariableBool("bHeadTracking", true);

		if (auto* high = HighOf(player)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, a_npc);
		}

		// The NPC looks back. Usually they already do, but a forcegreet or scene line
		// can leave them facing where they were walking.
		if (auto* high = HighOf(a_npc)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, player);
		}

		engaged = true;
		partner = a_npc->GetHandle();
		graphCountdown = kGraphInterval;

		if (reported.Take()) {
			Log::Info(Log::Category::kStaging,
				"Headtracking engaged; player graph variable was {}."sv,
				restoreHeadTracking ? "off"sv : "already on"sv);
		}
	}

	void Presence::Update(float a_delta)
	{
		if (!engaged) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  npc = partner.get();
		if (!player || !npc) {
			return;
		}

		if (auto* high = HighOf(player)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, npc.get());
		}
		if (auto* high = HighOf(npc.get())) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, player);
		}

		graphCountdown -= a_delta;
		if (graphCountdown <= 0.0f) {
			graphCountdown = kGraphInterval;

			// The permission half. Other mods switch it off (combat behaviour, mount
			// transitions, their own head tracking), and with it false the target does
			// nothing.
			player->SetGraphVariableBool("bHeadTracking", true);
		}
	}

	void Presence::Release()
	{
		if (!engaged) {
			return;
		}
		engaged = false;
		partner = {};

		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}

		if (auto* high = HighOf(player)) {
			high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPES::kDialogue, nullptr);
		}

		// Only clear the graph variable if this turned it on; other mods enable player
		// head tracking too.
		if (restoreHeadTracking) {
			player->SetGraphVariableBool("bHeadTracking", false);
			restoreHeadTracking = false;
		}
	}
}

#pragma once

namespace SD::Scene
{
	// Makes the player look at the person they're talking to. Vanilla never turns
	// player head tracking on in third person during dialogue, which only shows
	// once the camera turns around to face the player.
	class Presence
	{
	public:
		static void Engage(RE::Actor* a_npc);

		// Re-applied every frame for the whole conversation. The head-track target is
		// a slot the engine and AI packages write constantly (package changes, combat
		// checks, the dialogue system's own head tracking), so a value written once
		// doesn't stay.
		static void Update(float a_delta);

		static void Release();
	};
}

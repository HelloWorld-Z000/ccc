#pragma once

namespace SD::Scene
{
	// NPCs at a workbench put the work down to talk.
	//
	// Skyrim lets an NPC answer from inside furniture, so a smith keeps hammering
	// through the whole conversation. Furniture can have a "must exit to talk"
	// flag, but almost no work furniture sets it. This does the equivalent a bit
	// later and more gently: once the NPC has been speaking for 3 to 5 seconds,
	// someone still using work furniture is sent the furniture-exit animation
	// event, the same one their AI sends when a package moves them on. Seats,
	// benches, beds and lean spots are left alone, and so are quest scenes. Their
	// package takes them back afterwards.
	//
	// The exit is checked a few seconds later and logged either way, since a
	// behaviour graph can refuse the event.
	class StopWork
	{
	public:
		// A conversation opened (or reopened) with this NPC.
		static void Begin(RE::Actor* a_npc);

		// Per staged frame, from Director::Tick. a_npcSpeaking is the Director's turn
		// state, so the clock starts when they actually start talking.
		static void Update(float a_delta, bool a_npcSpeaking);

		// The conversation is over, or suspended for another menu.
		static void End();

		// [Performance] bStopWorkToTalk.
		static void SetEnabled(bool a_enabled) noexcept;

		// Whether this furniture is work rather than a seat. Exposed for the log.
		[[nodiscard]] static bool IsWorkFurniture(const RE::TESFurniture* a_furniture);
	};
}

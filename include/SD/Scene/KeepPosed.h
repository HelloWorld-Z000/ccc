#pragma once

namespace SD::Scene
{
	// Keeps the actors the camera may cut to fully animated while they are off
	// screen.
	//
	// Actor::ModifyAnimationUpdateData (vfunc 0x79) only requests a full bone update
	// for actors that were rendered last frame; everyone else gets just the root
	// written back. A normal game camera never shows this, but a hard cut to someone
	// who was behind the camera shows one frame of a stale pose before it catches
	// up. While a conversation is staged, the listed actors get all bones and a full
	// (non-interpolated) update every frame.
	class KeepPosed
	{
	public:
		// Hooks vfunc 0x79 on Character and PlayerCharacter.
		static void Install();

		// Replaces the current set. Called every staged frame; entries past kMaxActors
		// are ignored.
		static void Set(std::span<RE::Actor* const> a_actors);

		static void Clear();

		static constexpr std::size_t kMaxActors = 12;
	};
}

#pragma once

namespace RE
{
	class PlayerCamera;
}

namespace SD::Runtime
{
	void Initialize();

	// Called once per frame from the frame source, after the game's own camera
	// update.
	void OnFrame(RE::PlayerCamera* a_camera, float a_delta);

	// Ticks since load. Logged by the menu watch to confirm the frame source runs
	// every frame.
	[[nodiscard]] std::uint64_t FrameCount() noexcept;

	// Lets the conversation Runtime thinks it staged be staged again. Runtime
	// opens once per partner and won't reopen while the key matches (reopening on
	// any release used to fight the exit path). This is how the Director says a
	// release is expected to come back: it handed the screen to a menu a dialogue
	// topic opened, with the conversation still waiting underneath.
	void RearmConversation();

	void OnGameLoaded();

	// A load is starting. Drop everything that refers to the outgoing world,
	// synchronously.
	void AbandonForLoad();
}

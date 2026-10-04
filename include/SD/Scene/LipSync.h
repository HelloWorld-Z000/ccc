#pragma once

namespace SD::Scene
{
	// Drives the player's mouth directly.
	//
	// Under a player-voice mod the player's line is played with `Player.SpeakSound
	// "DBVO/<pack>/<line>.fuz"`. The audio plays, but the .lip track embedded in
	// the file is never applied, so the player's phoneme channel stays empty
	// during their own lines (NPC speech lights eight to fourteen of the sixteen
	// slots; the player's lights none). The channel itself works: forcing slot 0
	// through the morph hook opens the mouth. So this generates the mouth
	// movement, as other mods in this space do (Open Your Mouth, AudioUtil). The
	// camera isn't involved.
	class LipSync
	{
	public:
		// a_strength is 0..100 and mainly scales the jaw (slot 0 at 1.0 is a fully
		// open mouth). The jaw takes it linearly; the lips take sqrt(strength), so the
		// lip shapes that identify each sound aren't cut along with the jaw. Same at 0
		// and 100, more generous in between. See Sample.
		static void Configure(bool a_enabled, int a_strength);

		[[nodiscard]] static bool Enabled() noexcept;

		// Live values for the settings panel. The panel must not re-read these from
		// the ini each frame: sliders only save on release, so reading the file would
		// snap the slider back mid-drag.
		[[nodiscard]] static int StrengthPercent() noexcept;

		static void Engage();
		static void Release();

		// Per frame, from Director::Tick, staging or not (it has to work with
		// bEnabled=0). Only computes the envelope; the write happens in Scene::FaceGen
		// (see Sample).
		static void Update(float a_delta);

		// Main-thread voice state shared with full-face expressions. Follows
		// DBReV/DBVO line events and measured audio duration even with synthesis off.
		[[nodiscard]] static bool PlayerSpeaking() noexcept;
		[[nodiscard]] static std::uint64_t PlayerLineSerial() noexcept;
		// Whether the current line was announced by a voice mod (a DBReV event or the
		// SpeakSound command) rather than inferred from a playing sound handle, which
		// could be any sound on the player.
		[[nodiscard]] static bool PlayerLineAnnounced() noexcept;
		[[nodiscard]] static std::string_view PlayerLineText() noexcept;
		[[nodiscard]] static float PlayerLineDuration() noexcept;
		// Read-only clock for facial acting; doesn't affect mouth scheduling.
		[[nodiscard]] static float PlayerLineElapsed() noexcept;
		static void OnResponse();


		// This frame's mouth shape, written into a_out. False when SD isn't driving.
		//
		// A full shape, not just a jaw amount: slots 0 and 1 are both the jaw at
		// different sizes, and driving only those looks like chewing. All slots are
		// written, zeros included, so values other mods left in unused slots don't
		// blend in.
		//
		// Separate from Update because Conditional Expressions and Expressive Facial
		// Animation also write this channel, and a value written from the frame tick
		// can be overwritten before the morph pass. FaceGen writes it from
		// BSFaceGenNiNode::UpdateDownwardPass, right before the original runs.
		[[nodiscard]] static bool Sample(float* a_out, std::uint32_t a_count) noexcept;

	};
}

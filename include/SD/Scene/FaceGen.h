#pragma once

namespace SD::Scene
{
	// A hook on BSFaceGenNiNode::UpdateDownwardPass (vfunc 0x2C), where a head's
	// animation data is applied to its morphs. Used to write the player's mouth
	// and expression channels right before they're consumed, and as a diagnostic
	// that counts, per head and per frame, whether the morph pass runs, with what
	// NiUpdateData::time, and whether it consumes the phoneme keyframe (isUpdated
	// set by BSFaceGenKeyframeMultiple::SetValue, cleared by the reader).
	//
	// RE::VTABLE_BSFaceGenNiNode is in Offsets_VTABLE.h, so it's a write_vfunc
	// like Core/Tick. The declaration is compiled out under SKYRIM_CROSS_VR (hence
	// the free-function thunk), and 0x2C is the flat-Skyrim slot, so the install
	// refuses VR.
	class FaceGen
	{
	public:
		static void Install();

		[[nodiscard]] static bool Installed() noexcept;

		// Diagnostic: pin one viseme slot open on the player's head (-1 off, 0-15 a
		// slot). If the mouth opens, the channel reaches the geometry and any problem
		// is upstream; if not, it's on the head itself. Needs no conversation or voice
		// mod.
		static void SetForcedViseme(int a_slot);

		// Begin a counting window. Called when a conversation opens.
		static void Begin(RE::Actor* a_npc);

		// End the window and log the comparison.
		static void End();

		// Per-second summary while a window is open, driven from the frame tick.
		static void Tick(float a_delta);
		// Synchronous hand-back before a load: only modifier values SD still owns.
		static void ReleaseModifiers();
	};
}

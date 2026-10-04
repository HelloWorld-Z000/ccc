#pragma once

namespace SD::Dialogue
{
	// Line detection. TESObjectREFR::UpdateInDialogue(DialogueResponse*, bool) is
	// vfunc 0x4C, overridden by Actor, and the engine calls it on the speaking
	// actor with the response being spoken. Compared with polling MenuTopicManager
	// this needs no frame source, names the speaker directly, and hands over the
	// DialogueResponse (emotion, intensity, text, voice file and both authored
	// idles).
	//
	// Hooked on Character and PlayerCharacter separately, each with its own
	// trampoline.
	class LineWatch
	{
	public:
		static void Install();
		[[nodiscard]] static bool Installed() noexcept;
	};
}

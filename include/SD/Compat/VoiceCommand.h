#pragma once

namespace SD::Compat
{
	// Learns what the player is being made to say by watching the console command
	// that plays it.
	//
	// Every DBVO generation plays the player's line with `Player.SpeakSound
	// "DBVO/<pack>/<line>.fuz"` (DBReV logs the same call), so the argument names
	// the pack and the exact file. That matters with several packs installed:
	// guessing a filename and taking the first pack that has it often finds
	// another actor's recording at a different pace. The voice mods' own config
	// files aren't a reliable alternative (they differ by version, and DBVO 2's
	// isn't always written).
	//
	// The command table is data, so this swaps SpeakSound's function pointer for a
	// wrapper that records the call and then calls the original. Nothing else
	// changes, and if the command can't be found nothing is patched.
	//
	// The same call also gives the start of the line and confirms that the player
	// actually spoke (sound handle polling can't tell the player's line from other
	// sounds on the player). That's most of what DBReV's API provides, minus the
	// end event and post-line delay; DBReV stays the better source where it's
	// installed.
	class VoiceCommand
	{
	public:
		// From kDataLoaded, once the command table exists.
		static void Install();

		// The pack last seen playing, e.g. "voicebella". Empty until a line has been
		// spoken.
		[[nodiscard]] static std::string Pack();

		// One player line, as the SpeakSound call described it.
		struct Line
		{
			// The argument verbatim, e.g. "DBVO/voicebella/What_is_it_.fuz", relative to
			// Data/Sound.
			std::string path;

			// The pack segment of the same string, e.g. "voicebella".
			std::string pack;
		};

		// The start of a player line, if one arrived since the last call. Consuming,
		// main thread only. When EverHeard() is true, false means the player isn't
		// speaking.
		[[nodiscard]] static bool TakeLineStart(Line& a_out);

		// Has a SpeakSound on the player been seen this session? Same idea as DBReV's
		// EverSpoke: consumers only rely on this path once it has produced a line (the
		// command may not be patchable, or the voice mod may not use it). Not cleared
		// by Reset, since it describes the load order, not the save.
		[[nodiscard]] static bool EverHeard() noexcept;

		// Drop any unconsumed line. Called on load with DBReV::Reset.
		static void Reset();
	};
}

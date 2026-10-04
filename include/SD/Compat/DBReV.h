#pragma once

namespace SD::Compat
{
	// Dragonborn ReVoiced tells us when the player speaks, and for how long.
	//
	// Compared with polling sound handles this fixes three things: the mouth now
	// stops when the audio does (the handle often never reports an end), lines the
	// player didn't actually voice no longer get a moving mouth (handle polling
	// can't tell the player's line from other sounds on the player), and the exact
	// file path is known instead of guessed.
	//
	// PlayerLineStart also carries the FUZ's LIP data, but nothing here parses it;
	// decoding FaceFX is a separate project.
	//
	// One-way: DBReV pushes, we receive. See DBReV_API.h and DBReV's
	// INTEGRATION.md.
	class DBReV
	{
	public:
		// Call from kPostLoad, not plugin load: SKSE loads plugins alphabetically, so
		// DBReV isn't there yet at our load and the registration would silently fail.
		static void Register();

		// True when DBReV accepted the listener registration. Not decided by looking
		// for DBReV.esp (DBVO 2 has no ESP, and DBReV's author asks integrators not to
		// check plugin names). When false, callers keep their existing behaviour
		// (sound handle polling, .fuz lookup, timeouts), which is the DBVO path.
		[[nodiscard]] static bool Present() noexcept;

		// One player line, copied out of the broadcast: the message's pointers die
		// when the callback returns, and the callback runs on the Papyrus thread.
		struct Line
		{
			// When the mouth stops, measured from the FUZ header. 0 when DBReV couldn't
			// measure the file; then only totalSeconds is meaningful.
			float audioSeconds{ 0.0f };

			// When the conversation advances: audioSeconds plus the user's post-line delay
			// from DBReV's MCM. Not interchangeable with audioSeconds; driving the lips
			// off this would leave the mouth moving after the voice stops.
			float totalSeconds{ 0.0f };

			// Position in MenuTopicManager::dialogueList, as the engine reported it.
			std::uint32_t topicIndex{ 0 };

			// Sanitized topic text (the filename form, so punctuation is mangled). A
			// fallback only; LipSync reads the menu's own list for the words.
			std::string topicKey;
		};

		// The start of a line, if one arrived since the last call. Consuming, main
		// thread only. False on a DBReV profile means the player isn't speaking.
		[[nodiscard]] static bool TakeLineStart(Line& a_out);

		// The end of a line, if one arrived since the last call. Consuming. a_reason
		// is one of DBReV::kEndReason_* (completed, skipped, superseded).
		[[nodiscard]] static bool TakeLineEnd(std::uint32_t& a_reason);

		// "completed" / "skipped" / "superseded", for the log.
		[[nodiscard]] static std::string_view EndReasonName(std::uint32_t a_reason);

		// Is a player line open right now (between a start and its end)? This is the
		// conversation window (totalSeconds), so it's right for holding a shot or the
		// topic list and wrong for driving a mouth.
		[[nodiscard]] static bool Speaking() noexcept;

		// Has DBReV broadcast a line this session? Present() isn't enough: DBReV can
		// be loaded while another framework voices the player, or with its voice pack
		// off, and waiting for events that never come would leave the mouth still.
		// Callers switch to DBReV only once it has spoken.
		[[nodiscard]] static bool EverSpoke() noexcept;

		// Cleared on load and new game, so a line can't stay open across a reload.
		static void Reset();
	};
}

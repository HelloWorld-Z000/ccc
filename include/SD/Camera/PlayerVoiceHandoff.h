#pragma once

#include <algorithm>
#include <cstdint>

namespace SD::Camera
{
	// Camera-only anticipation. Never declares the NPC to be speaking or changes
	// dialogue readiness. A voice must start in this staging session to arm it.
	class PlayerVoiceHandoff
	{
	public:
		void Reset(std::uint64_t a_serial) noexcept
		{
			serial = a_serial;
			phase = Phase::kIdle;
			sinceEnd = 0.0f;
		}

		// Seconds to keep the camera on the player after their voiced line ends. Only
		// covers the silence before the reply: the reply starting ends the hold early.
		// Any pause after the line comes from the voice mod's own post-line delay.
		void SetDelay(float a_seconds) noexcept
		{
			delay = a_seconds > 0.0f ? a_seconds : 0.0f;
		}

		void Update(std::uint64_t a_serial, bool a_playerSpeaking,
			bool a_npcSpeaking, bool a_choosing, float a_delta = 0.0f) noexcept
		{
			if (Ended() && a_delta > 0.0f) {
				sinceEnd += a_delta;
			}

			if (a_serial != serial) {
				serial = a_serial;
				phase = a_playerSpeaking ? Phase::kPlayer : Phase::kIdle;
			} else if (phase == Phase::kPlayer && !a_playerSpeaking) {
				phase = Phase::kAwaitingReply;
				sinceEnd = 0.0f;
			}

			if (phase == Phase::kAwaitingReply && a_npcSpeaking) {
				phase = Phase::kReply;
				// Whoever is talking gets the camera. See SetDelay.
				sinceEnd = std::max(sinceEnd, delay);
			} else if (phase == Phase::kReply && !a_npcSpeaking) {
				phase = Phase::kIdle;
			}
			// A topic with no answer (or a cancelled line) must not strand the
			// camera on the NPC once the game returns control to the topic list.
			if (!a_playerSpeaking && !a_npcSpeaking && a_choosing) {
				phase = Phase::kIdle;
			}
		}

		// Line over and the hold has elapsed: the camera belongs on the NPC.
		[[nodiscard]] bool Active() const noexcept
		{
			return Ended() && sinceEnd >= delay;
		}

		// Line over but still inside the hold: stay on the player.
		[[nodiscard]] bool Holding() const noexcept
		{
			return Ended() && sinceEnd < delay;
		}

	private:
		enum class Phase { kIdle, kPlayer, kAwaitingReply, kReply };

		[[nodiscard]] bool Ended() const noexcept
		{
			return phase == Phase::kAwaitingReply || phase == Phase::kReply;
		}

		std::uint64_t serial{ 0 };
		Phase phase{ Phase::kIdle };
		float delay{ 0.0f };
		float sinceEnd{ 0.0f };
	};
}

#include "SD/Compat/DBReV.h"

#include "SD/Compat/DBReV_API.h"
#include "SD/Core/Logging.h"

namespace SD::Compat
{
	namespace
	{
		bool present{ false };
		bool registered{ false };

		// DBReV's callback runs on the Papyrus thread; everything that reads the data
		// runs on the main thread, and LipSync writes the player's FaceGen channel
		// from a render-path hook. So the callback only copies plain data and one
		// string under a mutex, and the main thread drains it every frame. No task
		// queue, which would only add a frame of latency. One write per line, one read
		// per frame.
		std::mutex mutex;

		bool             startPending{ false };
		DBReV::Line      pendingLine;
		bool             endPending{ false };
		std::uint32_t    endReason{ 0 };

		// Between a start and its end. Written under the mutex, read without it; a
		// reader one frame stale is fine here.
		std::atomic_bool speaking{ false };

		// Set on the first line and never cleared, even across loads: it answers
		// whether DBReV is the mod voicing this player.
		std::atomic_bool everSpoke{ false };

		Log::OnceFlag absenceReported;
		Log::OnceFlag firstLineReported;

		[[nodiscard]] std::string_view ReasonName(std::uint32_t a_reason)
		{
			switch (a_reason) {
			case ::DBReV::kEndReason_Completed:  return "completed"sv;
			case ::DBReV::kEndReason_Skipped:    return "skipped"sv;
			case ::DBReV::kEndReason_Superseded: return "superseded"sv;
			default:                             return "unknown"sv;
			}
		}

		void OnDBReVMessage(SKSE::MessagingInterface::Message* a_message)
		{
			if (!a_message || !a_message->data) {
				return;
			}

			switch (a_message->type) {
			case ::DBReV::kPlayerLineStart:
				{
					const auto* line = static_cast<const ::DBReV::PlayerLineStart*>(a_message->data);

					// The ABI is append-only, so fields this header knows about are safe from
					// version 1 on.
					if (line->version < 1) {
						return;
					}

					{
						const std::scoped_lock lock{ mutex };

						// A new start replacing an unconsumed one is a topic clicked before the last
						// line finished; animate the newest. DBReV closes the old one with
						// kEndReason_Superseded.
						pendingLine.audioSeconds = line->audioSeconds;
						pendingLine.totalSeconds = line->totalSeconds;
						pendingLine.topicIndex = line->topicIndex;

						// Copied: this pointer is into DBReV's stack.
						pendingLine.topicKey = line->topicKey ? line->topicKey : "";

						startPending = true;
					}

					speaking.store(true, std::memory_order_relaxed);
					everSpoke.store(true, std::memory_order_relaxed);

					if (firstLineReported.Take()) {
						Log::Info(Log::Category::kCompat,
							"DBReV: first player line received - audio {:.2f}s, total {:.2f}s. Voice timing is now event-driven."sv,
							line->audioSeconds, line->totalSeconds);
					}
				}
				break;

			case ::DBReV::kPlayerLineEnd:
				{
					const auto* end = static_cast<const ::DBReV::PlayerLineEnd*>(a_message->data);
					if (end->version < 1) {
						return;
					}

					{
						const std::scoped_lock lock{ mutex };
						endReason = end->reason;
						endPending = true;
					}

					speaking.store(false, std::memory_order_relaxed);
				}
				break;

			default:
				break;
			}
		}
	}

	void DBReV::Register()
	{
		if (registered) {
			return;
		}
		registered = true;

		const auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging) {
			Log::Warn(Log::Category::kCompat, "SKSE messaging unavailable; DBReV integration disabled."sv);
			return;
		}

		present = messaging->RegisterListener(::DBReV::kSender, OnDBReVMessage);

		if (present) {
			Log::Info(Log::Category::kCompat,
				"DBReV detected. Player line timing will come from its API rather than from sound handles."sv);
		} else if (absenceReported.Take()) {
			// Not a warning; most load orders use DBVO 1, DBVO 2 or no player voice, and
			// all are supported.
			Log::Info(Log::Category::kCompat,
				"DBReV not present (needs 1.4.4 or later). Falling back to sound-handle polling and .fuz filename matching."sv);
		}
	}

	bool DBReV::Present() noexcept
	{
		return present;
	}

	bool DBReV::TakeLineStart(Line& a_out)
	{
		const std::scoped_lock lock{ mutex };
		if (!startPending) {
			return false;
		}

		startPending = false;
		a_out = pendingLine;
		return true;
	}

	bool DBReV::TakeLineEnd(std::uint32_t& a_reason)
	{
		const std::scoped_lock lock{ mutex };
		if (!endPending) {
			return false;
		}

		endPending = false;
		a_reason = endReason;
		return true;
	}

	bool DBReV::Speaking() noexcept
	{
		return speaking.load(std::memory_order_relaxed);
	}

	bool DBReV::EverSpoke() noexcept
	{
		return everSpoke.load(std::memory_order_relaxed);
	}

	// Wrapped so callers don't need DBReV_API.h for the constants.
	std::string_view DBReV::EndReasonName(std::uint32_t a_reason)
	{
		return ReasonName(a_reason);
	}

	void DBReV::Reset()
	{
		{
			const std::scoped_lock lock{ mutex };
			startPending = false;
			endPending = false;
			endReason = 0;
			pendingLine = {};
		}

		speaking.store(false, std::memory_order_relaxed);
	}
}

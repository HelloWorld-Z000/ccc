#include "SD/Dialogue/Session.h"

#include "SD/Core/Logging.h"

namespace SD::Dialogue
{
	namespace
	{
		using Emotion = RE::EmotionType;

		std::string_view EmotionName(Emotion a_type) noexcept
		{
			switch (a_type) {
			case Emotion::kNeutral:  return "Neutral"sv;
			case Emotion::kAnger:    return "Anger"sv;
			case Emotion::kDisgust:  return "Disgust"sv;
			case Emotion::kFear:     return "Fear"sv;
			case Emotion::kSad:      return "Sad"sv;
			case Emotion::kHappy:    return "Happy"sv;
			case Emotion::kSurprise: return "Surprise"sv;
			case Emotion::kPuzzled:  return "Puzzled"sv;
			default:                 return "Unknown"sv;
			}
		}

		RE::Actor* AsActor(RE::ObjectRefHandle a_handle)
		{
			const auto ref = a_handle.get();
			return ref ? ref->As<RE::Actor>() : nullptr;
		}

		std::string NameOf(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return "<none>"s;
			}
			const char* name = a_actor->GetName();
			return (name && *name) ? std::string{ name } : "<unnamed>"s;
		}

		std::string Condense(const char* a_raw, std::size_t a_max)
		{
			if (!a_raw || !*a_raw) {
				return "<no text>"s;
			}
			std::string text{ a_raw };
			// Response text can contain newlines; keep the log line on one line.
			std::replace(text.begin(), text.end(), '\n', ' ');
			std::replace(text.begin(), text.end(), '\r', ' ');
			if (text.size() > a_max) {
				text.resize(a_max);
				text += "..."sv;
			}
			return text;
		}

		// The head position to frame against. Falls back to the root when there's no
		// head node, so creatures and unusual races still get framed.
		std::optional<RE::NiPoint3> HeadOf(RE::Actor* a_actor)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			if (!root) {
				return std::nullopt;
			}
			if (auto* node = root->GetObjectByName("NPC Head [Head]"sv)) {
				return node->world.translate;
			}
			return root->world.translate;
		}

		// Read the response list the manager already built for this topic info.
		// Doesn't construct a DialogueItem to fill gaps: that's an engine call that
		// allocates from the game heap, and the line can do without it. Unmatched
		// lines are logged as unresolved.
		void ReadResponses(RE::MenuTopicManager* a_manager, RE::TESTopicInfo* a_info, Line& a_line)
		{
			auto* dialogue = a_manager->lastSelectedDialogue;
			if (!dialogue || dialogue->parentTopicInfo != a_info) {
				return;
			}

			std::size_t                 count = 0;
			const RE::DialogueResponse* first = nullptr;
			for (auto* response : dialogue->responses) {
				if (!response) {
					continue;
				}
				if (!first) {
					first = response;
				}
				++count;
			}

			if (!first) {
				return;
			}

			a_line.responseCount = count;
			a_line.emotion = first->animFaceArchType.get();
			a_line.emotionPercent = first->percent;
			a_line.text = Condense(first->text.c_str(), 90);
			a_line.hasSpeakerIdle = first->speakerIdle != nullptr;
			a_line.hasListenIdle = first->listenIdle != nullptr;
			a_line.resolved = true;
		}
	}

	Session& Session::GetSingleton() noexcept
	{
		static Session instance;
		return instance;
	}

	void Session::OnFrame(float a_delta)
	{
		auto* manager = RE::MenuTopicManager::GetSingleton();
		if (!manager) {
			return;
		}

		// The two speaker handles, read separately (see Session.h). speaker goes null
		// when the menu closes, but the NPC keeps talking through the farewell;
		// lastSpeaker keeps the session alive through that tail, without counting as
		// the player still being in the conversation.
		auto*      live = AsActor(manager->speaker);
		auto*      tail = AsActor(manager->lastSpeaker);
		auto*      speaker = live ? live : tail;
		const bool inConversation = speaker != nullptr;

		// Published every frame, before any transition below can end the session. See
		// PlayerEngaged.
		engaged = live != nullptr;

		if (inConversation && !active) {
			Enter(speaker);
			hadLive = live != nullptr;
			lostLive = false;
		} else if (!inConversation && active) {
			Exit();
			return;
		} else if (inConversation && active && speaker->GetFormID() != partnerID) {
			// The player went straight from one conversation into another. The speaker
			// handle never goes null in between, so the partner change has to be detected
			// explicitly.
			Log::Info(Log::Category::kDialogue,
				"Partner changed mid-session: [{:08X}] -> {} [{:08X}]; restarting."sv,
				partnerID, NameOf(speaker), speaker->GetFormID());
			Exit();
			Enter(speaker);
			hadLive = live != nullptr;
			lostLive = false;
		} else if (active && live && lostLive) {
			// A new conversation with someone who never stopped talking: the player left
			// mid-line (the session stayed alive on lastSpeaker) and activated them again.
			// Same actor, same form ID, so restart the session once, on the frame the live
			// speaker comes back. lostLive is cleared by the restart.
			Log::Info(Log::Category::kDialogue,
				"Re-entered dialogue with {} [{:08X}] while their previous line was "
				"still running; ending the old session and starting a new one."sv,
				NameOf(live), live->GetFormID());
			Exit();
			Enter(live);
			hadLive = true;
			lostLive = false;
		} else if (active) {
			// The only place lostLive is armed. A session entered from the tail alone (a
			// forcegreet with no menu) never had a live speaker, so the menu appearing
			// partway through it continues the same conversation.
			if (live) {
				hadLive = true;
				lostLive = false;
			} else if (hadLive) {
				lostLive = true;
			}
		}

		if (!active) {
			return;
		}

		sessionElapsed += a_delta;

		auto* info = manager->currentTopicInfo;
		if (info != lastInfo) {
			if (speaking) {
				EndLine();
			}
			lastInfo = info;
			if (info) {
				BeginLine(info, speaker);
			}
		}

		if (speaking) {
			lineElapsed += a_delta;
		}
	}

	void Session::Enter(RE::Actor* a_speaker)
	{
		active = true;
		speaking = false;
		sessionElapsed = 0.0f;
		lineElapsed = 0.0f;
		lineCount = 0;
		lastInfo = nullptr;
		current = {};
		partnerID = a_speaker ? a_speaker->GetFormID() : 0;
		partner = a_speaker ? a_speaker->GetHandle() : RE::ActorHandle{};

		// The conversation identity everything downstream keys on. Bumped here only.
		// See ConversationSerial.
		++serial;

		auto*      player = RE::PlayerCharacter::GetSingleton();
		const auto speakerHead = HeadOf(a_speaker);
		const auto playerHead = HeadOf(player);

		float separation = -1.0f;
		if (speakerHead && playerHead) {
			separation = speakerHead->GetDistance(*playerHead);
		}

		Log::Info(Log::Category::kDialogue,
			"Conversation {} opened with {} [{:08X}] — separation {:.1f}u, speaker head {}, player head {}."sv,
			serial, NameOf(a_speaker), partnerID, separation,
			speakerHead ? "found"sv : "MISSING"sv,
			playerHead ? "found"sv : "MISSING"sv);
	}

	void Session::Exit()
	{
		if (speaking) {
			EndLine();
		}

		Log::Info(Log::Category::kDialogue,
			"Conversation closed after {:.1f}s and {} line(s)."sv, sessionElapsed, lineCount);

		active = false;
		speaking = false;
		lastInfo = nullptr;
		partnerID = 0;
		partner = {};
		current = {};
		lineElapsed = 0.0f;
		sessionElapsed = 0.0f;

		// Cleared with everything else, or the next session's first frame would
		// restart a conversation that just began.
		hadLive = false;
		lostLive = false;
		engaged = false;
	}

	void Session::BeginLine(RE::TESTopicInfo* a_info, RE::Actor* a_speaker)
	{
		auto* manager = RE::MenuTopicManager::GetSingleton();

		current = {};
		current.topicInfoID = a_info->GetFormID();
		current.speakerID = a_speaker ? a_speaker->GetFormID() : 0;
		current.speakerName = NameOf(a_speaker);
		current.greeting = manager && manager->isGreetingPlayer;
		// forceGoodbye is the same flag CommonLibSSE used to call isSayingGoodbye.
		current.farewell = manager && manager->forceGoodbye;

		if (manager) {
			ReadResponses(manager, a_info, current);
		}

		speaking = true;
		lineElapsed = 0.0f;
		++lineCount;

		if (current.resolved) {
			Log::Info(Log::Category::kDialogue,
				"Line {} start | {} [{:08X}] | {}({}%) | responses {} | idles spk={} lsn={}{}{} | \"{}\""sv,
				lineCount, current.speakerName, current.topicInfoID,
				EmotionName(current.emotion), current.emotionPercent,
				current.responseCount,
				current.hasSpeakerIdle ? "y"sv : "n"sv,
				current.hasListenIdle ? "y"sv : "n"sv,
				current.greeting ? " | GREETING"sv : ""sv,
				current.farewell ? " | FAREWELL"sv : ""sv,
				current.text);
		} else {
			// Expected whenever the NPC says something the player didn't pick
			// (forcegreets, scene lines, idle chatter).
			Log::Info(Log::Category::kDialogue,
				"Line {} start | {} [{:08X}] | UNRESOLVED (no matching response list){}{}"sv,
				lineCount, current.speakerName, current.topicInfoID,
				current.greeting ? " | GREETING"sv : ""sv,
				current.farewell ? " | FAREWELL"sv : ""sv);
		}
	}

	void Session::EndLine()
	{
		// Whether a line's length is known before it plays or only after it ends.
		Log::Info(Log::Category::kDialogue,
			"Line {} end   | {:.2f}s | {} response(s){}"sv,
			lineCount, lineElapsed, current.responseCount,
			current.resolved ? ""sv : " | was unresolved"sv);

		speaking = false;
		lineElapsed = 0.0f;
	}

	void Session::Abandon()
	{
		active = false;
		speaking = false;
		lastInfo = nullptr;
		partnerID = 0;
		partner = {};
		current = {};
		lineElapsed = 0.0f;
		sessionElapsed = 0.0f;
		lineCount = 0;
		hadLive = false;
		lostLive = false;
		engaged = false;

		// The serial isn't reset: it's an identity, and Runtime compares it with one
		// recorded before the load.
	}
}

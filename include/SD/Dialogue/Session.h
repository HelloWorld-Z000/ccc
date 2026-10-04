#pragma once

namespace SD::Dialogue
{
	// One spoken line, resolved when it starts.
	struct Line
	{
		RE::FormID  topicInfoID{ 0 };
		RE::FormID  speakerID{ 0 };
		std::string speakerName;
		std::string text;

		// Every authored response carries an emotion and an intensity (the engine uses
		// them for facegen); the director uses them to choose shots.
		RE::EmotionType emotion{ RE::EmotionType::kNeutral };
		std::uint16_t                     emotionPercent{ 0 };

		// A topic info can hold several responses spoken back to back, and the manager
		// doesn't expose which one is playing, so the group is treated as one line.
		// This count shows in the log when that matters.
		std::size_t responseCount{ 0 };

		// Authored idles for the speaker and listener on this line.
		bool hasSpeakerIdle{ false };
		bool hasListenIdle{ false };

		bool resolved{ false };  // false when the response list could not be matched
		bool greeting{ false };
		bool farewell{ false };
	};

	// Watches MenuTopicManager and turns it into conversation and line edges.
	// Observational only; nothing here moves the camera.
	class Session
	{
	public:
		[[nodiscard]] static Session& GetSingleton() noexcept;

		void OnFrame(float a_delta);
		void Abandon();  // a load is starting; drop state without touching the world

		[[nodiscard]] bool            Active() const noexcept { return active; }
		[[nodiscard]] RE::ActorHandle Partner() const noexcept { return partner; }

		// Which conversation this is, counted from load; bumped by Enter(). The
		// partner's form ID can't tell two conversations with the same person apart
		// (leave while they're still talking, then talk to them again), so Runtime
		// keys on this as well.
		[[nodiscard]] std::uint32_t ConversationSerial() const noexcept { return serial; }

		// Is the player still in this conversation, as opposed to standing outside one
		// that's still talking at them? Active() is true for both (the farewell tail
		// keeps the session alive). This decides whether a conversation suspended for
		// a trade is owed a return.
		[[nodiscard]] bool PlayerEngaged() const noexcept { return engaged; }
		[[nodiscard]] bool        Speaking() const noexcept { return speaking; }
		[[nodiscard]] const Line& Current() const noexcept { return current; }
		[[nodiscard]] float       LineElapsed() const noexcept { return lineElapsed; }

	private:
		void Enter(RE::Actor* a_speaker);
		void Exit();
		void BeginLine(RE::TESTopicInfo* a_info, RE::Actor* a_speaker);
		void EndLine();

		bool              active{ false };
		bool              speaking{ false };
		Line              current{};
		RE::TESTopicInfo* lastInfo{ nullptr };
		RE::FormID        partnerID{ 0 };
		RE::ActorHandle   partner{};
		std::uint32_t     serial{ 0 };

		// MenuTopicManager has two speaker handles: `speaker` is live participation,
		// `lastSpeaker` is the tail after the player has left.
		//
		//   hadLive   this session has had a live speaker at least once (the player
		//             entered it, rather than a forcegreet seen from its tail).
		//   lostLive  ...and has since lost it. A live speaker arriving while this
		//             is set is the player starting a new conversation with someone
		//             who never stopped talking.
		bool hadLive{ false };
		bool lostLive{ false };

		// This frame's PlayerEngaged value, written every frame the session is alive.
		bool engaged{ false };
		float             lineElapsed{ 0.0f };
		float             sessionElapsed{ 0.0f };
		std::uint32_t     lineCount{ 0 };
	};
}

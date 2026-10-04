#pragma once

namespace SD::Dialogue
{
	// Who is talking around the player when the player isn't. Keeps the last few
	// lines from other NPCs so a conversation between them can be found and
	// filmed.
	struct SceneCast
	{
		// The NPC who spoke last, and who they're talking to.
		RE::ActorHandle speaker{};
		RE::ActorHandle other{};

		// Both have spoken: a real exchange between two NPCs. False for one NPC
		// talking at someone, where `other` is whoever they're addressing (the player,
		// if nobody else).
		bool exchange{ false };

		// How `other` was found, for the log.
		std::string_view how{};

		std::uint64_t key{ 0 };

		// Seconds since `speaker` said the line this was found from (the scene's
		// newest line).
		float sinceLine{ 0.0f };
	};

	// A line that appeared in the engine's subtitles.
	struct HeardLine
	{
		RE::ActorHandle speaker{};
		std::string     text{};
	};

	class SceneWatch
	{
	public:
		// Every line spoken by someone other than the player, from LineWatch.
		static void OnLine(RE::Actor* a_speaker, const RE::DialogueResponse* a_response);

		// Lines between NPCs don't reach UpdateInDialogue (it only fires for lines
		// spoken to the player), but every subtitled line goes through the engine's
		// SubtitleManager with its speaker. Polled every frame from the tick; returns
		// the speakers whose subtitle text changed since the last call.
		[[nodiscard]] static std::vector<HeardLine> Poll();

		// The actor's subtitle is on screen, as of the last Poll.
		[[nodiscard]] static bool Talking(const RE::Actor* a_actor);

		// Who the engine says this NPC is talking to: their dialogue target, then
		// where their head is turned. Null when neither names another actor.
		[[nodiscard]] static RE::NiPointer<RE::Actor> Listener(RE::Actor* a_speaker);

		// Everyone other than the player who said a line in the last a_maxAge seconds,
		// newest first, each once.
		[[nodiscard]] static std::vector<RE::ActorHandle> RecentSpeakers(float a_maxAge);

		// The conversation worth filming nearest the player, if any.
		//
		// a_range is how far either participant may be from the player. a_strict is
		// auto mode: two NPCs in a real conversation (same scene, or a quick
		// back-and-forth); without it one NPC talking to anyone will do, since a key
		// press already shows intent. a_viewCos is how close to the middle of the view
		// the scene must be, as the cosine of the angle off the camera's axis (-1
		// skips the check). a_maxAge is how recent the newest line must be.
		[[nodiscard]] static std::optional<SceneCast> Find(float a_range, bool a_strict, float a_viewCos,
			float a_maxAge);

		// Forget everything: a load, or a new game.
		static void Reset();
	};
}

#include "SD/Dialogue/SceneWatch.h"

#include "SD/Core/Logging.h"
#include "SD/Dialogue/SceneTrigger.h"

#include <chrono>

namespace SD::Dialogue
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Line
		{
			RE::ActorHandle   speaker{};
			RE::FormID        id{ 0 };
			Clock::time_point at{};

			// Said to the player (a greeting in passing, a follower's aside, a merchant's
			// pitch).
			bool toPlayer{ false };
		};

		// Enough for a scene with a third voice plus a stray remark from across the
		// street.
		constexpr std::size_t kKept = 12;

		// How far back the other half of an exchange may have spoken.
		constexpr float kExchangeSeconds = 30.0f;

		// How close two speakers must be to count as one conversation.
		constexpr float kPairReach = 700.0f;

		// Roughly where a standing person's face is above their root. Only used to
		// check whether a scene is in front of the camera.
		constexpr float kHeadHeight = 110.0f;

		std::mutex              lock;
		std::array<Line, kKept> lines{};
		std::size_t             next{ 0 };
		std::size_t             stored{ 0 };

		// Who had a subtitle up at the last poll and its text, so a new line is a
		// change of text rather than every frame of the same one. Main thread only.
		struct Showing
		{
			std::string       text{};
			Clock::time_point at{};
			bool              counted{ false };  // recorded and logged as a line
		};
		std::unordered_map<RE::FormID, Showing> showing;

		// Most subtitles copied out in one poll. The engine shows one at a time with a
		// few queued; far more than that means a layout mismatch, so it's refused.
		constexpr std::size_t kMaxSubtitles = 16;
		constexpr std::size_t kImplausibleSubtitles = 256;

		Log::OnceFlag firstHeardReported;
		Log::OnceFlag layoutReported;

		// Is this line said to the player? Only the signals that mean addressed: the
		// dialogue item's target (set by greetings, SayTo and a scene's "speak to")
		// and the dialogue head-tracking slot. Not the default look-at, which points
		// at the player whenever they're nearby.
		[[nodiscard]] bool ToPlayer(RE::Actor* a_speaker)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!a_speaker || !player) {
				return false;
			}
			auto& data = a_speaker->GetActorRuntimeData();
			if (auto ref = data.dialogueItemTarget.get(); ref && ref.get() == player) {
				return true;
			}
			auto* process = data.currentProcess;
			auto* high = process ? process->high : nullptr;
			if (high) {
				auto ref = high->headTrackTarget[RE::HighProcessData::HEAD_TRACK_TYPE::kDialogue].get();
				return ref && ref.get() == player;
			}
			return false;
		}

		Line Record(RE::Actor* a_speaker)
		{
			const Line line{ a_speaker->GetHandle(), a_speaker->GetFormID(), Clock::now(), ToPlayer(a_speaker) };
			const std::scoped_lock guard{ lock };
			lines[next] = line;
			next = (next + 1) % kKept;
			stored = std::min(stored + 1, kKept);
			return line;
		}

		[[nodiscard]] float SecondsSince(Clock::time_point a_when, Clock::time_point a_now)
		{
			return std::chrono::duration<float>(a_now - a_when).count();
		}

		[[nodiscard]] float Distance(const RE::NiPoint3& a, const RE::NiPoint3& b)
		{
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		// Someone the camera can stay on for a whole scene.
		[[nodiscard]] bool Usable(RE::Actor* a_actor, RE::Actor* a_player)
		{
			return a_actor && a_actor != a_player && a_actor->Is3DLoaded() && !a_actor->IsDead() &&
			       !a_actor->IsDisabled() && !a_actor->IsInCombat();
		}

		// Who an NPC is talking to, if the engine says: the dialogue item's target
		// first, then the head-tracking slots. Null when neither names an actor.
		[[nodiscard]] RE::NiPointer<RE::Actor> ListenerOf(RE::Actor* a_speaker, std::string_view& a_how)
		{
			auto& data = a_speaker->GetActorRuntimeData();

			if (auto ref = data.dialogueItemTarget.get(); ref) {
				if (auto* actor = ref->As<RE::Actor>(); actor && actor != a_speaker) {
					a_how = "their dialogue target"sv;
					return RE::NiPointer<RE::Actor>{ actor };
				}
			}

			auto* process = data.currentProcess;
			auto* high = process ? process->high : nullptr;
			if (!high) {
				return {};
			}

			using Track = RE::HighProcessData::HEAD_TRACK_TYPE;
			constexpr std::array kOrder{ Track::kDialogue, Track::kScript, Track::kProcedure, Track::kAction, Track::kDefault };
			for (const auto type : kOrder) {
				if (auto ref = high->headTrackTarget[type].get(); ref) {
					if (auto* actor = ref->As<RE::Actor>(); actor && actor != a_speaker) {
						a_how = "where they are looking"sv;
						return RE::NiPointer<RE::Actor>{ actor };
					}
				}
			}
			return {};
		}

		// Is this point in front of the camera, within the cone a_cos describes?
		[[nodiscard]] bool InView(const RE::NiPoint3& a_point, float a_cos)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			auto* root = camera ? camera->cameraRoot.get() : nullptr;
			if (!root) {
				return true;  // nothing to ask; do not refuse on a missing camera
			}

			const auto& eye = root->world.translate;
			float dx = a_point.x - eye.x;
			float dy = a_point.y - eye.y;
			float dz = a_point.z - eye.z;
			const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (length < 1.0f) {
				return true;
			}
			dx /= length;
			dy /= length;
			dz /= length;

			// The camera node looks down its own Y axis.
			const auto& r = root->world.rotate;
			const float facing = r.entry[0][1] * dx + r.entry[1][1] * dy + r.entry[2][1] * dz;
			return facing >= a_cos;
		}

		[[nodiscard]] RE::NiPoint3 HeadOf(RE::Actor* a_actor)
		{
			auto position = a_actor->GetPosition();
			position.z += kHeadHeight * a_actor->GetScale();
			return position;
		}
	}

	void SceneWatch::OnLine(RE::Actor* a_speaker, const RE::DialogueResponse* a_response)
	{
		if (!a_speaker || !a_response) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (a_speaker == static_cast<RE::Actor*>(player)) {
			return;
		}
		Record(a_speaker);
	}

	std::vector<HeardLine> SceneWatch::Poll()
	{
		std::vector<HeardLine> heard;

		auto* manager = RE::SubtitleManager::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!manager || !player) {
			showing.clear();
			return heard;
		}

		struct Entry
		{
			RE::ObjectRefHandle speaker{};
			std::string         text{};
		};
		std::array<Entry, kMaxSubtitles> entries{};
		std::size_t                      count = 0;
		{
			RE::BSSpinLockGuard guard{ manager->lock };
			if (manager->subtitles.size() > kImplausibleSubtitles) {
				if (layoutReported.Take()) {
					Log::Warn(Log::Category::kDialogue,
						"SubtitleManager reports {} subtitles; its layout does not match this build. "
						"Other people's conversations can only be heard through the dialogue hook."sv,
						manager->subtitles.size());
				}
				return heard;
			}
			for (const auto& info : manager->subtitles) {
				if (count >= entries.size()) {
					break;
				}
				const char* text = info.subtitle.c_str();
				entries[count++] = Entry{ info.speaker, text ? std::string{ text } : std::string{} };
			}
		}

		// The player's own partner is their conversation (heard through the dialogue
		// hook), not a scene.
		RE::TESObjectREFR* partner = nullptr;
		if (auto* topics = RE::MenuTopicManager::GetSingleton()) {
			partner = topics->speaker.get().get();
		}

		const auto                              clock = Clock::now();
		std::unordered_map<RE::FormID, Showing> now;
		for (std::size_t i = 0; i < count; ++i) {
			auto& entry = entries[i];
			auto  ref = entry.speaker.get();
			auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor || actor == static_cast<RE::Actor*>(player) || entry.text.empty()) {
				continue;
			}

			const auto id = actor->GetFormID();
			const auto previous = showing.find(id);
			const bool fresh = previous == showing.end() || previous->second.text != entry.text;

			// Combat barks aren't conversation; skip anyone fighting.
			const bool counted = ref.get() != partner && !actor->IsInCombat();

			Showing shown = fresh ? Showing{ std::move(entry.text), clock, counted } : std::move(previous->second);
			if (fresh && counted) {
				const auto  line = Record(actor);
				const auto* scene = actor->GetCurrentScene();
				const char* name = actor->GetName();
				if (firstHeardReported.Take()) {
					Log::Info(Log::Category::kDialogue,
						"Hearing other people's lines through the engine's subtitles."sv);
				}
				// The scene and its phase, which tells whether a line is story or filler (see
				// SceneScript.h).
				const std::string where = scene ?
					fmt::format(" | in a scene ({:08X} phase {})", scene->GetFormID(),
						static_cast<std::int32_t>(scene->currentPhaseIndex)) :
					std::string{};
				Log::Info(Log::Category::kDialogue, "Overheard | {} [{:08X}]{}{} | \"{}\""sv,
					(name && *name) ? name : "<unnamed>", id,
					line.toPlayer ? " | to you"sv : ""sv,
					where,
					shown.text.size() > 90 ? shown.text.substr(0, 90) + "..." : shown.text);
				heard.push_back(HeardLine{ actor->GetHandle(), shown.text });
			}
			now[id] = std::move(shown);
		}

		showing = std::move(now);
		return heard;
	}

	bool SceneWatch::Talking(const RE::Actor* a_actor)
	{
		return a_actor && showing.contains(a_actor->GetFormID());
	}

	RE::NiPointer<RE::Actor> SceneWatch::Listener(RE::Actor* a_speaker)
	{
		if (!a_speaker) {
			return {};
		}
		std::string_view how{};
		return ListenerOf(a_speaker, how);
	}

	std::vector<RE::ActorHandle> SceneWatch::RecentSpeakers(float a_maxAge)
	{
		std::vector<RE::ActorHandle> speakers;
		std::vector<RE::FormID>      seen;
		const auto                   now = Clock::now();

		const std::scoped_lock guard{ lock };
		for (std::size_t i = 0; i < stored; ++i) {
			const auto& line = lines[(next + kKept - 1 - i) % kKept];
			if (SecondsSince(line.at, now) > a_maxAge) {
				break;
			}
			if (std::find(seen.begin(), seen.end(), line.id) == seen.end()) {
				seen.push_back(line.id);
				speakers.push_back(line.speaker);
			}
		}
		return speakers;
	}

	std::optional<SceneCast> SceneWatch::Find(float a_range, bool a_strict, float a_viewCos, float a_maxAge)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return std::nullopt;
		}

		// Newest first, copied out so nothing below holds the lock while querying
		// actors.
		std::array<Line, kKept> recent{};
		std::size_t             count = 0;
		{
			const std::scoped_lock guard{ lock };
			for (std::size_t i = 0; i < stored; ++i) {
				recent[count++] = lines[(next + kKept - 1 - i) % kKept];
			}
		}
		if (count == 0) {
			return std::nullopt;
		}

		const auto now = Clock::now();
		if (SecondsSince(recent[0].at, now) > a_maxAge) {
			return std::nullopt;  // nobody has said anything lately
		}

		// The player's own partner is the player's conversation, not a scene.
		RE::TESObjectREFR* partner = nullptr;
		if (auto* manager = RE::MenuTopicManager::GetSingleton()) {
			partner = manager->speaker.get().get();
		}

		const auto here = player->GetPosition();

		// The same lines, in the form the back-and-forth test reads.
		std::array<HeardTurn, kKept> turns{};
		for (std::size_t i = 0; i < count; ++i) {
			turns[i] = HeardTurn{ recent[i].id, SecondsSince(recent[i].at, now), recent[i].toPlayer };
		}
		const std::span<const HeardTurn> history{ turns.data(), count };

		for (std::size_t i = 0; i < count; ++i) {
			const auto& line = recent[i];
			if (SecondsSince(line.at, now) > a_maxAge) {
				break;  // newest first, so everything after this is older still
			}

			auto speaker = line.speaker.get();
			if (!Usable(speaker.get(), player) || speaker.get() == partner) {
				continue;
			}

			std::string_view how{};
			auto             listener = ListenerOf(speaker.get(), how);
			const auto*      scene = speaker->GetCurrentScene();

			// Outside a scene, a line said to the player never starts anything (a
			// greeting, an aside, a merchant). Inside one it can, with a second voice from
			// the same scene (see StrictPair).
			if (a_strict && line.toPlayer && !scene) {
				continue;
			}

			// The other half of the exchange: someone else who spoke recently and is
			// standing with them. The one actually being addressed wins if several
			// qualify. In strict mode the pair also has to be a real conversation: both in
			// the same game scene, or a quick back-and-forth with none of it said to the
			// player. See StrictPair and Exchanges.
			RE::NiPointer<RE::Actor> other{};
			std::string_view         strictHow{};
			for (std::size_t j = 0; j < count; ++j) {
				const auto& reply = recent[j];
				if (reply.id == line.id || SecondsSince(reply.at, now) > kExchangeSeconds) {
					continue;
				}
				auto candidate = reply.speaker.get();
				if (!Usable(candidate.get(), player) || candidate.get() == partner ||
					Distance(candidate->GetPosition(), speaker->GetPosition()) > kPairReach) {
					continue;
				}

				std::string_view why{};
				if (a_strict) {
					const bool sameScene = scene && candidate->GetCurrentScene() == scene;
					const int  exchanges = sameScene || line.toPlayer || reply.toPlayer ?
						0 :
						Exchanges(history, line.id, reply.id, kReplyGapSeconds);
					switch (StrictPair(sameScene, line.toPlayer, reply.toPlayer, exchanges)) {
					case StrictPairing::kOneScene:
						why = "the two of them in one scene"sv;
						break;
					case StrictPairing::kOneSceneForYou:
						why = "the two of them in one scene, addressing you"sv;
						break;
					case StrictPairing::kBackAndForth:
						why = "a back-and-forth between them"sv;
						break;
					case StrictPairing::kNone:
						continue;
					}
				}

				const bool addressed = candidate.get() == listener.get();
				if (!other || addressed) {
					other = candidate;
					strictHow = why;
				}
				if (addressed) {
					break;
				}
			}

			const bool exchange = static_cast<bool>(other);
			if (exchange) {
				how = a_strict ? strictHow : "both of them speaking"sv;
			} else if (a_strict) {
				continue;
			} else if (listener && listener.get() != player && Usable(listener.get(), player) &&
					   listener.get() != partner &&
					   Distance(listener->GetPosition(), speaker->GetPosition()) <= kPairReach) {
				other = listener;
			} else {
				// One NPC talking to nobody in particular, or to the player: the player is the
				// other half of the frame.
				other = RE::NiPointer<RE::Actor>{ player };
				how = "you, as their audience"sv;
			}

			const float speakerDistance = Distance(here, speaker->GetPosition());
			const float otherDistance = other.get() == player ? speakerDistance : Distance(here, other->GetPosition());
			if (std::min(speakerDistance, otherDistance) > a_range) {
				continue;
			}

			// In front of the camera: either of them, or the space between them.
			if (a_viewCos > -1.0f) {
				const auto first = HeadOf(speaker.get());
				bool       seen = InView(first, a_viewCos);
				if (!seen && other.get() != player) {
					const auto second = HeadOf(other.get());
					const RE::NiPoint3 middle{ (first.x + second.x) * 0.5f, (first.y + second.y) * 0.5f,
						(first.z + second.z) * 0.5f };
					seen = InView(second, a_viewCos) || InView(middle, a_viewCos);
				}
				if (!seen) {
					continue;
				}
			}

			SceneCast cast{};
			cast.speaker = speaker->GetHandle();
			cast.other = other->GetHandle();
			cast.exchange = exchange;
			cast.how = how;
			cast.key = PairKey(speaker->GetFormID(), other->GetFormID());
			cast.sinceLine = SecondsSince(line.at, now);
			return cast;
		}

		return std::nullopt;
	}

	void SceneWatch::Reset()
	{
		const std::scoped_lock guard{ lock };
		lines = {};
		next = 0;
		stored = 0;
		showing.clear();
	}
}

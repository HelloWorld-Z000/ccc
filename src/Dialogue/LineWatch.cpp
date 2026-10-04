#include "SD/Dialogue/LineWatch.h"

#include "SD/Camera/Director.h"
#include "SD/Core/Logging.h"
#include "SD/Dialogue/SceneWatch.h"

#include <chrono>

namespace SD::Dialogue
{
	namespace
	{
		using Emotion = RE::EmotionType;

		std::atomic_bool installed{ false };

		// The line being tracked, identified by response pointer. The hook may be
		// called once per line or every frame; a new pointer opens a line and a repeat
		// increments a counter, which is logged.
		const RE::DialogueResponse*           activeResponse{ nullptr };
		std::chrono::steady_clock::time_point activeSince{};
		std::uint32_t                         activeCalls{ 0 };
		std::uint32_t                         lineOrdinal{ 0 };

		// Who owns the line being tracked. The engine calls UpdateInDialogue(null) on
		// actors that aren't speaking (including the listener) shortly after a line
		// starts, so a null only ends the line when it comes from the speaker.
		RE::FormID activeSpeakerID{ 0 };

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

		std::string Condense(const char* a_raw, std::size_t a_max)
		{
			if (!a_raw || !*a_raw) {
				return "<no text>"s;
			}
			std::string text{ a_raw };
			std::replace(text.begin(), text.end(), '\n', ' ');
			std::replace(text.begin(), text.end(), '\r', ' ');
			if (text.size() > a_max) {
				text.resize(a_max);
				text += "..."sv;
			}
			return text;
		}

		std::string NameOf(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return "<null>"s;
			}
			const char* name = a_actor->GetName();
			return (name && *name) ? std::string{ name } : "<unnamed>"s;
		}

		void CloseActiveLine()
		{
			if (!activeResponse) {
				return;
			}

			// No duration is reported here: the engine calls UpdateInDialogue(null) on the
			// same actor about a tenth of a second after the line starts, while it still
			// has seconds to run. This hook reports line starts; Session tracks duration
			// through MenuTopicManager::currentTopicInfo.
			activeResponse = nullptr;
			activeSpeakerID = 0;
			activeCalls = 0;
		}

		void OnLine(RE::Actor* a_speaker, RE::DialogueResponse* a_response)
		{
			const RE::FormID speakerID = a_speaker ? a_speaker->GetFormID() : 0u;

			if (!a_response) {
				if (activeResponse && speakerID == activeSpeakerID) {
					CloseActiveLine();
				} else if (activeResponse) {
					// Logged, to see how often a non-speaker sends null mid-line.
					Log::Info(Log::Category::kDialogue,
						"Ignored null from {} [{:08X}] while line {} is held by [{:08X}]."sv,
						NameOf(a_speaker), speakerID, lineOrdinal, activeSpeakerID);
				}
				return;
			}

			if (a_response == activeResponse) {
				++activeCalls;
				return;
			}

			CloseActiveLine();

			activeResponse = a_response;
			activeSpeakerID = speakerID;
			activeSince = std::chrono::steady_clock::now();
			activeCalls = 1;
			++lineOrdinal;

			auto*      player = RE::PlayerCharacter::GetSingleton();
			const bool byPlayer = a_speaker && player && a_speaker == static_cast<RE::Actor*>(player);

			const char* voice = a_response->voice.c_str();

			Log::Info(Log::Category::kDialogue,
				"Cue {} | {} [{:08X}]{} | {}({}%) | emo={} idles spk={} lsn={} | voice={} | \"{}\""sv,
				lineOrdinal,
				NameOf(a_speaker),
				a_speaker ? a_speaker->GetFormID() : 0u,
				byPlayer ? " (PLAYER)"sv : ""sv,
				EmotionName(a_response->animFaceArchType.get()),
				a_response->percent,
				a_response->useEmotion ? "y"sv : "n"sv,
				a_response->speakerIdle ? "y"sv : "n"sv,
				a_response->listenIdle ? "y"sv : "n"sv,
				(voice && *voice) ? voice : "<none>",
				Condense(a_response->text.c_str(), 90));

			// Every NPC line is kept for a while, so two NPCs talking can be filmed. See
			// SceneWatch.
			if (!byPlayer) {
				SceneWatch::OnLine(a_speaker, a_response);
			}

			// The cue tells the Director a reply has started and carries the intensity
			// that decides whether it earns a close-up.
			Camera::Director::OnCue(a_speaker, a_response);
		}

		// Separate instantiations so Character and PlayerCharacter keep their own
		// original; a shared static would call whichever was written last.
		template <std::size_t Slot>
		struct UpdateInDialogueHook
		{
			static bool thunk(RE::Actor* a_this, RE::DialogueResponse* a_response, bool a_unused)
			{
				const bool result = func(a_this, a_response, a_unused);
				OnLine(a_this, a_response);
				return result;
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		constexpr std::size_t kUpdateInDialogueIndex = 0x4C;
	}

	void LineWatch::Install()
	{
		if (installed.load(std::memory_order_relaxed)) {
			return;
		}

		REL::Relocation<std::uintptr_t> character{ RE::VTABLE_Character[0] };
		UpdateInDialogueHook<0>::func =
			character.write_vfunc(kUpdateInDialogueIndex, UpdateInDialogueHook<0>::thunk);

		REL::Relocation<std::uintptr_t> playerCharacter{ RE::VTABLE_PlayerCharacter[0] };
		UpdateInDialogueHook<1>::func =
			playerCharacter.write_vfunc(kUpdateInDialogueIndex, UpdateInDialogueHook<1>::thunk);

		installed.store(true, std::memory_order_relaxed);
		Log::Info(Log::Category::kDialogue,
			"Line watch installed on Character and PlayerCharacter UpdateInDialogue (vfunc 0x4C)."sv);
	}

	bool LineWatch::Installed() noexcept
	{
		return installed.load(std::memory_order_relaxed);
	}
}

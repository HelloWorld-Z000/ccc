#include "SD/Scene/StopWork.h"

#include "SD/Core/Logging.h"

#include <random>

namespace SD::Scene
{
	namespace
	{
		enum class Phase
		{
			kIdle,       // no conversation
			kWaiting,    // staged; the clock starts when they start talking
			kVerifying,  // asked them to stop; checking it took
			kDone        // asked, or found nothing to ask; quiet until next time
		};

		bool                enabled{ true };
		RE::ActorHandle     npc{};
		Phase               phase{ Phase::kIdle };
		bool                started{ false };
		float               elapsed{ 0.0f };
		float               delay{ 4.0f };
		float               verifyIn{ 0.0f };
		RE::ObjectRefHandle stoppedFurniture{};
		RE::TESIdleForm*    stoppedIdle{ nullptr };
		std::string         stoppedWhat;

		// 3 to 5 seconds after they start speaking, randomized so it doesn't land on
		// the same beat every time.
		constexpr float kMinDelay = 3.0f;
		constexpr float kMaxDelay = 5.0f;

		// Long enough for the slowest vanilla exit animation (the tanning rack) to
		// finish.
		constexpr float kVerifySeconds = 3.5f;

		// Work, by the keywords work furniture carries (matched on the keyword's
		// editor ID, which keywords keep at runtime). The bench type check covers
		// crafting stations; these cover chores that aren't workbenches (chopping
		// blocks, ore veins, mills, churns, spits).
		constexpr std::array kWorkWords{
			"Crafting"sv, "Smelter"sv, "Forge"sv, "Anvil"sv, "Sharpening"sv, "Grindstone"sv,
			"Tanning"sv, "Cook"sv, "Alchemy"sv, "Enchant"sv, "Pickaxe"sv, "Chopping"sv,
			"WoodChop"sv, "Mill"sv, "Churn"sv, "Carpenter"sv, "Workbench"sv, "Smithing"sv,
			"Spit"sv,
		};

		// Idle loops that are conversation or posture rather than work.
		constexpr std::array kNotWorkWords{
			"Dialogue"sv, "Talk"sv, "Gesture"sv, "Greet"sv, "Nod"sv, "Shrug"sv, "Wave"sv,
			"Point"sv, "Laugh"sv, "Salute"sv, "Bow"sv, "Clap"sv, "Cheer"sv,
			"Sit"sv, "Lean"sv, "Lay"sv, "Lounge"sv, "Sleep"sv, "Bed"sv, "Chair"sv,
		};

		[[nodiscard]] char Lower(char a_c)
		{
			return (a_c >= 'A' && a_c <= 'Z') ? static_cast<char>(a_c - 'A' + 'a') : a_c;
		}

		[[nodiscard]] bool ContainsNoCase(std::string_view a_haystack, std::string_view a_needle)
		{
			if (a_needle.empty() || a_haystack.size() < a_needle.size()) {
				return false;
			}
			for (std::size_t i = 0; i + a_needle.size() <= a_haystack.size(); ++i) {
				bool match = true;
				for (std::size_t j = 0; j < a_needle.size(); ++j) {
					if (Lower(a_haystack[i + j]) != Lower(a_needle[j])) {
						match = false;
						break;
					}
				}
				if (match) {
					return true;
				}
			}
			return false;
		}

		template <std::size_t N>
		[[nodiscard]] bool MatchesAny(std::string_view a_name, const std::array<std::string_view, N>& a_words)
		{
			for (const auto& word : a_words) {
				if (ContainsNoCase(a_name, word)) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] float RollDelay()
		{
			static std::minstd_rand rng{ std::random_device{}() };
			std::uniform_real_distribution<float> range{ kMinDelay, kMaxDelay };
			return range(rng);
		}

		[[nodiscard]] std::string_view NameOf(RE::Actor* a_actor)
		{
			const char* name = a_actor ? a_actor->GetName() : nullptr;
			return (name && *name) ? std::string_view{ name } : "<unnamed>"sv;
		}

		[[nodiscard]] RE::TESFurniture* FurnitureOf(RE::Actor* a_actor, RE::ObjectRefHandle& a_handle)
		{
			a_handle = a_actor ? a_actor->GetOccupiedFurniture() : RE::ObjectRefHandle{};
			auto  ref = a_handle.get();
			auto* base = ref ? ref->GetBaseObject() : nullptr;
			return base ? base->As<RE::TESFurniture>() : nullptr;
		}

		// Name for the log: the furniture's name and the keyword that marked it as
		// work.
		[[nodiscard]] std::string Describe(const RE::TESFurniture* a_furniture)
		{
			std::string out;
			const char* name = a_furniture ? a_furniture->GetName() : nullptr;
			out = (name && *name) ? name : "furniture";
			if (!a_furniture) {
				return out;
			}
			for (std::uint32_t i = 0; i < a_furniture->numKeywords; ++i) {
				const auto* keyword = a_furniture->keywords ? a_furniture->keywords[i] : nullptr;
				const char* id = keyword ? keyword->GetFormEditorID() : nullptr;
				if (id && *id && MatchesAny(id, kWorkWords)) {
					out += " ("sv;
					out += id;
					out += ")"sv;
					return out;
				}
			}
			return out;
		}

		[[nodiscard]] bool IdleLooping(RE::Actor* a_actor)
		{
			bool playing = false;
			return a_actor && a_actor->GetGraphVariableBool("bIdlePlaying", playing) && playing;
		}

		[[nodiscard]] RE::TESIdleForm* LastIdle(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process && process->middleHigh ? process->middleHigh->lastIdlePlayed : nullptr;
		}

		[[nodiscard]] std::string_view IdleName(const RE::TESIdleForm* a_idle)
		{
			if (!a_idle) {
				return {};
			}
			if (const char* id = a_idle->GetFormEditorID(); id && *id) {
				return id;
			}
			const char* event = a_idle->animEventName.c_str();
			return (event && *event) ? std::string_view{ event } : std::string_view{};
		}

		void Stop(RE::Actor* a_actor, const char* a_event)
		{
			if (a_actor->NotifyAnimationGraph(a_event)) {
				return;
			}
			// The graph had no transition for the event from where it is; try the engine's
			// own idle stop.
			if (auto* process = a_actor->GetActorRuntimeData().currentProcess) {
				process->StopCurrentIdle(a_actor, true);
			}
		}

		void Attempt(RE::Actor* a_actor)
		{
			phase = Phase::kDone;
			const auto who = NameOf(a_actor);

			// Leave quest scenes alone (pulling someone off furniture a scene put them on
			// can stall it), and anyone dead, fighting or mounted.
			if (a_actor->IsDead() || a_actor->IsInCombat() || a_actor->IsOnMount() || a_actor->GetCurrentScene()) {
				Log::Info(Log::Category::kStaging,
					"Stop work: {} is in a scene, mounted or fighting; left as they are."sv, who);
				return;
			}

			RE::ObjectRefHandle handle{};
			if (const auto* furniture = FurnitureOf(a_actor, handle)) {
				if (!StopWork::IsWorkFurniture(furniture)) {
					return;  // sitting, lying, leaning: not an action
				}
				stoppedWhat = Describe(furniture);
				stoppedFurniture = handle;
				stoppedIdle = nullptr;
				Stop(a_actor, "IdleFurnitureExit");
				phase = Phase::kVerifying;
				verifyIn = kVerifySeconds;
				Log::Info(Log::Category::kStaging,
					"Stop work: {} has been talking {:.1f}s at {}; asking them to step away from it."sv,
					who, elapsed, stoppedWhat);
				return;
			}

			if (IdleLooping(a_actor)) {
				auto* idle = LastIdle(a_actor);
				const auto name = IdleName(idle);
				if (name.empty() || MatchesAny(name, kNotWorkWords)) {
					return;
				}
				stoppedWhat = name;
				stoppedFurniture = {};
				stoppedIdle = idle;
				Stop(a_actor, "IdleStop");
				phase = Phase::kVerifying;
				verifyIn = kVerifySeconds;
				Log::Info(Log::Category::kStaging,
					"Stop work: {} has been talking {:.1f}s while playing {}; stopping it."sv,
					who, elapsed, stoppedWhat);
			}
		}

		void Verify(RE::Actor* a_actor)
		{
			phase = Phase::kDone;
			const auto who = NameOf(a_actor);

			if (stoppedIdle) {
				if (IdleLooping(a_actor) && LastIdle(a_actor) == stoppedIdle) {
					// For a pose the exit didn't end.
					a_actor->NotifyAnimationGraph("IdleForceDefaultState");
					Log::Info(Log::Category::kStaging,
						"Stop work: {} was still playing {}; reset to standing."sv, who, stoppedWhat);
				} else {
					Log::Info(Log::Category::kStaging, "Stop work: {} stopped {}."sv, who, stoppedWhat);
				}
				return;
			}

			RE::ObjectRefHandle handle{};
			const auto* furniture = FurnitureOf(a_actor, handle);
			const auto* state = a_actor->AsActorState();
			const bool  stillThere = furniture && handle == stoppedFurniture && state &&
				state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSitting;
			if (stillThere) {
				// Not escalated: StopInteractingQuick would teleport them out of the
				// furniture, which looks worse than a smith who keeps working.
				Log::Warn(Log::Category::kStaging,
					"Stop work: {} is still at {}; the furniture refused the exit, so they keep working."sv,
					who, stoppedWhat);
			} else {
				Log::Info(Log::Category::kStaging, "Stop work: {} stepped away from {}."sv, who, stoppedWhat);
			}
		}
	}

	bool StopWork::IsWorkFurniture(const RE::TESFurniture* a_furniture)
	{
		if (!a_furniture) {
			return false;
		}
		using BenchType = RE::TESFurniture::WorkBenchData::BenchType;
		if (a_furniture->workBenchData.benchType.get() != BenchType::kNone) {
			return true;
		}
		for (std::uint32_t i = 0; i < a_furniture->numKeywords; ++i) {
			const auto* keyword = a_furniture->keywords ? a_furniture->keywords[i] : nullptr;
			const char* id = keyword ? keyword->GetFormEditorID() : nullptr;
			if (id && *id && MatchesAny(id, kWorkWords)) {
				return true;
			}
		}
		return false;
	}

	void StopWork::Begin(RE::Actor* a_npc)
	{
		const auto handle = a_npc ? a_npc->GetHandle() : RE::ActorHandle{};
		if (handle && handle == npc && phase != Phase::kIdle) {
			return;  // re-staging the same conversation keeps its clock
		}
		npc = handle;
		phase = handle ? Phase::kWaiting : Phase::kIdle;
		started = false;
		elapsed = 0.0f;
		delay = RollDelay();
		verifyIn = 0.0f;
		stoppedFurniture = {};
		stoppedIdle = nullptr;
		stoppedWhat.clear();
	}

	void StopWork::Update(float a_delta, bool a_npcSpeaking)
	{
		if (!enabled || phase == Phase::kIdle || phase == Phase::kDone) {
			return;
		}

		auto  actorPtr = npc.get();
		auto* actor = actorPtr.get();
		if (!actor || !actor->Is3DLoaded()) {
			phase = Phase::kDone;
			return;
		}

		if (!started) {
			if (!a_npcSpeaking) {
				return;
			}
			started = true;
			elapsed = 0.0f;
		}

		const float step = std::max(a_delta, 0.0f);
		elapsed += step;

		if (phase == Phase::kWaiting && elapsed >= delay) {
			Attempt(actor);
		} else if (phase == Phase::kVerifying) {
			verifyIn -= step;
			if (verifyIn <= 0.0f) {
				Verify(actor);
			}
		}
	}

	void StopWork::End()
	{
		npc = {};
		phase = Phase::kIdle;
		started = false;
		stoppedFurniture = {};
		stoppedIdle = nullptr;
	}

	void StopWork::SetEnabled(bool a_enabled) noexcept
	{
		enabled = a_enabled;
	}
}

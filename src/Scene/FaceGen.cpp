#include "SD/Scene/FaceGen.h"

#include "SD/Core/Logging.h"
#include "SD/Scene/LipSync.h"
#include "SD/Scene/Performance.h"
#include "SD/Scene/UpperFace.h"
#include "SD/Scene/RegionalFace.h"
#include "SD/Scene/FaceTest.h"
#include "SD/Scene/UpperFaceTest.h"

#include <mutex>

namespace SD::Scene
{
	namespace
	{
		std::atomic_bool installed{ false };
		std::mutex modifierMutex;
		RE::NiPointer<RE::BSFaceGenNiNode> modifierOwner{};
		RE::BSFaceGenAnimationData* modifierData{};
		float* modifierValues{};
		UpperFaceTest::OwnedPose modifierPose;
		std::mutex listenerMutex;
		RE::NiPointer<RE::BSFaceGenNiNode> listenerOwner;
		RE::BSFaceGenAnimationData* listenerData{};
		float* listenerValues{};
		Listener::OwnedPose listenerPose;

		// Requires listenerMutex. Debts belong to one exact animation buffer.
		void ReleaseListenerOwner()
		{
			if (listenerOwner) {
				auto* data = listenerOwner->GetRuntimeData().animationData.get();
				if (data && data == listenerData && data->expressionKeyFrame.values == listenerValues &&
					data->expressionKeyFrame.count == Expressions::kSlots) {
					auto& keys = data->expressionKeyFrame;
					Expressions::Shape live{};
					std::copy_n(keys.values, live.size(), live.begin());
					const auto restored = listenerPose.Release(live);
					for (std::uint32_t i = 0; i < Expressions::kSlots; ++i) {
						if (restored[i] != live[i]) keys.SetValue(i, restored[i]);
					}
				}
			}
			listenerOwner.reset();
			listenerData = nullptr;
			listenerValues = nullptr;
			listenerPose = {};
		}

		void ApplyListener(RE::BSFaceGenNiNode* node, const float* values, bool active)
		{
			const std::lock_guard lock(listenerMutex);
			auto* data = node ? node->GetRuntimeData().animationData.get() : nullptr;
			const bool valid = data && data->expressionKeyFrame.values && data->expressionKeyFrame.count == Expressions::kSlots;
			if (!active || !valid || listenerOwner.get() != node || listenerData != data ||
				listenerValues != data->expressionKeyFrame.values) ReleaseListenerOwner();
			if (!active || !valid) return;
			auto& keys = data->expressionKeyFrame;
			Expressions::Shape live{}, target{};
			std::copy_n(keys.values, live.size(), live.begin());
			std::copy_n(values, target.size(), target.begin());
			const auto out = listenerPose.Apply(live, target);
			listenerOwner.reset(node);
			listenerData = data;
			listenerValues = keys.values;
			for (std::uint32_t i = 0; i < Expressions::kSlots; ++i) keys.SetValue(i, out[i]);
		}

		// Called under modifierMutex. Do not restore a stale snapshot over another
		// writer's new pose, and do not touch blink/gaze/phonemes during handback.
		void ReleaseModifierOwner()
		{
			if (modifierOwner) {
				if (auto* data = modifierOwner->GetRuntimeData().animationData.get();
					data && data == modifierData && data->modifierKeyFrame.values == modifierValues) {
					auto& keyframe = data->modifierKeyFrame;
					UpperFace::Shape live{};
					for (std::size_t i = 0; i < UpperFace::kSlots.size(); ++i) {
						const auto slot = UpperFace::kSlots[i];
						if (keyframe.values && slot < keyframe.count) live[i] = keyframe.values[slot];
					}
					const auto restored = modifierPose.Release(live);
					for (std::size_t i = 0; i < UpperFace::kSlots.size(); ++i) {
						const auto slot = UpperFace::kSlots[i];
						if (keyframe.values && slot < keyframe.count &&
							restored[i] != live[i]) {
							keyframe.SetValue(slot, restored[i]);
						}
					}
				}
			}
			modifierOwner.reset();
			modifierData = nullptr;
			modifierValues = nullptr;
			modifierPose = {};
		}

		void ApplyModifiers(RE::BSFaceGenNiNode* node, const float* values, bool active)
		{
			const std::lock_guard lock(modifierMutex);
			auto* data = node ? node->GetRuntimeData().animationData.get() : nullptr;
			const bool valid = data && data->modifierKeyFrame.values && data->modifierKeyFrame.count > UpperFace::kSlots.back();
			if (!active || !valid || modifierOwner.get() != node || modifierData != data ||
				modifierValues != data->modifierKeyFrame.values) ReleaseModifierOwner();
			if (!active || !valid) return;
			{
				auto& keyframe = data->modifierKeyFrame;
				UpperFace::Shape live{}, target{};
				for (std::size_t i = 0; i < UpperFace::kSlots.size(); ++i) {
					live[i] = keyframe.values[UpperFace::kSlots[i]];
					target[i] = values[i];
				}
				modifierPose.Apply(live, target);
				modifierOwner.reset(node);
				modifierData = data;
				modifierValues = keyframe.values;
				for (std::size_t i = 0; i < UpperFace::kSlots.size(); ++i) {
					const auto slot = UpperFace::kSlots[i];
					if (slot < keyframe.count) keyframe.SetValue(slot, values[i]);
				}
			}
		}

		// Skyrim's expression list: 7 dialogue expressions, MoodNeutral, 8 mood
		// variants and 2 combat ones. The face probe reports n=17 on this channel.
		constexpr std::uint32_t kExpressionSlots = 17;

		// What one head did over the counting window. Call counts rather than samples,
		// so a head morphed thirty times can be told from one morphed once.
		struct Counts
		{
			std::uint64_t calls{ 0 };          // UpdateDownwardPass entered
			std::uint64_t advanced{ 0 };       // ...and lastTime moved: it did work
			std::uint64_t consumedPhoneme{ 0 };// ...and isUpdated went true -> false
			std::uint64_t sawPending{ 0 };     // isUpdated was true on entry
			float         lastTimeSeen{ -1.0f };
			float         timeArg{ -1.0f };
			float         phonemePeakIn{ 0.0f };

			// Peak over the whole window, not the last frame's (which reads 0 for a mouth
			// that just came to rest).
			float peakMax{ 0.0f };

			// Every slot that held a non-zero value, one bit each. A viseme track lights
			// several of the 16 slots over a line; a single stuck value lights one.
			std::uint32_t slotsSeen{ 0 };
		};

		[[nodiscard]] std::uint32_t SlotCount(std::uint32_t a_mask) noexcept
		{
			std::uint32_t n = 0;
			for (; a_mask; a_mask &= a_mask - 1) {
				++n;
			}
			return n;
		}

		Counts playerCounts{};
		Counts npcCounts{};
		struct FinalState
		{
			UpperFace::Shape modifiers{};
			float expressionPeak{ 0.0f };
			float phonemePeak{ 0.0f };
		};
		std::mutex finalMutex;
		FinalState playerFinal{}, npcFinal{};

		// Resolved once per window; a pointer compare is cheap, and this runs for
		// every head in the cell.
		RE::NiPointer<RE::BSFaceGenNiNode> playerFace{};
		RE::NiPointer<RE::BSFaceGenNiNode> npcFace{};
		RE::ActorHandle npcActor{};

		std::atomic_bool counting{ false };
		float            secondCountdown{ 0.0f };
		std::uint32_t    secondsElapsed{ 0 };

		// Diagnostic: -1 off; 0-15 pins that viseme slot fully open on the player's
		// head every frame. If the mouth visibly moves, the channel reaches the
		// geometry and any problem is upstream in what feeds the viseme track. If it
		// doesn't, the problem is on the head itself (morph data, a head replacer, or
		// SMP hair/beard/headgear). Needs no conversation or voice mod.
		std::atomic_int forcedViseme{ -1 };

		[[nodiscard]] float PeakOf(const RE::BSFaceGenKeyframeMultiple& a_keyframe,
			std::uint32_t* a_slots = nullptr)
		{
			if (!a_keyframe.values || a_keyframe.count == 0 || a_keyframe.count > 256) {
				return -1.0f;
			}
			float peak = 0.0f;
			for (std::uint32_t i = 0; i < a_keyframe.count; ++i) {
				const float value = a_keyframe.values[i];
				peak = std::max(peak, value);
				if (a_slots && value > 0.0f && i < 32) {
					*a_slots |= 1u << i;
				}
			}
			return peak;
		}

		void Observe(RE::BSFaceGenNiNode* a_node, Counts& a_counts, const RE::NiUpdateData& a_data,
			float a_lastTimeBefore, bool a_pendingBefore, float a_peakBefore,
			std::uint32_t a_slotsBefore)
		{
			const auto& runtime = a_node->GetRuntimeData();
			a_counts.calls++;
			a_counts.timeArg = a_data.time;
			a_counts.lastTimeSeen = runtime.lastTime;
			a_counts.phonemePeakIn = a_peakBefore;
			a_counts.peakMax = std::max(a_counts.peakMax, a_peakBefore);
			a_counts.slotsSeen |= a_slotsBefore;

			if (runtime.lastTime != a_lastTimeBefore) {
				a_counts.advanced++;
			}

			if (a_pendingBefore) {
				a_counts.sawPending++;
				if (auto* data = runtime.animationData.get();
					data && !data->phenomeKeyFrame.isUpdated) {
					a_counts.consumedPhoneme++;
				}
			}
		}

		struct FaceGenDownwardPass
		{
			static void thunk(RE::BSFaceGenNiNode* a_this, RE::NiUpdateData& a_data, std::uint32_t a_arg2)
			{
				// The common path; runs for every facegen head in the cell every frame, so it
				// stays at two pointer compares. The player is tracked while forcing even
				// outside a conversation (see forcedViseme).
				const bool active = counting.load(std::memory_order_relaxed);
				const bool isPlayer = a_this == playerFace.get();
				const bool isNpc = !isPlayer && active && a_this == npcFace.get();
				if (!isPlayer && !isNpc) {
					func(a_this, a_data, a_arg2);
					return;
				}
				const int forced = forcedViseme.load(std::memory_order_relaxed);
				UpperFace::Shape testValues{};
				const bool testing = isPlayer && FaceTest::Sample(testValues);
				float mouth[16]{};
				const bool speaking = isPlayer && LipSync::Sample(mouth, 16);
				float expression[kExpressionSlots]{};
				const bool emoting = !testing && Performance::SampleExpression(expression, kExpressionSlots, isPlayer);
				float listener[kExpressionSlots]{};
				const bool listening = isPlayer && !testing && !emoting && forced < 0 && !speaking &&
					Performance::SampleListenerExpression(listener, kExpressionSlots);
				UpperFace::Shape brows{};
				const bool upperFace = isPlayer && Performance::SampleUpperFace(brows.data(), static_cast<std::uint32_t>(brows.size()));

				auto&       runtime = a_this->GetRuntimeData();
				const float lastTimeBefore = runtime.lastTime;

				// Written before the original call, so the pass that drains the channel drains
				// this value; anything that wrote phonemes earlier this frame has been
				// overwritten.
				if (isPlayer && forced >= 0) {
					if (auto* data = runtime.animationData.get()) {
						auto& keyframe = data->phenomeKeyFrame;
						if (keyframe.values && forced < static_cast<int>(keyframe.count)) {
							keyframe.SetValue(static_cast<std::uint32_t>(forced), 1.0f);
						}
					}
				} else if (isPlayer && speaking) {
					// Synthesized lip sync, written at the same point for the same reason. Every
					// slot is written, zeros included, so a value another mod left in an unused
					// slot doesn't blend into SD's shape.
					if (auto* data = runtime.animationData.get()) {
						auto& keyframe = data->phenomeKeyFrame;
						if (keyframe.values) {
							const std::uint32_t count = std::min<std::uint32_t>(keyframe.count, 16);
							for (std::uint32_t i = 0; i < count; ++i) {
								keyframe.SetValue(i, mouth[i]);
							}
						}
					}
				}

				// Refresh both participants right before their morph consumes the expression
				// channel. Zeros are included to clear conflicting moods. Mouth, blink and
				// eye-direction channels keep their own writers.
				if (isPlayer) ApplyListener(a_this, listener, listening);
				if (emoting) {
					if (auto* data = runtime.animationData.get()) {
						auto& keyframe = data->expressionKeyFrame;
						if (keyframe.values) {
							const std::uint32_t count =
								std::min<std::uint32_t>(keyframe.count, kExpressionSlots);
							for (std::uint32_t i = 0; i < count; ++i) {
								keyframe.SetValue(i, expression[i]);
							}
						}
					}
				}

				// The brows. Unlike the channels above, the modifier channel isn't written
				// whole: slots 0-1 are blinks (paced by the engine via blinkDelay) and 8-11
				// are eye look direction (head tracking). Only the slots in UpperFace::kSlots
				// are written: brows 2-7 and squints 12-13. Expression profiles are the only
				// normal writer; only the forced full-expression diagnostic suppresses
				// modifiers.
				if (isPlayer) {
					if (emoting) brows.fill(0.0f);
					if (testing) brows = testValues;
					ApplyModifiers(a_this, brows.data(), testing || upperFace || emoting);
				}

				bool          pendingBefore = false;
				float         peakBefore = -1.0f;
				std::uint32_t slotsBefore = 0;
				if (auto* data = runtime.animationData.get()) {
					pendingBefore = data->phenomeKeyFrame.isUpdated;
					peakBefore = PeakOf(data->phenomeKeyFrame, &slotsBefore);
				}

				if (isPlayer) RegionalFace::Before(a_this);
				func(a_this, a_data, a_arg2);
				if (isPlayer) RegionalFace::After(a_this, testing);
				if (testing) FaceTest::Record(a_this);

				// Observe the final channels after the original pass, not our requested
				// inputs. Fixed-size copy only; formatting happens on Tick.
				if (active) {
					FinalState final{};
					if (auto* data = runtime.animationData.get()) {
						const auto& modifiers = data->modifier3;
						for (std::size_t i = 0; i < UpperFace::kSlots.size(); ++i) {
							const auto slot = UpperFace::kSlots[i];
							if (modifiers.values && slot < modifiers.count) final.modifiers[i] = modifiers.values[slot];
						}
						final.expressionPeak = PeakOf(data->expression3);
						final.phonemePeak = PeakOf(data->phoneme3);
					}
					const std::lock_guard lock(finalMutex);
					(isPlayer ? playerFinal : npcFinal) = final;
				}
				Observe(a_this, isPlayer ? playerCounts : npcCounts, a_data,
					lastTimeBefore, pendingBefore, peakBefore, slotsBefore);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		constexpr std::size_t kUpdateDownwardPass = 0x2C;

		void Report(std::string_view a_when)
		{
			RegionalFace::Report();
			FinalState finalPlayer{}, finalNpc{};
			{
				const std::lock_guard lock(finalMutex);
				finalPlayer = playerFinal;
				finalNpc = npcFinal;
			}
			const auto reportFinal = [](std::string_view who, const FinalState& state) {
				const auto& m = state.modifiers;
				Log::Info(Log::Category::kStaging,
					"Face final | {} | down={:.3f}/{:.3f} in={:.3f}/{:.3f} up={:.3f}/{:.3f} squint={:.3f}/{:.3f} expressionPeak={:.3f} phonemePeak={:.3f}"sv,
					who, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], state.expressionPeak, state.phonemePeak);
			};
			reportFinal("player"sv, finalPlayer);
			reportFinal("npc"sv, finalNpc);
			const auto line = [](std::string_view a_who, const Counts& a_counts) {
				Log::Info(Log::Category::kStaging,
					"  {:<7} calls={:<6} didWork={:<6} ({:>5.1f}%)  phonemePending={:<5} consumed={:<5}  time={:.3f}  lastTime={:.2f}  peakLast={:.3f}  peakMax={:.3f}  slotsLit={}/16 (0x{:04X})"sv,
					a_who, a_counts.calls, a_counts.advanced,
					a_counts.calls ? 100.0 * static_cast<double>(a_counts.advanced) / static_cast<double>(a_counts.calls) : 0.0,
					a_counts.sawPending, a_counts.consumedPhoneme,
					a_counts.timeArg, a_counts.lastTimeSeen, a_counts.phonemePeakIn, a_counts.peakMax,
					SlotCount(a_counts.slotsSeen), a_counts.slotsSeen);
			};

			Log::Info(Log::Category::kStaging, "FaceGen morph pass | {}"sv, a_when);

			// Mentioned in every report, so a forced run isn't mistaken for a real one.
			if (const int forced = forcedViseme.load(std::memory_order_relaxed); forced >= 0) {
				Log::Warn(Log::Category::kStaging,
					"  FORCING viseme slot {} to 1.0 on the player every frame. Numbers below describe the forced state, not normal play."sv,
					forced);
			}
			line("PLAYER"sv, playerCounts);
			line("npc"sv, npcCounts);

			// The player-versus-NPC comparison, stated directly.
			if (npcCounts.calls > 0) {
				const double ratio = static_cast<double>(playerCounts.calls) / static_cast<double>(npcCounts.calls);
				if (playerCounts.calls == 0) {
					Log::Warn(Log::Category::kStaging,
						"  -> the player's head was NOT morphed at all while the NPC's was {} times. That is the bug."sv,
						npcCounts.calls);
				} else if (ratio < 0.5 || ratio > 2.0) {
					Log::Warn(Log::Category::kStaging,
						"  -> the player's head was morphed {:.2f}x as often as the NPC's."sv, ratio);
				} else {
					Log::Info(Log::Category::kStaging,
						"  -> both heads morphed at a comparable rate ({:.2f}x). The gate is not whether the pass runs."sv,
						ratio);
				}

				// Whether anything writes a real viseme track while the player's voiced line
				// plays. (Forcing slot 0 proved the channel reaches the geometry.) Meaningless
				// while forcing, since the forced slot is counted.
				if (forcedViseme.load(std::memory_order_relaxed) < 0) {
					const std::uint32_t lit = SlotCount(playerCounts.slotsSeen);
					if (lit == 0) {
						Log::Warn(Log::Category::kStaging,
							"  -> NOTHING wrote the player's viseme track this conversation (0 slots, peak {:.3f}). SpeakSound is not feeding the channel, so the mouth has nothing to play. SD would have to drive it from the .lip itself."sv,
							playerCounts.peakMax);
					} else if (lit <= 2) {
						Log::Warn(Log::Category::kStaging,
							"  -> only {} slot(s) ever lit on the player (peak {:.3f}, mask 0x{:04X}). That is a stuck value, not a track."sv,
							lit, playerCounts.peakMax, playerCounts.slotsSeen);
					} else {
						Log::Info(Log::Category::kStaging,
							"  -> a real viseme track played on the player: {} slots, peak {:.3f}. The channel is fed AND reaches the geometry, so if the mouth still looks still the amplitude is the suspect."sv,
							lit, playerCounts.peakMax);
					}
				}
			}
		}
	}

	void FaceGen::Install()
	{
		if (installed.load(std::memory_order_relaxed)) {
			return;
		}

		// 0x2C is the flat-Skyrim slot; VR's vtable differs, the same reason
		// SKSEPlugin_Load refuses VR.
		if (REL::Module::IsVR()) {
			Log::Warn(Log::Category::kCore, "FaceGen probe not installed: VR vtable layout differs."sv);
			return;
		}

		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_BSFaceGenNiNode[0] };
		FaceGenDownwardPass::func = vtable.write_vfunc(kUpdateDownwardPass, FaceGenDownwardPass::thunk);

		installed.store(true, std::memory_order_relaxed);
		Log::Info(Log::Category::kCore,
			"FaceGen morph hook installed on BSFaceGenNiNode::UpdateDownwardPass (vfunc 0x2C)."sv);
	}

	bool FaceGen::Installed() noexcept
	{
		return installed.load(std::memory_order_relaxed);
	}

	void FaceGen::SetForcedViseme(int a_slot)
	{
		const int slot = (a_slot >= 0 && a_slot <= 15) ? a_slot : -1;
		if (forcedViseme.exchange(slot, std::memory_order_relaxed) == slot) {
			return;
		}

		if (slot < 0) {
			Log::Info(Log::Category::kStaging, "Forced viseme off; the player's mouth is the engine's again."sv);
			return;
		}

		// Logged as a warning so it's obvious when it's active.
		Log::Warn(Log::Category::kStaging,
			"FORCED VISEME {}: pinning that slot to 1.0 on the player every frame. Look at the player's mouth in third person - it should be visibly stuck open. Set [Diagnostics] iForceViseme=-1 to stop."sv,
			slot);

		// The install check isn't done here: this runs during LoadSettings, before
		// Runtime installs the hook. Runtime checks once the install is final.
	}

	void FaceGen::Begin(RE::Actor* a_npc)
	{
		if (!installed.load(std::memory_order_relaxed)) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		playerFace.reset(player ? player->GetFaceNodeSkinned() : nullptr);
		npcFace.reset(a_npc ? a_npc->GetFaceNodeSkinned() : nullptr);
		npcActor = a_npc ? a_npc->GetHandle() : RE::ActorHandle{};

		playerCounts = {};
		npcCounts = {};
		secondCountdown = 1.0f;
		secondsElapsed = 0;
		counting.store(true, std::memory_order_relaxed);

		Log::Info(Log::Category::kStaging,
			"FaceGen morph counting started (player head {}, npc head {})."sv,
			playerFace ? "found"sv : "MISSING"sv,
			npcFace ? "found"sv : "MISSING"sv);
	}

	void FaceGen::End()
	{
		{
			const std::lock_guard lock(listenerMutex);
			ReleaseListenerOwner();
		}
		if (!counting.exchange(false, std::memory_order_relaxed)) {
			return;
		}
		Report("whole conversation"sv);
		playerFace.reset();
		npcFace.reset();
		npcActor = {};
	}

	void FaceGen::Tick(float a_delta)
	{
		if (!installed.load(std::memory_order_relaxed)) {
			return;
		}

		const bool active = counting.load(std::memory_order_relaxed);
		FaceTest::Tick(a_delta);
		RegionalFace::Prepare(RE::PlayerCharacter::GetSingleton());

		// The player's head is latched even outside a conversation, because the forced
		// viseme and synthesized lip sync both work without one (including with
		// bEnabled=0). Otherwise the thunk would compare against null and both would
		// silently do nothing.

		// Re-latch when the engine swaps in a new head. Otherwise the probe keeps
		// counting the old node and reports the player's head as never morphed again.
		// Performance.cpp's face hold deals with the same thing (see heldRoot).
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			if (auto* face = player->GetFaceNodeSkinned(); face && face != playerFace.get()) {
				{
					const std::lock_guard lock(listenerMutex);
					ReleaseListenerOwner();
				}
				if (playerFace) {
					Log::Warn(Log::Category::kStaging,
						"FaceGen probe: the player's head was replaced; re-latching. Counts so far are from the old node ({} calls)."sv,
						playerCounts.calls);
				} else {
					Log::Info(Log::Category::kStaging, "FaceGen: latched the player's head."sv);
				}
				playerFace.reset(face);
				if (auto* base = player->GetActorBase()) {
					if (auto* part = base->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kFace)) {
						Log::Info(Log::Category::kStaging,
							"Player face asset: headPart={:08X} model={} raceTri={} defaultTri={} chargenTri={}"sv,
							part->GetFormID(), part->GetModel(), part->morphs[0].GetModel(),
							part->morphs[1].GetModel(), part->morphs[2].GetModel());
					}
				}
			}
		}

		if (!active) {
			return;
		}

		if (auto actor = npcActor.get()) {
			auto* face = actor->GetFaceNodeSkinned();
			if (face != npcFace.get()) npcFace.reset(face);
		} else {
			npcFace.reset();
		}

		secondCountdown -= a_delta;
		if (secondCountdown > 0.0f) {
			return;
		}
		secondCountdown = 1.0f;

		// Cumulative rather than per second; a running total is easier to read down a
		// log.
		Report(fmt::format("{}s in, cumulative", ++secondsElapsed));
	}

	void FaceGen::ReleaseModifiers()
	{
		FaceTest::Cancel();
		{
			const std::lock_guard lock(listenerMutex);
			ReleaseListenerOwner();
		}
		RegionalFace::Reset();
		const std::lock_guard lock(modifierMutex);
		ReleaseModifierOwner();
	}
}

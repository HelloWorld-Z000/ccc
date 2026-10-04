#include "SD/Scene/Performance.h"

#include "SD/Core/Logging.h"
#include "SD/Core/Config.h"
#include "SD/Dialogue/Session.h"
#include "SD/Scene/ExpressionModel.h"
#include "SD/Scene/UpperFace.h"
#include "SD/Scene/FaceGen.h"

#include "SD/Scene/LipSync.h"

#include <mutex>

namespace SD::Scene
{
	namespace
	{
		using namespace Expressions;

		struct Gaze
		{
			bool         averted{ false };
			float        remaining{ 1.0f };
			RE::NiPoint3 offset{};
		};

		Gaze            npcGaze{};
		Gaze            playerGaze{};
		RE::ActorHandle npcHandle{};
		bool            engaged{ false };

		std::atomic_bool wantExpressions{ true };
		bool  wantGaze{ true };
		bool  wantHoldFace{ true };
		constexpr float intensityScale = ExpressionProfiles::kIntensity;

		// Nodes whose draw flags this mod raised, and what they held before. The exact
		// previous value is restored rather than the bits cleared, because other mods
		// (face lights, head mesh replacers, LOD tweaks) set the same flags for their
		// own reasons.
		struct HeldNode
		{
			RE::NiPointer<RE::NiAVObject> node{};
			std::uint32_t                 flags{ 0 };
		};

		std::vector<HeldNode> heldNodes{};
		Log::OnceFlag         holdReported;

		// The face node the hold was rooted at, so a swap can be noticed. The engine
		// can destroy and rebuild the player's head mid-conversation (for example when
		// a headgear mod removes a helmet); the new node arrives without the flags and
		// heldNodes would point at a dead node. Compared against GetFaceNodeSkinned()
		// each tick.
		RE::NiPointer<RE::BSFaceGenNiNode> heldRoot{};

		// kAlwaysDraw (1 << 11) takes the node out of culling; kHighDetail (1 << 24)
		// covers the case where the gate is a level-of-detail choice instead. Both are
		// draw hints on one head for the length of a conversation.
		constexpr std::uint32_t kHoldFlags =
			static_cast<std::uint32_t>(RE::NiAVObject::Flag::kAlwaysDraw) |
			static_cast<std::uint32_t>(RE::NiAVObject::Flag::kHighDetail);

		// The listener holds eye contact more than the speaker, who looks away while
		// putting a sentence together. Equal values turn it into a stare.
		float listenerHold{ 0.82f };
		float speakerHold{ 0.48f };

		// Rising-edge tracking for the player's own line; see the gaze step.
		bool playerWasTalking{ false };


		std::uint32_t rng{ 0x1F123BB5u };

		Log::OnceFlag gazeReported;

		// Game-thread envelopes. Only complete snapshots cross into the morph hook.
		struct FaceExpression
		{
			Shape base{};
			Shape current{};
			float age{ 0.0f };
			float quietFor{ 0.0f };
			bool ownsOverride{ false };
		};
		UpperFace::Performance playerUpperFace{};
		Listener::Performance playerListener{};
		FaceExpression npcExpression{};
		std::mutex expressionMutex;
		UpperFace::Shape publishedUpperFace{};
		UpperFace::Shape publishedRegionalFace{};
		Shape publishedNpc{};
		Shape publishedListener{};
		bool publishedListenerActive{ false };
		std::atomic_bool cinematicListening{ true };
		bool publishedPlayerActive{ false };
		bool publishedNpcActive{ false };
		std::atomic<std::uint32_t> playerLineEmotion{ kNeutral };
		std::atomic_int forcedExpression{ -1 };
		Reading playerReaction{};
		bool playerExprWasTalking{ false };
		std::uint64_t playerLineSerial{ 0 };
		constexpr float kReactionScale = 0.80f;

		void SetReading(FaceExpression& a_face, const Reading& a_reading, float a_scale)
		{
			const auto index = ExpressionFor(a_reading.emotion);
			const float strength = AuthoredStrength(index, a_reading.percent);
			a_face.base = MakeShape(index, strength * a_scale);
			a_face.quietFor = 0.0f;
			a_face.age = 0.0f;
		}

		void ApplyPlayerReading(const Reading& a_reading, std::string_view a_why,
			std::string_view a_detail, bool a_listening = false)
		{
			playerUpperFace.Begin(a_reading, a_listening, a_detail);
			playerLineEmotion.store(playerUpperFace.reading.emotion, std::memory_order_relaxed);
			Log::Info(Log::Category::kStaging,
				"Acting plan v2: {} | {} beat(s), estimated {:.2f}s | {}"sv,
				a_why, playerUpperFace.plan.count, playerUpperFace.duration, a_detail);
			for (std::size_t i = 0; i < playerUpperFace.plan.count; ++i) {
				const auto& beat = playerUpperFace.plan.beats[i];
				Log::Info(Log::Category::kStaging,
					"  Beat {}: {} tone={} strength={} evidence={} span={:.3f}-{:.3f} profile={}"sv,
					i + 1, Acting::Name(beat.action), beat.tone.emotion, beat.tone.percent,
					Acting::Name(beat.evidence), beat.begin, beat.end,
					ExpressionProfiles::kSections[static_cast<std::size_t>(Acting::ProfileFor(beat))]);
			}
		}

		[[nodiscard]] float NextUnit()
		{
			rng = rng * 1664525u + 1013904223u;
			return static_cast<float>((rng >> 16) & 0x7FFF) / 32767.0f;
		}

		[[nodiscard]] RE::HighProcessData* HighOf(RE::Actor* a_actor)
		{
			auto* process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process ? process->high : nullptr;
		}

		// Is a voice line playing on this actor? soundHandles only. voiceState and
		// voiceTimeElapsed belong to the shout system, and voiceTimer holds a constant
		// rather than counting down a line.
		[[nodiscard]] bool VoiceHandlePlaying(RE::Actor* a_actor)
		{
			auto* high = HighOf(a_actor);
			if (!high) {
				return false;
			}

			for (const auto& handle : high->soundHandles) {
				if (handle.soundID != RE::BSSoundHandle::kInvalidID &&
					handle.state.get() == RE::BSSoundHandle::AssumedState::kPlaying) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] RE::BSFaceGenAnimationData* FaceOf(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->GetFaceGenAnimationData() : nullptr;
		}

		void ClearFace(RE::Actor* a_actor)
		{
			if (auto* face = FaceOf(a_actor)) {
				face->ClearExpressionOverride();
			}
		}

		// Raise the draw flags on a node and everything under it. Culling is decided
		// per drawn object, so the parent alone isn't enough.
		void HoldSubtree(RE::NiAVObject* a_object)
		{
			if (!a_object) {
				return;
			}

			auto& flags = a_object->GetFlags();
			heldNodes.push_back({ RE::NiPointer<RE::NiAVObject>{ a_object }, flags.underlying() });
			flags.set(static_cast<RE::NiAVObject::Flag>(kHoldFlags));

			if (auto* node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					HoldSubtree(child.get());
				}
			}
		}

		void ReleaseHeldNodes()
		{
			for (auto& held : heldNodes) {
				if (held.node) {
					held.node->GetFlags() = static_cast<RE::NiAVObject::Flag>(held.flags);
				}
			}
			heldNodes.clear();
			heldRoot.reset();
		}

		// Keep the player's head drawable for the length of the conversation. Not in
		// first person, where forcing the head to draw would put it in front of the
		// camera.
		void ApplyFaceHold(RE::Actor* a_player)
		{
			if (!wantHoldFace || !heldNodes.empty() || !a_player) {
				return;
			}

			auto* camera = RE::PlayerCamera::GetSingleton();
			if (camera && camera->IsInFirstPerson()) {
				return;
			}

			auto* face = a_player->GetFaceNodeSkinned();
			if (!face) {
				return;
			}

			HoldSubtree(face);
			heldRoot.reset(face);

			if (holdReported.Take()) {
				Log::Info(Log::Category::kStaging,
					"Holding the player's head in the drawn set ({} nodes) so it keeps animating off camera."sv,
					heldNodes.size());
			}
		}

		// Move the hold onto a new head when the engine swaps one in. Cheap on the
		// common path (one virtual call and a compare). A null target is a normal
		// state (first person, no 3D); heldRoot stays null so the hold comes back by
		// itself when the head returns.
		void RefreshFaceHold(RE::Actor* a_player)
		{
			if (!wantHoldFace || !a_player) {
				return;
			}

			auto* face = a_player->GetFaceNodeSkinned();
			if (face == heldRoot.get()) {
				return;
			}

			const bool hadHold = !heldNodes.empty();
			ReleaseHeldNodes();
			ApplyFaceHold(a_player);

			// Logged as a warning because the FaceGen probe's call counter goes flat at
			// the same moment, which would otherwise look like the head stopped being
			// morphed.
			if (!heldNodes.empty()) {
				Log::Warn(Log::Category::kStaging,
					"The player's face node was replaced mid-conversation; hold re-applied to the new head ({} nodes)."sv,
					heldNodes.size());
			} else if (hadHold) {
				Log::Warn(Log::Category::kStaging,
					"The player's face node was replaced mid-conversation and the hold could not follow it."sv);
			}
		}

		bool  probeFace{ false };
		float probeCountdown{ 0.0f };

		// Sample every frame for the first seconds of a conversation, since that's
		// when the engine seems to decide whether the player's face animates. Falls
		// back to the idle rate afterwards.
		float probeBurst{ 0.0f };

		constexpr float kProbeBurstSeconds = 2.5f;
		constexpr float kProbeIdleInterval = 0.5f;

		// The player's phoneme channel, tallied over one conversation.
		//
		// phenomeKeyFrame holds values waiting to be applied: when something applies
		// the morphs it drains them, otherwise they pile up. So a high reading means
		// the mouth is not moving. Sample readings:
		//
		//   npc, mouth working ...................... mean 0.000
		//   player, opened facing the camera ........ mean 0.068 (mouth works)
		//   player, opened facing away .............. mean 0.145 (mouth frozen)
		//
		// The threshold sits between the two player cases. It's a rough indicator; the
		// raw numbers are printed with it.
		struct Tally
		{
			std::uint32_t samples{ 0 };
			float         sum{ 0.0f };
			float         peak{ 0.0f };
		};

		Tally playerPhonemes{};

		constexpr float kAppliedBelow = 0.105f;

		// What one keyframe channel currently holds. count comes from the engine's
		// struct and is used as a loop bound, so it's clamped (16 phonemes in Skyrim;
		// 256 is slack).
		//
		// `readable` matters: a peak of 0 can mean a flat channel, a null `values`, a
		// zero count or an absurd count, and only this tells them apart. `argmax`
		// records which slot won, since the slot is what says whether it's a phoneme
		// or a modifier.
		struct Channel
		{
			std::uint32_t count{ 0 };
			float         peak{ 0.0f };
			std::int32_t  argmax{ -1 };
			bool          updated{ false };
			bool          readable{ false };
		};

		[[nodiscard]] Channel Sample(const RE::BSFaceGenKeyframeMultiple& a_keyframe)
		{
			Channel channel{};
			channel.count = a_keyframe.count;
			channel.updated = a_keyframe.isUpdated;

			if (!a_keyframe.values || a_keyframe.count == 0 || a_keyframe.count > 256) {
				return channel;
			}

			channel.readable = true;
			for (std::uint32_t i = 0; i < a_keyframe.count; ++i) {
				if (a_keyframe.values[i] > channel.peak || channel.argmax < 0) {
					channel.peak = a_keyframe.values[i];
					channel.argmax = static_cast<std::int32_t>(i);
				}
			}
			return channel;
		}

		// One channel, formatted for the log. rd=0 means the peak is meaningless; n is
		// the slot count, i the winning slot, up the engine's isUpdated flag (set by
		// SetValue, cleared by whatever consumes the keyframe).
		[[nodiscard]] std::string Describe(const Channel& a_channel)
		{
			return fmt::format("{:.3f}[rd={} n={} i={} up={}]"sv,
				a_channel.peak,
				a_channel.readable ? 1 : 0,
				a_channel.count,
				a_channel.argmax,
				a_channel.updated ? 1 : 0);
		}

		[[nodiscard]] std::string_view PostureOf(RE::Actor* a_actor)
		{
			const auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!state) {
				return "unknown"sv;
			}
			switch (state->GetSitSleepState()) {
			case RE::SIT_SLEEP_STATE::kNormal:            return "standing"sv;
			case RE::SIT_SLEEP_STATE::kIsSitting:         return "seated"sv;
			case RE::SIT_SLEEP_STATE::kIsSleeping:        return "asleep"sv;
			case RE::SIT_SLEEP_STATE::kWantToSit:         return "about-to-sit"sv;
			case RE::SIT_SLEEP_STATE::kWaitingForSitAnim: return "sitting-down"sv;
			case RE::SIT_SLEEP_STATE::kWantToStand:       return "standing-up"sv;
			default:                                      return "other"sv;
			}
		}

		// GetFaceNode() (vfunc 62) isn't overridden by Actor, Character or
		// PlayerCharacter, and TESObjectREFR's version calls GetFaceNodeSkinned()
		// (vfunc 61), so both return the same node. The process-side animation data
		// (vfunc 63) isn't guaranteed to be the same instance as the node's; `same=`
		// in the log compares them.
		void ReportFace(std::string_view a_who, RE::Actor* a_actor)
		{
			const auto posture = PostureOf(a_actor);

			auto* fromProcess = FaceOf(a_actor);
			auto* node = a_actor ? a_actor->GetFaceNode() : nullptr;
			auto* skinned = a_actor ? a_actor->GetFaceNodeSkinned() : nullptr;
			auto* fromNode = node ? node->GetRuntimeData().animationData.get() : nullptr;

			if (!fromProcess && !fromNode) {
				Log::Info(Log::Category::kStaging,
					"Face probe | {} | {} | NO facegen data on either path (node={})."sv,
					a_who, posture, static_cast<const void*>(node));
				return;
			}

			const auto describe = [](RE::BSFaceGenAnimationData* a_data, Channel& a_phoneme,
									  Channel& a_expression, Channel& a_modifier) {
				if (!a_data) {
					return;
				}
				a_phoneme = Sample(a_data->phenomeKeyFrame);
				a_expression = Sample(a_data->expressionKeyFrame);
				a_modifier = Sample(a_data->modifierKeyFrame);
			};

			Channel procPhoneme{}, procExpression{}, procModifier{};
			Channel nodePhoneme{}, nodeExpression{}, nodeModifier{};
			describe(fromProcess, procPhoneme, procExpression, procModifier);
			describe(fromNode, nodePhoneme, nodeExpression, nodeModifier);

			// The node's own state. BSFaceGenNiNode::UpdateDownwardPass (vfunc 0x2C)
			// applies the morphs. lastTime is a global timestamp (identical across
			// actors), so it can't show whether a particular head is being updated.
			std::uint16_t nodeFlags = 0;
			float         nodeLastTime = -1.0f;
			if (node) {
				const auto& runtime = node->GetRuntimeData();
				nodeFlags = runtime.flags;
				nodeLastTime = runtime.lastTime;
			}

			// The face node's full NiAVObject flags. The selective-update bits
			// (0x02/0x04/0x08/0x10) matter for whether a downward pass reaches it. These
			// are different from BSFaceGenNiNode::RUNTIME_DATA::flags (a uint16 at
			// runtime+0x38): `fg=` in the log is that one, `av=` is this one.
			const std::uint32_t nodeAvFlags = node ? node->GetFlags().underlying() : 0u;
			const std::uint32_t nodeCulled = node ? ((nodeAvFlags & 1u) ? 1u : 0u) : 2u;

			// Whether the process-side object and the node-side object are the same.
			const bool sameObject = fromProcess && fromNode && fromProcess == fromNode;

			// Geometry for the log:
			//
			//   body    - the actor's heading against the direction to the camera.
			//             Mostly reflects where the camera sits along the player/NPC
			//             axis, since the body barely turns during a conversation.
			//   inView  - the camera's forward against the direction from the lens to
			//             the head node: +1 dead ahead, 0 at the edge, negative behind.
			//   headFwd - the head node's forward (column 1) against the direction to
			//             the lens, so head tracking is included.
			//   dist    - separates orientation from proximity.
			float body = -2.0f;
			float inView = -2.0f;
			float headFwd = -2.0f;
			float dist = -1.0f;
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot && a_actor) {
				const auto& root = camera->cameraRoot->world;
				const auto  eye = root.translate;
				const auto  here = a_actor->GetPosition();

				const float bx = eye.x - here.x;
				const float by = eye.y - here.y;
				const float flat = std::sqrt(bx * bx + by * by);
				if (flat > 1.0f) {
					const float heading = a_actor->GetAngleZ();
					body = (std::sin(heading) * bx + std::cos(heading) * by) / flat;
				}

				const auto head = node ? node->world.translate : here;
				float      hx = head.x - eye.x;
				float      hy = head.y - eye.y;
				float      hz = head.z - eye.z;
				dist = std::sqrt(hx * hx + hy * hy + hz * hz);
				if (dist > 1.0f) {
					hx /= dist;
					hy /= dist;
					hz /= dist;
					inView = root.rotate.entry[0][1] * hx +
						root.rotate.entry[1][1] * hy +
						root.rotate.entry[2][1] * hz;

					if (node) {
						const auto& hr = node->world.rotate;
						headFwd = -(hr.entry[0][1] * hx + hr.entry[1][1] * hy + hr.entry[2][1] * hz);
					}
				}
			}

			// Does the engine think this actor is voicing a line? DBVO plays lines with
			// `Player.SpeakSound "DBVO/<pack>/<line>.fuz"` rather than the dialogue path,
			// so this checks whether the engine registered a voice line on the player at
			// all. voiceState, soundHandles and voiceTimer are the engine's record of
			// that.
			std::uint32_t voiceState = 0xFFFFFFFFu;
			float         voiceTimer = -1.0f;
			float         voiceElapsed = -1.0f;
			std::uint32_t sound0 = 0;
			std::uint32_t sound0State = 0xFFu;
			std::uint32_t sound1 = 0;
			std::uint32_t sound1State = 0xFFu;
			bool          talkingToPC = false;
			std::string   subtitle{};
			if (auto* high = HighOf(a_actor)) {
				voiceState = high->voiceState.underlying();
				voiceTimer = high->voiceTimer;
				voiceElapsed = high->voiceTimeElapsed;
				sound0 = high->soundHandles[0].soundID;
				sound0State = high->soundHandles[0].state.underlying();
				sound1 = high->soundHandles[1].soundID;
				sound1State = high->soundHandles[1].state.underlying();
				talkingToPC = high->talkingToPC;
				if (high->voiceSubtitle.c_str()) {
					subtitle.assign(high->voiceSubtitle.c_str());
					if (subtitle.size() > 24) {
						subtitle.resize(24);
					}
				}
			}

			// The engine's per-frame morph budget (uiNumActorsAllowedToMorph, default 10,
			// max 64), chosen by distance.
			std::uint32_t morphBudget = 0;
			bool          morphEmotions = false;
			if (auto* faceGen = RE::BSFaceGenManager::GetSingleton()) {
				morphBudget = faceGen->numActorsToMorph;
				morphEmotions = faceGen->emotions;
			}

			if (engaged && a_who == "PLAYER"sv && nodePhoneme.readable) {
				playerPhonemes.samples++;
				playerPhonemes.sum += nodePhoneme.peak;
				playerPhonemes.peak = std::max(playerPhonemes.peak, nodePhoneme.peak);
			}

			Log::Info(Log::Category::kStaging,
				"Face probe | {} | {} | {} | VOICE state={} timer={:.2f} elapsed={:.2f} snd0={}/{} snd1={}/{} toPC={} sub=\"{}\" | ph={} ex={} md={} | same={} skin={} | fg=0x{:04X} t={:.2f} | av=0x{:08X} culled={} | inView={:+.2f} headFwd={:+.2f} dist={:.0f} | morph={}"sv,
				a_who, posture,
				engaged ? "STAGING"sv : "idle   "sv,
				voiceState, voiceTimer, voiceElapsed,
				sound0, sound0State, sound1, sound1State,
				talkingToPC ? 1 : 0, subtitle,
				Describe(nodePhoneme), Describe(nodeExpression), Describe(nodeModifier),
				fromProcess && fromNode ? (sameObject ? "yes"sv : "NO"sv) : "n/a"sv,
				skinned == node ? "same"sv : "DIFFERENT"sv,
				nodeFlags, nodeLastTime,
				nodeAvFlags, nodeCulled,
				inView, headFwd, dist,
				morphBudget);
		}

		// One step of the gaze model. a_holdBias is the fraction of the time this
		// character looks at the other: high while listening, lower while speaking.
		void StepGaze(RE::Actor* a_actor, Gaze& a_gaze, float a_delta, float a_holdBias)
		{
			auto* high = HighOf(a_actor);
			if (!high) {
				return;
			}

			a_gaze.remaining -= a_delta;
			if (a_gaze.remaining <= 0.0f) {
				const bool wantAvert = NextUnit() > a_holdBias;

				if (wantAvert) {
					// A glance, not a head turn: small, and more often sideways or down than up.
					const float lateral = (NextUnit() - 0.5f) * 90.0f;
					const float vertical = (NextUnit() - 0.75f) * 40.0f;
					a_gaze.offset = { lateral, lateral * 0.4f, vertical };
					a_gaze.averted = true;
					a_gaze.remaining = 0.5f + NextUnit() * 1.3f;
				} else {
					a_gaze.offset = {};
					a_gaze.averted = false;
					a_gaze.remaining = 1.4f + NextUnit() * 2.6f;
				}
			}

			// Ease toward the wanted offset so the eyes travel rather than jump.
			auto&       live = high->headTrackTargetOffset;
			const float k = std::clamp(a_delta * 6.0f, 0.0f, 1.0f);
			live.x += (a_gaze.offset.x - live.x) * k;
			live.y += (a_gaze.offset.y - live.y) * k;
			live.z += (a_gaze.offset.z - live.z) * k;
		}
	}

	void Performance::Configure(bool a_expressions, bool a_gaze)
	{
		// Config I/O stays on the game thread, never in the morph hook. Missing keys
		// use built-in defaults, and SD_user.ini can override any profile field.
		playerUpperFace.profiles = ExpressionProfiles::Defaults();
		for (std::size_t i = 0; i < playerUpperFace.profiles.size(); ++i) {
			auto& profile = playerUpperFace.profiles[i];
			const auto section = ExpressionProfiles::kSections[i];
			for (std::size_t key = 0; key < profile.controls.size(); ++key) {
				const int fallback = static_cast<int>(std::lround(profile.controls[key] * 100));
				profile.controls[key] = static_cast<float>(std::clamp(
					Config::Int(section, ExpressionProfiles::kKeys[key], fallback), key < 2 ? -100 : 0, 100)) / 100.0f;
			}
			profile.region = static_cast<std::uint32_t>(std::clamp(
				Config::Int(section, "iRegionEmotion", static_cast<int>(profile.region)), 0, 7));
			profile.regionWeight = static_cast<float>(std::clamp(
				Config::Int(section, "iRegionStrength", static_cast<int>(std::lround(profile.regionWeight * 100))), 0, 100)) / 100.0f;
		}
		cinematicListening.store(Config::Bool("Performance", "bCinematicListening", true), std::memory_order_relaxed);
		wantExpressions.store(a_expressions, std::memory_order_relaxed);
		wantGaze = a_gaze;
		// Expression profiles don't depend on mouth animation being enabled.
		if (a_expressions) FaceGen::Install();
	}

	bool Performance::ExpressionsEnabled() noexcept
	{
		return wantExpressions.load(std::memory_order_relaxed);
	}

	void Performance::UpdateFace(float a_delta)
	{
		if (!std::isfinite(a_delta) || a_delta <= 0.0f) return;
		const float delta = std::min(a_delta, 0.1f);
		auto npc = npcHandle.get();
		const bool driving = engaged && ExpressionsEnabled();
		const bool npcTalking = driving && Dialogue::Session::GetSingleton().Speaking();
		const bool playerTalking = driving && LipSync::PlayerSpeaking();

		if (driving) {
			const auto serial = LipSync::PlayerLineSerial();
			if (playerTalking && (!playerExprWasTalking || serial != playerLineSerial)) {
				const auto topic = LipSync::PlayerLineText();
				// Clause planning interprets the text; no borrowed NPC emotion.
				const Reading reading{ kNeutral, 0 };
				ApplyPlayerReading(reading,
					topic.empty() ? "speaking, no topic text"sv : "speaking, text cues"sv, topic);
				const float duration = LipSync::PlayerLineDuration();
				playerUpperFace.SetDuration(duration);
				Log::Info(Log::Category::kStaging, "Acting clock: {} duration={:.2f}s; clause positions are text-weight estimates."sv,
					playerUpperFace.voiceDuration ? "voice-window (may be fallback)"sv : "text estimate"sv, playerUpperFace.duration);
				if (!npcTalking) SetReading(npcExpression, { kNeutral, 0 }, kReactionScale);
				playerLineSerial = serial;
			}
			// Keep the just-spoken emotion while settling. The next NPC response supplies
			// a new listener reaction.
		}
		playerExprWasTalking = playerTalking;
		if (!driving) playerLineEmotion.store(kNeutral, std::memory_order_relaxed);

		const float scale = intensityScale;
		const auto advance = [&](FaceExpression& face, RE::Actor* actor, bool attentive) {
			face.age += delta;
			face.quietFor = attentive ? 0.0f : face.quietFor + delta;
			Shape target{};
			if (driving) {
				// Brief gaps in speech mustn't pump the expression in and out.
				const float gain = LineGain(face.age, face.quietFor < 0.25f);
				for (std::size_t i = 0; i < kSlots; ++i) target[i] = face.base[i] * gain;
			}
			const bool any = Step(face.current, target, delta) && scale > 0.0f;
			if (any) {
				if (auto* data = FaceOf(actor)) {
					const auto peak = std::max_element(face.current.begin(), face.current.end());
					const auto slot = static_cast<std::uint32_t>(peak - face.current.begin());
					data->SetExpressionOverride(slot, std::clamp(*peak * scale, 0.0f, 1.0f));
					face.ownsOverride = true;
				}
			} else if (face.ownsOverride) {
				ClearFace(actor);
				face.ownsOverride = false;
			}
			return any;
		};
		// Full-face acting only during a silent NPC-listening interval. A voice start
		// clears it immediately instead of fading through lip closures.
		const bool listenerActive = playerListener.Step(delta, playerUpperFace.reading,
			playerUpperFace.age, scale, playerUpperFace.listening && npcTalking,
			playerTalking || !playerUpperFace.listening, driving && cinematicListening.load(std::memory_order_relaxed));
		const bool wasMoving = std::any_of(playerUpperFace.motion.current.begin(),
			playerUpperFace.motion.current.end(), [](float v) { return v > 0.0f; });
		const auto previousBeat = playerUpperFace.beatIndex;
		if (playerTalking) playerUpperFace.SynchronizeSpeech(LipSync::PlayerLineElapsed(), delta);
		const bool moving = playerUpperFace.Step(delta, driving, playerTalking || npcTalking, scale, listenerActive);
		if (driving && playerUpperFace.beatIndex != previousBeat) {
			const auto& beat = playerUpperFace.plan.beats[playerUpperFace.beatIndex];
			Log::Info(Log::Category::kStaging, "Acting beat active: {} {} at {:.2f}/{:.2f}s (text-weight timing)."sv,
				playerUpperFace.beatIndex + 1, Acting::Name(beat.action), playerUpperFace.age, playerUpperFace.duration);
		}
		if (driving) playerLineEmotion.store(playerUpperFace.reading.emotion, std::memory_order_relaxed);
		// Publish the zero endpoint too, so the last residual isn't left on the head.
		const bool playerActive = driving || moving || wasMoving;
		const bool npcActive = advance(npcExpression, npc.get(), npcTalking || playerTalking);
		// No engine calls under this lock; the render hook only copies 17 floats.
		{
			const std::lock_guard lock(expressionMutex);
			publishedUpperFace = playerUpperFace.motion.current;
			publishedRegionalFace = playerUpperFace.regionalMotion.current;
			// Native-listener suppression is already eased above; publishing a hard zero
			// here would skip that smoothing.
			publishedListener = playerListener.current;
			publishedListenerActive = listenerActive;
			publishedNpc = npcExpression.current;
			publishedPlayerActive = playerActive;
			publishedNpcActive = npcActive;
		}
	}

	bool Performance::PlayerExpressionActive() noexcept
	{
		const std::lock_guard lock(expressionMutex);
		return publishedPlayerActive;
	}

	std::uint32_t Performance::PlayerEmotion() noexcept
	{
		return playerLineEmotion.load(std::memory_order_relaxed);
	}

	bool Performance::SampleExpression(float* a_out, std::uint32_t a_count, bool a_player) noexcept
	{
		if (!a_out || a_count == 0) return false;
		if (const int forced = forcedExpression.load(std::memory_order_relaxed); a_player && forced >= 0) {
			std::fill_n(a_out, a_count, 0.0f);
			if (static_cast<std::uint32_t>(forced) < a_count) a_out[forced] = 1.0f;
			return true;
		}
		if (a_player) return false;
		const std::lock_guard lock(expressionMutex);
		if (!publishedNpcActive) return false;
		const auto& shape = publishedNpc;
		const float scale = intensityScale;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			a_out[i] = i < kSlots ? std::clamp(shape[i] * scale, 0.0f, 1.0f) : 0.0f;
		}
		return true;
	}

	bool Performance::SampleListenerExpression(float* a_out, std::uint32_t a_count) noexcept
	{
		if (!a_out || a_count == 0 || !ExpressionsEnabled() || !cinematicListening.load(std::memory_order_relaxed) ||
			forcedExpression.load(std::memory_order_relaxed) >= 0) return false;
		const std::lock_guard lock(expressionMutex);
		if (!publishedListenerActive) return false;
		for (std::uint32_t i = 0; i < a_count; ++i) a_out[i] = i < kSlots ? publishedListener[i] : 0.0f;
		return true;
	}

	bool Performance::SampleUpperFace(float* a_out, std::uint32_t a_count) noexcept
	{
		if (!a_out || a_count == 0) return false;
		const std::lock_guard lock(expressionMutex);
		if (!publishedPlayerActive) return false;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			a_out[i] = i < publishedUpperFace.size() ? publishedUpperFace[i] : 0.0f;
		}
		return true;
	}

	bool Performance::SampleRegionalFace(std::array<float, 8>& a_out) noexcept
	{
		const std::lock_guard lock(expressionMutex);
		a_out = publishedRegionalFace;
		return publishedPlayerActive && forcedExpression.load(std::memory_order_relaxed) < 0;
	}

	void Performance::SetForcedExpression(int a_slot)
	{
		const int slot = (a_slot >= 0 && a_slot <= 16) ? a_slot : -1;
		if (forcedExpression.exchange(slot, std::memory_order_relaxed) == slot) {
			return;
		}

		if (slot < 0) {
			Log::Info(Log::Category::kStaging,
				"Forced expression off; the player's face is the engine's again."sv);
			return;
		}

		// Logged as a warning so it's obvious when it's active.
		Log::Warn(Log::Category::kStaging,
			"FORCED EXPRESSION {}: pinning that slot to 1.0 on the player every frame. "
			"Look at the player's face in third person - it should be visibly stuck in that "
			"expression, with no conversation needed. If it is NOT, the expression channel "
			"does not reach the player's geometry and the fault is the head, not this mod. "
			"Set [Diagnostics] iForceExpression=-1 to stop."sv,
			slot);
	}

	void Performance::SetGaze(float a_listenerHold, float a_speakerHold)
	{
		listenerHold = std::clamp(a_listenerHold, 0.0f, 1.0f);
		speakerHold = std::clamp(a_speakerHold, 0.0f, 1.0f);
	}

	void Performance::Engage(RE::Actor* a_npc)
	{
		if (engaged || !a_npc) {
			return;
		}

		npcHandle = a_npc->GetHandle();
		npcGaze = {};
		playerGaze = {};
		engaged = true;

		// Start each conversation without a reaction carried over from the last one.
		playerReaction = {};
		playerExprWasTalking = false;
		playerUpperFace.Begin({}, true, {});
		playerListener = {};
		Log::Info(Log::Category::kStaging,
			"Expression profiles v4: fixed 1.50 strength; soft ceiling; continuous brow velocity; eased listener handoff; mouth-safe player speech."sv);
		Log::Info(Log::Category::kStaging, "Cinematic listening: {}; strong v2 (scale 0.72, cap 0.85), player speech priority."sv,
			cinematicListening.load(std::memory_order_relaxed) ? "enabled"sv : "disabled"sv);
		npcExpression = {};
		playerLineSerial = 0;
		playerLineEmotion.store(kNeutral, std::memory_order_relaxed);

		gazeReported.Reset();
		holdReported.Reset();
		playerPhonemes = {};
		Log::Info(Log::Category::kStaging,
			"Performance settings for this conversation: gaze={} expressions={} holdFace={} intensity={:.2f} listenerHold={:.2f} speakerHold={:.2f}."sv,
			wantGaze ? "on"sv : "OFF"sv,
			wantExpressions ? "on"sv : "OFF"sv,
			wantHoldFace ? "on"sv : "OFF"sv,
			intensityScale, listenerHold, speakerHold);

		ApplyFaceHold(RE::PlayerCharacter::GetSingleton());

		// Open the dense sampling window.
		probeBurst = kProbeBurstSeconds;
		probeCountdown = 0.0f;
	}

	void Performance::HoldPlayerFace(bool a_hold)
	{
		wantHoldFace = a_hold;

		// Turning it off mid-conversation restores the head immediately.
		if (!wantHoldFace) {
			ReleaseHeldNodes();
		} else if (engaged) {
			ApplyFaceHold(RE::PlayerCharacter::GetSingleton());
		}
	}

	void Performance::Release()
	{
		if (!engaged) {
			return;
		}
		engaged = false;

		// Restore the head first, unconditionally, however the conversation ended
		// (menu close, combat, cell change, the subject dying). Expressions and eyes
		// are handed back as they were found.
		ReleaseHeldNodes();

		// The result for this conversation, in one line (a low number is good; see
		// Tally).
		if (playerPhonemes.samples > 0) {
			const float mean = playerPhonemes.sum / static_cast<float>(playerPhonemes.samples);
			Log::Info(Log::Category::kStaging,
				"VERDICT | player facegen looks {} | phoneme mean={:.4f} peak={:.3f} over {} samples "
				"(low = drained by the morph = mouth moving; calibration npc 0.000 / working 0.068 / frozen 0.145)."sv,
				mean < kAppliedBelow ? "APPLIED — mouth should be moving"sv
									 : "NOT APPLIED — mouth frozen"sv,
				mean, playerPhonemes.peak, playerPhonemes.samples);
		}
		playerPhonemes = {};

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  npc = npcHandle.get();

		// The player's envelope finishes fading through UpdateFace. The NPC leaves the
		// morph hook at End(), so release its override now.
		if (npcExpression.ownsOverride) ClearFace(npc.get());
		npcExpression = {};
		{
			const std::lock_guard lock(expressionMutex);
			publishedNpc = {};
			publishedNpcActive = false;
			publishedListener = {};
			publishedListenerActive = false;
		}
		if (wantGaze) {
			if (auto* high = HighOf(player)) high->headTrackTargetOffset = {};
			if (auto* high = HighOf(npc.get())) high->headTrackTargetOffset = {};
		}

		npcHandle = {};
	}

	void Performance::ResetForLoad()
	{
		Release();
		FaceGen::ReleaseModifiers();
		const auto configuredProfiles = playerUpperFace.profiles;
		playerUpperFace = {};
		playerUpperFace.profiles = configuredProfiles;
		playerListener = {};
		npcExpression = {};
		playerReaction = {};
		playerExprWasTalking = false;
		playerLineSerial = 0;
		playerLineEmotion.store(kNeutral, std::memory_order_relaxed);
		const std::lock_guard lock(expressionMutex);
		publishedUpperFace = {};
		publishedRegionalFace = {};
		publishedListener = {};
		publishedListenerActive = false;
		publishedNpc = {};
		publishedPlayerActive = false;
		publishedNpcActive = false;
	}

	void Performance::OnLine(RE::Actor* a_speaker, std::uint32_t a_emotion,
		std::uint16_t a_percent, std::string_view a_text)
	{
		if (!engaged || !a_speaker || a_speaker != npcHandle.get().get()) return;
		// Keep readings while disabled, so turning expressions on can react to the
		// current line.
		LipSync::OnResponse();
		playerExprWasTalking = false;
		// Keep the authored NPC emotion. Neutral records don't get a full-face
		// override just because the line contains an emotional word.
		const Reading reading = a_emotion > kNeutral && a_emotion <= kPuzzled && a_percent > 0 ?
			Reading{ a_emotion, std::min<std::uint16_t>(a_percent, 100), false } : Reading{ kNeutral, 0 };
		SetReading(npcExpression, reading, 1.0f);
		playerReaction = Listener::ResponseTo(reading.emotion, reading.percent);
		ApplyPlayerReading(playerReaction, "listening"sv, a_text, true);
		Log::Info(Log::Category::kStaging,
			"NPC expression: slot {} ({} emotion {} @ {}%, authored {} @ {}%) <- {}"sv,
			ExpressionFor(reading.emotion), reading.inferred ? "text-inferred"sv : "record"sv,
			reading.emotion, reading.percent, a_emotion, a_percent, a_text);
	}

	void Performance::ConfigureProbe(bool a_probeFace)
	{
		probeFace = a_probeFace;
		probeCountdown = 0.0f;
	}

	// Every change of the player's voice sound handle, at frame rate. The 2 Hz
	// probe can't tell a dropped handle from a short line; logging transitions
	// (sound ID and how long the previous one was held) can.
	void WatchVoice(float a_delta)
	{
		static std::uint32_t lastSound = 0xFFFFFFFFu;
		static float         heldFor = 0.0f;

		auto* high = HighOf(RE::PlayerCharacter::GetSingleton());
		if (!high) {
			return;
		}

		const std::uint32_t now = high->soundHandles[0].soundID;
		heldFor += a_delta;

		if (now == lastSound) {
			return;
		}

		constexpr std::uint32_t kNone = 0xFFFFFFFFu;
		if (lastSound != kNone && now == kNone) {
			Log::Info(Log::Category::kStaging,
				"VOICE player sound {} RELEASED after {:.2f}s."sv, lastSound, heldFor);
		} else if (now != kNone) {
			Log::Info(Log::Category::kStaging,
				"VOICE player sound {} STARTED{}."sv, now,
				lastSound != kNone ? fmt::format(" (replacing {} after {:.2f}s)", lastSound, heldFor) : "");
		}

		lastSound = now;
		heldFor = 0.0f;
	}

	void Performance::Probe(RE::Actor* a_npc, float a_delta)
	{
		if (!probeFace) {
			return;
		}

		// Before the countdown: this one runs every frame.
		WatchVoice(a_delta);

		probeBurst = std::max(0.0f, probeBurst - a_delta);

		probeCountdown -= a_delta;
		if (probeCountdown > 0.0f) {
			return;
		}
		// Zero rather than a small interval, so the burst samples at the game's frame
		// rate.
		probeCountdown = probeBurst > 0.0f ? 0.0f : kProbeIdleInterval;

		// Doesn't require `engaged`: with bEnabled=0 the director never opens, and
		// that's the control case this probe is for.
		ReportFace("PLAYER"sv, RE::PlayerCharacter::GetSingleton());
		if (a_npc) {
			ReportFace("npc"sv, a_npc);
		}
	}

	void Performance::Update(float a_delta, bool a_npcSpeaking)
	{
		if (!engaged) {
			return;
		}

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto  npc = npcHandle.get();
		if (!player || !npc) {
			return;
		}

		// Before the gaze early-return: the head hold isn't part of the gaze model.
		RefreshFaceHold(player);

		if (!wantGaze) {
			return;
		}

		// The player looks at whoever they're talking to while delivering a line. The
		// engine's dialogue state is blank during the player's own voiced line, so the
		// sound handle is checked directly; it works with any voice mod and any other
		// settings.
		const bool playerTalking = VoiceHandlePlaying(player);

		if (playerTalking && !playerWasTalking) {
			// Cancel a glance in progress when the line starts, or the head could stay
			// turned away for most of a short line.
			playerGaze.offset = {};
			playerGaze.averted = false;
			playerGaze.remaining = 0.0f;
		}
		playerWasTalking = playerTalking;

		StepGaze(npc.get(), npcGaze, a_delta, a_npcSpeaking ? speakerHold : listenerHold);
		StepGaze(player, playerGaze, a_delta,
			playerTalking ? 1.0f : (a_npcSpeaking ? listenerHold : speakerHold));

		if (gazeReported.Take()) {
			Log::Info(Log::Category::kStaging, "Gaze model running for both participants."sv);
		}
	}
}

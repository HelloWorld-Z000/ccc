#pragma once

namespace SD::Scene
{
	// Facial performance. Player reactions use coordinated upper-face modifiers;
	// mouth lip sync, blinking and gaze keep their own owners. NPCs use dialogue
	// morphs: NPC records supply authored emotion, and explicit English text cues
	// fill in neutral records and player topics. Listener reactions are softer,
	// separate readings.
	class Performance
	{
	public:
		static void Engage(RE::Actor* a_npc);
		static void Release();
		// Synchronous hand-back before the outgoing world unloads; no fade carries
		// across saves.
		static void ResetForLoad();

		// A new line has begun. Emotion and intensity come from the response the
		// engine is about to speak.
		static void OnLine(RE::Actor* a_speaker, std::uint32_t a_emotion, std::uint16_t a_percent, std::string_view a_text);

		static void Update(float a_delta, bool a_npcSpeaking);

		// The player's face, every frame and ahead of the staging gate. Separate from
		// Update, which returns early when the camera can't resolve its anchors (the
		// face would freeze exactly then), and which stops after Release (the ease-out
		// has to finish and the override flag has to be handed back).
		static void UpdateFace(float a_delta);

		// A synchronized snapshot consumed right before the face morph. a_player=false
		// selects the conversation partner's envelope.
		[[nodiscard]] static bool SampleExpression(float* a_out, std::uint32_t a_count,
			bool a_player = true) noexcept;
		[[nodiscard]] static bool SampleUpperFace(float* a_out, std::uint32_t a_count) noexcept;
		[[nodiscard]] static bool SampleListenerExpression(float* a_out, std::uint32_t a_count) noexcept;
		[[nodiscard]] static bool SampleRegionalFace(std::array<float, 8>& a_out) noexcept;

		// The emotion read from the player's own line, as a
		// RE::DialogueResponse::EmotionType value. Informational; expression profiles
		// own normal brow motion.
		[[nodiscard]] static std::uint32_t PlayerEmotion() noexcept;

		static void Configure(bool a_expressions, bool a_gaze);
		[[nodiscard]] static bool ExpressionsEnabled() noexcept;
		// Includes the coordinated expression's release tail.
		[[nodiscard]] static bool PlayerExpressionActive() noexcept;

		// Diagnostic: pin one expression slot fully on the player (-1 off, 0-16 a
		// slot). The counterpart of FaceGen::SetForcedViseme; needs no conversation or
		// voice mod.
		static void SetForcedExpression(int a_slot);

		// Keep the player's head animating while it's off camera. The engine doesn't
		// morph a face it isn't showing, and this mod is the first thing to cut to the
		// player's face mid-line, so the mouth would otherwise catch up a frame after
		// the cut. Keeps the head in the drawn set for the conversation. Third person
		// only; the flags are restored to their previous values on Release.
		static void HoldPlayerFace(bool a_hold);

		// Fraction of the time each party holds eye contact. The difference between
		// them is the point; equal values make both characters stare. Separate from
		// Configure so the menu can change it live.
		static void SetGaze(float a_listenerHold, float a_speakerHold);

		// Diagnostic: report both participants' facegen state twice a second. The NPC
		// is sampled too as the control: their mouth works, so their numbers show the
		// probe is reading the right fields.
		static void ConfigureProbe(bool a_probeFace);

		// Run the probe against an explicit NPC without Engage. Driven from
		// Director::Tick so it also reports when SD isn't staging anything, which is
		// the control case.
		static void Probe(RE::Actor* a_npc, float a_delta);
	};
}

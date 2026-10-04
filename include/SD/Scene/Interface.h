#pragma once

#include "SD/Scene/DialoguePhase.h"

namespace SD::Scene
{
	// Clears the HUD during a conversation, keeping the parts the game uses to
	// tell the player something.
	//
	// A keep-list rather than a block-list: every child of HUDMovieBaseInstance is
	// hidden except a short list of notification elements (see kReleased in
	// Interface.cpp). The children are found at runtime by walking the movie's
	// depths, so it works with HUD replacers whose element names nobody wrote
	// down. Hiding HUDMenu's root wholesale is only a fallback, since it loses
	// notifications (they're queued by the movie, not deferred).
	//
	// Every child found is named in the log.
	class Interface
	{
	public:
		static void Suppress();
		static void Restore();

		// The two halves of Restore(), separately. The topic list fade and the HUD
		// hide are independent, so the list has to be handed back even when the HUD
		// wasn't touched; otherwise it can be left invisible and still clickable.
		static void RestoreHud();
		// Restore only retained display objects owned by the current movie. Pending
		// restoration is never transferred to a replacement menu.
		static void ReleaseChoices();

		// Re-apply what Suppress() hid, every staged frame. The engine and HUD mods
		// rewrite _visible on their own schedule, and menus that open mid-conversation
		// weren't there for the first sweep. Newcomers are swept on a timer.
		static void Enforce();

		// Fades the topic list out once a choice is made and back in for the next one.
		// Only the topic holder is touched; DialogueMenu's subtitles stay.
		//
		// Returns whether the write reached the list. False means the movie or path
		// couldn't be resolved this frame; the fade just tries again next frame, but
		// ReleaseChoices needs to know. 100 releases our fade rather than forcing an
		// untouched clip to full opacity. Partial fades are capped at the movie's own
		// latest opacity, so its selection and rebuild animations aren't brightened.
		static bool SetChoiceAlpha(float a_alpha);


		// What the dialogue movie says it's doing, from its own state. The engine
		// calls NotifyVoiceReady on the movie, which clears bAllowProgress and arms a
		// timer to set it again, so the movie has an authoritative account of dialogue
		// timing. Read only.
		using MenuPhase = Scene::MenuPhase;
		using DialoguePhase = Scene::DialoguePhase;

		[[nodiscard]] static DialoguePhase ReadDialoguePhase();

		// The topic the player has highlighted, as displayed; empty if unreadable.
		// This is the line the player is about to speak, and the only reliable source
		// for it: under a player-voice mod the engine's dialogue state is blank for
		// the player's whole turn, and lastSelectedDialogue still holds the previous
		// line.
		[[nodiscard]] static std::string ReadSelectedTopic();

		// An identity for the rows the topic list is currently showing (0 = couldn't
		// be read). Lets the fade wait until the engine has actually rebuilt the rows
		// for the new choices instead of guessing from a timer.
		[[nodiscard]] static std::uint64_t ReadTopicListFingerprint();

		[[nodiscard]] static std::string_view Name(MenuPhase a_phase);

		// Hides the vanilla speaker name printed beside the highlighted topic. It's a
		// sibling of the topic list, so the fade doesn't reach it. False lets it fade
		// with the list instead. Applies live.
		static void SetHideSpeakerName(bool a_hide);

		// Leave HUDMenu's subtitle visible while the HUD is hidden. Set before
		// Suppress() for one staging (filming two NPCs, whose lines only appear there)
		// and cleared when it closes.
		static void SetKeepSubtitles(bool a_keep);
	};
}

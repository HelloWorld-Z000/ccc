#pragma once

namespace SD::Core
{
	// Keys that do something while a conversation is on screen.
	//
	// This isn't a dialogue input handler and must not become one: ProcessEvent
	// always returns kContinue and passes events on untouched, so nothing here can
	// affect the topic list. A left click committing the highlighted topic is the
	// game's business.
	//
	// Keyboard only. The mouse buttons are tied up with the topic list, and a
	// gamepad has no free buttons during dialogue.
	class Hotkeys
	{
	public:
		// The bindable actions. All ship unassigned.
		enum class Action : std::uint8_t
		{
			kNextAngle,  // cut now, ignoring the hold floor
			kFraming,    // cycle automatic / them / you / the room
			kFilmScene,  // film the conversation in front of you, or stop; works outside one
			kCount
		};

		static void Install();

		// Re-read the bindings from the ini after something else wrote one.
		static void Refresh();

		// The scan code bound to this action, or 0 for unassigned.
		[[nodiscard]] static std::uint32_t Binding(Action a_action) noexcept;

		// Bind, or pass 0 to clear. Writes the ini and applies from the next press.
		static void SetBinding(Action a_action, std::uint32_t a_code);

		// Capture the next key from the game's own input. ImGui's key enum doesn't map
		// cleanly to the DirectX scan codes the game reports (dead keys, international
		// layouts, the numpad), so capturing from the same event stream that fires the
		// hotkey records exactly the code that will match. The sink is always
		// registered so capture works before anything is bound.
		static void Arm(Action a_action);
		static void Cancel();

		// Offer a scan code to whatever is waiting for one; true if it was taken.
		// Called from the game's input sink and from the settings framework's
		// callback, because the framework intercepts input while its menu (where the
		// capture widget lives) is open. The first caller clears the armed flag, so a
		// key arriving by both routes is taken once.
		static bool OfferKey(std::uint32_t a_code);

		// Which action is waiting for a key, if any.
		[[nodiscard]] static bool Capturing() noexcept;
		[[nodiscard]] static bool CapturingFor(Action a_action) noexcept;

		// Called from the panel each frame while armed; applies a captured key and
		// returns true on the frame it lands. Polled rather than applied in the sink
		// because SetBinding writes the ini.
		static bool PollCapture();

		// A readable name for a scan code ("F7", "Numpad 3"). "Not assigned" for 0 and
		// "Key 137" for unknown codes; never empty.
		[[nodiscard]] static std::string_view KeyName(std::uint32_t a_code) noexcept;

		[[nodiscard]] static bool AnyBound() noexcept;
	};
}

#pragma once

namespace SD::Render
{
	// Black bars across the top and bottom of the frame while a conversation is
	// staged. The only part of the mod that draws anything, so it's kept small,
	// backs up every bit of device state it touches, and disables itself for good
	// on the first failure.
	class Letterbox
	{
	public:
		static void Install();
		static void Shutdown();

		// Bars ease toward this over about a third of a second.
		static void SetVisible(bool a_visible);

		// Take the bars off this frame, without the ease, for when a menu is about to
		// draw over the frame (otherwise the menu's header sits under a bar while they
		// slide away). Safe from the game thread; the next SetVisible(true) clears it.
		static void Retract();

		// Whether a menu owns the screen, as MenuWatch sees it, pushed to the render
		// thread. RE::UI::GameIsPaused() is too broad: it also counts the console and
		// SKSE Menu Framework's own panel (when FreezeTimeOnMenu is on), which hid the
		// bars while they were being configured.
		static void SetScreenTaken(bool a_taken);

		// Height of each bar as a fraction of screen height. Safe to call from the
		// game thread.
		static void SetBarFraction(float a_fraction);

		// Draw the bars under one menu so its subtitle can sit inside them. kOff draws
		// them at Present, over every menu. Otherwise they're drawn just before the
		// named menu's movie: that menu draws on top and everything rendered before it
		// stays covered. One specific menu, not whichever draws first (drawing under
		// the HUD would uncover every mod widget): the dialogue menu in a
		// conversation, the HUD while filming NPCs. Present still draws on any frame
		// the menu didn't. Only used for Subtitles In The Black Bar; see
		// Scene::Subtitles.
		enum class Beneath : std::uint8_t
		{
			kOff,
			kDialogue,
			kHud
		};
		static void SetBeneath(Beneath a_menu);

		// The bottom bar as drawn last frame, as a fraction of the frame height,
		// easing included. 0 when there are no bars.
		[[nodiscard]] static float DrawnFraction() noexcept;

		// The bottom bar the ease is heading for: full height while wanted, 0
		// otherwise. Subtitles are placed against this so they land where the bar will
		// be.
		[[nodiscard]] static float TargetFraction() noexcept;

		[[nodiscard]] static bool Installed() noexcept;
	};
}

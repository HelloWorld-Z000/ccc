#pragma once

namespace SD::Dialogue
{
	// Watches the menu stack, driven by the engine's menu events. Independent of
	// Session (which polls MenuTopicManager from the tick), and the one part of
	// the mod that still runs while a pausing menu is open. The two can
	// legitimately disagree: the menu closes while an NPC is still saying goodbye,
	// and a forcegreet starts a conversation with no menu.
	class MenuWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static void Register();

		// A screen-owning menu is open. The only copy of this answer, kept here
		// because the Director's tick doesn't run while the game is paused. Runtime
		// checks it before opening a conversation.
		[[nodiscard]] static bool ScreenTaken() noexcept;

		// Drop any recorded menu the UI says is no longer open. A record built from
		// paired events can be left holding a menu whose close never arrived (a load,
		// a mod force-closing a menu), which would block staging for good. Called from
		// the Director's tick.
		static void Reconcile();

		MenuWatch(const MenuWatch&) = delete;
		MenuWatch(MenuWatch&&) = delete;
		MenuWatch& operator=(const MenuWatch&) = delete;
		MenuWatch& operator=(MenuWatch&&) = delete;

	protected:
		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent*               a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_source) override;

	private:
		MenuWatch() = default;
		~MenuWatch() override = default;

		[[nodiscard]] static MenuWatch* GetSingleton();
	};
}

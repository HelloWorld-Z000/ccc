#pragma once

namespace SD::Menu
{
	// The in-game settings panel, drawn through SKSE Menu Framework.
	//
	// A soft dependency: the framework header resolves every entry point through
	// GetProcAddress, and IsInstalled() checks for the DLL before anything is
	// registered, so without the framework the mod works as before with SD.ini as
	// its only interface. Nothing is linked, so there's no import to fail at load.
	//
	// The ini files are the source of truth; the menu writes to them and applies
	// changes live, with no state of its own.
	class Settings
	{
	public:
		// Safe to call unconditionally; does nothing when the framework is absent.
		static void Register();
	};
}

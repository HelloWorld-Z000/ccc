#pragma once

namespace SD::Scene
{
	// Depth of field without shipping a plugin file. Engine DoF needs a
	// TESImageSpaceModifier form, which would normally mean bundling an ESL, but
	// Skyrim.esm already has one built for it: VATSImodDOF (0x00035301), left over
	// from an unused feature and present in every install.
	class Focus
	{
	public:
		static void Engage();
		static void Release();
		static void Configure(bool a_enabled, float a_strength);

		[[nodiscard]] static bool Available();
	};
}

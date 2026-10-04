#pragma once

namespace SD::Scene
{
	// Lighting: four looks, plus on/off, brightness and color. Each look is a
	// fixed arrangement of up to three lamps, positioned relative to the camera
	// and subject; the geometry is authored here rather than exposed as settings.
	enum class Lamp : std::uint8_t
	{
		// The light that models the face.
		kKey = 0,

		// Lifts the shadow the key leaves.
		kFill,

		// Behind the subject, edging the hair and shoulder. Separates the subject from
		// the background, which a key alone doesn't do in an already lit room.
		kRim,

		kCount
	};

	inline constexpr std::size_t kLampCount = static_cast<std::size_t>(Lamp::kCount);

	// One lamp's place in a look: geometry and relative strength. Color is a
	// single global setting (see KeyLight::Configure).
	struct LampSpec
	{
		// 0-300 percent, relative to the brightness setting. Zero means the lamp isn't
		// created at all, so a look can be defined by what it leaves out (Hard has no
		// fill).
		int intensity;

		// World units the light reaches, 60-1200. A small radius falls off fast and
		// gives a harder shadow edge.
		int radius;

		// Degrees around the subject from the camera. 0 is at the lens (flat), 180 is
		// directly behind (rim). The sign picks a side; KeyLight flips it with the
		// camera so the key stays on the same side of the frame through a cut.
		int azimuth;

		// Degrees above the subject's eye line, -60 to 80.
		int elevation;

		// Percent of the camera-to-subject distance, 10-200. Relative, so a look
		// lights a close-up and a master the same way.
		int distance;
	};

	// A named look. Four of them plus Off. They differ only in shape (how hard the
	// key is, whether there's a fill, how much rim), not color.
	struct LookSpec
	{
		// The ini token. Shots store this string rather than an index so the table can
		// be reordered or extended without relighting everything.
		const char* key;

		const char* name;     // menu label
		const char* summary;  // one line, under the label

		LampSpec lamps[kLampCount];
	};

	[[nodiscard]] std::span<const LookSpec> AllLooks();

	// Index into AllLooks, or -1. Case-insensitive, since the file is hand-edited.
	[[nodiscard]] int FindLook(std::string_view a_key);

	// What an angle falls back to when its key is missing or unknown. Never -1.
	[[nodiscard]] int DefaultLook();

	// Three 0-255 channels to the engine's color. This is the light's diffuse
	// color, not its amount (brightness drives fade separately), so 255 on every
	// channel is white, not brighter.
	[[nodiscard]] RE::NiColor ColourFrom(int a_red, int a_green, int a_blue);
}

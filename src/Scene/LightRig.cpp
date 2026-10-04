#include "SD/Scene/LightRig.h"

#include <cctype>

namespace SD::Scene
{
	namespace
	{
		// The four looks, plus Off. They differ only in shape; color is a separate
		// setting. A look is defined as much by the lamp it leaves out as by the ones
		// it uses (Hard has no fill, Edge has no key).
		//
		// Order isn't storage: shots store the `key` string, so the table can be
		// reordered or extended; only a rename breaks a config.
		constexpr LookSpec kLooks[] = {
			//                    intensity, radius, azimuth, elevation, distance
			{ "off", "Off", "No added light. The room lights the scene, as the game always did.",
				{ { 0, 400, 34, 20, 68 },
					{ 0, 560, -55, 4, 92 },
					{ 0, 300, 155, 34, 60 } } },

			{ "natural", "Natural", "A gentle lift on the face. Safe anywhere, and hard to notice.",
				{ { 85, 440, 34, 20, 68 },
					{ 34, 560, -55, 4, 92 },
					{ 18, 300, 155, 34, 60 } } },

			{ "soft", "Soft", "Broad and forgiving. Low contrast, flattering.",
				{ { 110, 490, 32, 18, 65 },
					{ 62, 600, -58, 0, 95 },
					{ 34, 310, 155, 34, 60 } } },

			{ "hard", "Hard", "One strong side light and nothing filling the shadow.",
				{ { 155, 300, 48, 28, 55 },
					{ 0, 520, -55, 0, 95 },
					{ 32, 260, 160, 40, 55 } } },

			{ "edge", "Edge only", "No key at all. Shape from behind, face left in shadow.",
				{ { 0, 300, 45, 25, 60 },
					{ 14, 620, -60, 0, 100 },
					{ 130, 320, 172, 34, 55 } } },
		};

		constexpr std::size_t kLookCount = std::size(kLooks);
	}

	std::span<const LookSpec> AllLooks()
	{
		return std::span<const LookSpec>{ kLooks, kLookCount };
	}

	int FindLook(std::string_view a_key)
	{
		if (a_key.empty()) {
			return -1;
		}

		for (std::size_t i = 0; i < kLookCount; ++i) {
			const std::string_view candidate{ kLooks[i].key };
			if (candidate.size() != a_key.size()) {
				continue;
			}

			// ASCII-only on purpose: look keys are ASCII, and std::tolower on a signed
			// char above 0x7F is undefined.
			bool match = true;
			for (std::size_t c = 0; c < candidate.size(); ++c) {
				const auto lhs = static_cast<unsigned char>(candidate[c]);
				const auto rhs = static_cast<unsigned char>(a_key[c]);
				if (std::tolower(lhs) != std::tolower(rhs)) {
					match = false;
					break;
				}
			}
			if (match) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	int DefaultLook()
	{
		// Natural rather than Off for an untouched angle: it's the look that's safe
		// everywhere.
		const int natural = FindLook("natural");
		return natural >= 0 ? natural : 0;
	}

	RE::NiColor ColourFrom(int a_red, int a_green, int a_blue)
	{
		// Stored 0-255 (what a color picker returns and what the ini shows), used 0-1.
		return RE::NiColor{
			std::clamp(a_red, 0, 255) / 255.0f,
			std::clamp(a_green, 0, 255) / 255.0f,
			std::clamp(a_blue, 0, 255) / 255.0f
		};
	}
}

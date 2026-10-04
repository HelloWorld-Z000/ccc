#pragma once

// UTF-8 text helpers. Topic rows from Scaleform and subtitles from response
// records are UTF-8, and anything that needs characters rather than bytes goes
// through here: counting words in scripts without spaces, or finding
// full-width ？ and ！ in Japanese and Chinese text.

namespace SD::Text
{
	// U+FFFD, returned for bytes that aren't valid UTF-8. Every caller is
	// classifying, so a corrupt byte just becomes a character that matches
	// nothing.
	inline constexpr char32_t kReplacement = 0xFFFD;

	// Decode one character and advance a_pos past it. Always advances (callers
	// loop over the whole string); a bad lead byte consumes itself and returns
	// kReplacement.
	[[nodiscard]] inline char32_t NextCodepoint(std::string_view a_text, std::size_t& a_pos)
	{
		if (a_pos >= a_text.size()) {
			return 0;
		}

		const auto byte = [&](std::size_t a_index) {
			return static_cast<std::uint8_t>(a_text[a_index]);
		};

		const std::uint8_t lead = byte(a_pos);

		if (lead < 0x80) {
			++a_pos;
			return lead;
		}

		// How many bytes the lead byte claims and the bits it contributes. 0 marks a
		// continuation byte or a retired length, which is a stray, not a start.
		std::size_t  length = 0;
		std::uint32_t value = 0;
		if ((lead & 0xE0) == 0xC0) {
			length = 2;
			value = lead & 0x1Fu;
		} else if ((lead & 0xF0) == 0xE0) {
			length = 3;
			value = lead & 0x0Fu;
		} else if ((lead & 0xF8) == 0xF0) {
			length = 4;
			value = lead & 0x07u;
		} else {
			++a_pos;
			return kReplacement;
		}

		if (a_pos + length > a_text.size()) {
			++a_pos;
			return kReplacement;
		}

		for (std::size_t i = 1; i < length; ++i) {
			const std::uint8_t continuation = byte(a_pos + i);
			if ((continuation & 0xC0) != 0x80) {
				++a_pos;  // truncated sequence: resync from the next byte, not past it
				return kReplacement;
			}
			value = (value << 6) | (continuation & 0x3Fu);
		}

		a_pos += length;

		// Overlong forms and surrogates are rejected like a bad lead byte.
		const bool overlong = (length == 2 && value < 0x80) ||
			(length == 3 && value < 0x800) ||
			(length == 4 && value < 0x10000);
		const bool surrogate = value >= 0xD800 && value <= 0xDFFF;
		if (overlong || surrogate || value > 0x10FFFF) {
			return kReplacement;
		}

		return static_cast<char32_t>(value);
	}

	// ASCII-only lowercasing. std::tolower depends on the locale above 0x7F, and
	// every keyword this is used with is ASCII.
	[[nodiscard]] inline constexpr char AsciiLower(char a_char) noexcept
	{
		return (a_char >= 'A' && a_char <= 'Z') ?
			static_cast<char>(a_char - 'A' + 'a') :
			a_char;
	}
}

#pragma once

#include <algorithm>

namespace SD::Render
{
	// Where a subtitle goes when it's placed in the bottom bar. Pure, so it can be
	// tested without a movie; Scene::Subtitles reads the numbers from Scaleform
	// and writes the result back.
	//
	// One vertical axis, top-down, in the movie's stage units: the visible frame
	// runs from screenTop to screenBottom, and the bar is the bottom barFraction
	// of that height.
	struct SubtitleFrame
	{
		float screenTop{ 0.0f };
		float screenBottom{ 0.0f };
		float barFraction{ 0.0f };

		// Where the movie itself put the first line, and how tall the lines are.
		float textTop{ 0.0f };
		float textHeight{ 0.0f };
	};

	struct SubtitleSpots
	{
		// The text centred between the top of the bar and the bottom of the frame.
		float centred{ 0.0f };

		// The text over the picture: the movie's own position, raised only as much as
		// needed to clear the bar.
		float clear{ 0.0f };

		// The text fits inside the bar with its padding. False with no bar.
		bool fits{ false };
	};

	// Padding between the text and each edge of the bar, as a fraction of the bar.
	constexpr float kBarPadding = 0.08f;

	// Gap between the text and the top of the bar when it sits over the picture,
	// as a fraction of the frame.
	constexpr float kBarGap = 0.01f;

	[[nodiscard]] inline SubtitleSpots SpotsFor(const SubtitleFrame& a_frame)
	{
		SubtitleSpots spots{ a_frame.textTop, a_frame.textTop, false };

		const float height = a_frame.screenBottom - a_frame.screenTop;
		if (!(height > 0.0f) || !(a_frame.textHeight > 0.0f)) {
			return spots;
		}

		const float bar = std::clamp(a_frame.barFraction, 0.0f, 0.5f) * height;
		const float barTop = a_frame.screenBottom - bar;

		spots.centred = barTop + (bar - a_frame.textHeight) * 0.5f;
		spots.clear = std::min(a_frame.textTop, barTop - kBarGap * height - a_frame.textHeight);
		spots.fits = bar > 0.0f && a_frame.textHeight <= bar * (1.0f - 2.0f * kBarPadding);
		return spots;
	}

	// Eases between the two positions. Both positions already follow the bar, so
	// this only handles switching between them (a three-row line that doesn't fit
	// followed by a one-row line that does).
	//
	// Held while there's no text: a blank field would always fit, so the subtitle
	// would bounce in and out of the bar between lines. The first measurement
	// lands immediately, text or not, so the first line appears in the right place
	// instead of sliding in.
	class SubtitleSlide
	{
	public:
		static constexpr float kSlideSeconds = 0.25f;

		// Returns 0 for over the picture, 1 for in the bar.
		float Advance(bool a_fits, bool a_haveText, float a_delta)
		{
			if (!primed) {
				weight = a_fits ? 1.0f : 0.0f;
				primed = true;
				return weight;
			}

			if (a_haveText) {
				const float target = a_fits ? 1.0f : 0.0f;
				const float step = std::max(a_delta, 0.0f) / kSlideSeconds;
				weight += std::clamp(target - weight, -step, step);
			}
			return weight;
		}

		void Reset()
		{
			weight = 0.0f;
			primed = false;
		}

		[[nodiscard]] float Weight() const { return weight; }

	private:
		float weight{ 0.0f };
		bool  primed{ false };
	};

	[[nodiscard]] inline float Blend(const SubtitleSpots& a_spots, float a_weight)
	{
		const float w = std::clamp(a_weight, 0.0f, 1.0f);
		return a_spots.clear + (a_spots.centred - a_spots.clear) * w;
	}
}

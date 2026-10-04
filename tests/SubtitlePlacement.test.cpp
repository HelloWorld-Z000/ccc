#include "SD/Render/SubtitlePlacement.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
	void Expect(bool condition, const char* message)
	{
		if (!condition) {
			std::cerr << "SubtitlePlacement: " << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	bool Near(float a, float b)
	{
		return std::fabs(a - b) < 0.01f;
	}

	using SD::Render::SubtitleFrame;
	using SD::Render::SubtitleSlide;
	using SD::Render::SpotsFor;
}

int main()
{
	// A 720-unit stage, the subtitle placed by its movie at 560, one row of 30.
	SubtitleFrame frame{ 0.0f, 720.0f, 0.12f, 560.0f, 30.0f };

	// 12% of 720 is 86.4, so the bar starts at 633.6 and the row is centred in it.
	auto spots = SpotsFor(frame);
	Expect(spots.fits, "one row fits a 12% bar");
	Expect(Near(spots.centred, 633.6f + (86.4f - 30.0f) * 0.5f), "centred between the bar's top and the frame's bottom");
	Expect(Near(spots.clear, 560.0f), "over the picture is the movie's own place when that clears the bar");

	// A row that ends just short of the bar still keeps the gap above it.
	frame.textTop = 600.0f;
	spots = SpotsFor(frame);
	Expect(Near(spots.clear, 633.6f - 7.2f - 30.0f), "a row ending inside the gap is lifted to keep it");

	// Three rows do not fit the same bar.
	frame.textHeight = 90.0f;
	spots = SpotsFor(frame);
	Expect(!spots.fits, "three rows overflow a 12% bar");
	Expect(Near(spots.clear, 633.6f - 7.2f - 90.0f), "a block that would run into the bar is lifted clear of it");

	// No bar: nothing fits, and the text stays where its movie put it.
	frame = { 0.0f, 720.0f, 0.0f, 600.0f, 30.0f };
	spots = SpotsFor(frame);
	Expect(!spots.fits, "no bar means nothing fits");
	Expect(Near(spots.clear, 600.0f), "no bar leaves the movie's own position alone");

	// A thin bar with the movie's own position already inside it.
	frame = { 0.0f, 720.0f, 0.04f, 700.0f, 30.0f };
	spots = SpotsFor(frame);
	Expect(!spots.fits, "a 4% bar is too small for a row of 30");
	Expect(spots.clear + 30.0f <= 720.0f - 0.04f * 720.0f, "a too-small bar puts the text over it, not across it");

	// The visible frame need not start at zero: a stage letterboxed by its own
	// scale mode has a top and bottom of its own.
	frame = { -60.0f, 780.0f, 0.10f, 650.0f, 30.0f };
	spots = SpotsFor(frame);
	Expect(spots.fits, "a row fits a 10% bar of an 840-unit frame");
	Expect(Near(spots.centred, 780.0f - 84.0f + (84.0f - 30.0f) * 0.5f), "measured from the visible frame, not the stage");

	// The slide: the first measurement lands in place, even on a blank field, so
	// the first line is written into a field already in the bar.
	SubtitleSlide slide;
	Expect(slide.Advance(true, false, 0.016f) == 1.0f, "a blank field lands in the bar before the first line");
	Expect(slide.Advance(true, true, 0.016f) == 1.0f, "and the first line appears there without sliding");
	const float half = slide.Advance(false, true, SubtitleSlide::kSlideSeconds * 0.5f);
	Expect(Near(half, 0.5f), "a line that stops fitting eases out over the slide time");
	Expect(Near(slide.Advance(true, false, 1.0f), 0.5f), "a blank field holds where the text was");
	Expect(Near(slide.Advance(false, true, 1.0f), 0.0f), "and the ease finishes once there is text again");

	slide.Reset();
	Expect(slide.Advance(false, true, 0.016f) == 0.0f, "after a reset the first line lands over the picture if it does not fit");

	// Blend between the two.
	frame = { 0.0f, 720.0f, 0.12f, 600.0f, 30.0f };
	spots = SpotsFor(frame);
	Expect(Near(SD::Render::Blend(spots, 0.0f), spots.clear), "weight 0 is over the picture");
	Expect(Near(SD::Render::Blend(spots, 1.0f), spots.centred), "weight 1 is in the bar");

	std::cout << "SubtitlePlacement: all tests passed\n";
	return EXIT_SUCCESS;
}

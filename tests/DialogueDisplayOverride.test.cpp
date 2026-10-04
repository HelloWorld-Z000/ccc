#include "SD/Scene/DialogueDisplayOverride.h"

#include <cstdlib>
#include <iostream>

namespace
{
	struct Clip
	{
		double alpha{ 100.0 };
		bool visible{ true };
		bool readable{ true };
		bool writable{ true };
		int writes{ 0 };
		bool roundAlpha{ false };
	};
	struct Node
	{
		Clip* clip{};
		bool operator==(const Node&) const = default;
		bool ReadAlpha(double& a_value) const
		{
			if (!clip || !clip->readable) { return false; }
			a_value = clip->alpha;
			return true;
		}
		bool ReadVisible(bool& a_value) const
		{
			if (!clip || !clip->readable) { return false; }
			a_value = clip->visible;
			return true;
		}
		bool WriteAlpha(double a_value)
		{
			if (!clip || !clip->writable) { return false; }
			clip->alpha = clip->roundAlpha ? static_cast<double>(static_cast<float>(a_value)) : a_value;
			++clip->writes;
			return true;
		}
		bool WriteVisible(bool a_value)
		{
			if (!clip || !clip->writable) { return false; }
			clip->visible = a_value;
			++clip->writes;
			return true;
		}
	};
	void Expect(bool a_ok, const char* a_message)
	{
		if (!a_ok) { std::cerr << a_message << '\n'; std::exit(EXIT_FAILURE); }
	}
}

int main()
{
	SD::Scene::DialogueDisplayOverride<Node> fade;
	Clip oldMenu;
	fade.Bind({ &oldMenu });
	Expect(fade.SetAlpha(50) && fade.SetAlpha(0) && fade.SetHidden(true), "fade old menu");
	oldMenu.readable = false;  // close while a restore cannot reach the old clip
	Expect(!fade.Release(), "failed restoration stays pending for the bound clip");
	Clip newMenu{ 0.0, false };
	fade.Bind({ &newMenu });  // same path, different movie / different display object
	Expect(fade.SetAlpha(100) && fade.SetHidden(false) && fade.Release(), "new menu is untouched");
	Expect(newMenu.alpha == 0 && !newMenu.visible && newMenu.writes == 0,
		"old debt cannot reveal replacement placeholder rows");
	newMenu.alpha = 100;
	newMenu.visible = true;
	Expect(fade.SetAlpha(40) && fade.SetHidden(true), "new menu may fade once populated");
	fade.Bind({ &newMenu });  // unchanged movie across barter suspension
	Expect(fade.Release(), "same-object restoration succeeds");
	Expect(newMenu.alpha == 100 && newMenu.visible, "same movie regains its original values");
	const int restoredWrites = newMenu.writes;
	Expect(fade.Release() && newMenu.writes == restoredWrites, "restoration occurs once");

	Clip hiddenByGame{ 0.0, false };
	fade.Bind({ &hiddenByGame });
	Expect(fade.SetAlpha(0) && fade.SetHidden(true) && fade.Release(), "already hidden clip is untouched");
	Expect(hiddenByGame.writes == 0 && !hiddenByGame.visible, "do not claim another writer's hide");

	Clip altered{ 75.0, true };
	fade.Bind({ &altered });
	Expect(fade.SetAlpha(20), "capture non-default original alpha");
	altered.alpha = 33.0;  // engine or another mod changes it while SD is releasing
	Expect(fade.Release() && altered.alpha == 33.0, "release preserves another writer's new value");
	Expect(fade.SetAlpha(0) && fade.SetAlpha(100) && altered.alpha == 33.0,
		"ending a fade restores the actual original rather than forcing 100");

	Clip rebuilt{ 0.0, true };
	fade.Bind({ &rebuilt });
	Expect(fade.SetAlpha(50), "fade starts while the movie still has a zero-alpha holder");
	Expect(rebuilt.alpha == 0.0 && rebuilt.writes == 0,
		"an in-progress fade cannot reveal the movie's hidden placeholder rows");
	rebuilt.alpha = 100.0;  // the movie finishes rebuilding the available topics
	Expect(fade.SetAlpha(0) && fade.SetHidden(true), "reassert the outgoing fade after rebuild");
	Expect(fade.SetAlpha(50) && fade.SetHidden(false) && rebuilt.alpha == 50.0 && rebuilt.visible,
		"returning options become visible during the fade in");
	Expect(fade.SetAlpha(99) && fade.SetAlpha(100) && rebuilt.alpha == 100.0 && rebuilt.visible,
		"returning options stay visible when the fade finishes");

	Clip changedDuringFade{ 75.0, true };
	fade.Bind({ &changedDuringFade });
	Expect(fade.SetAlpha(20), "start fading a translucent holder");
	changedDuringFade.alpha = 33.0;
	Expect(fade.SetAlpha(10) && fade.SetAlpha(90) && fade.SetAlpha(100) && changedDuringFade.alpha == 33.0,
		"fade completion preserves the movie's latest opacity, including non-default values");
	Expect(fade.SetAlpha(90) && changedDuringFade.alpha == 33.0,
		"a partial fade cannot brighten a translucent movie above its own opacity");
	Expect(fade.SetAlpha(10) && changedDuringFade.alpha == 10.0 &&
		fade.SetAlpha(20) && changedDuringFade.alpha == 20.0 &&
		fade.SetAlpha(90) && changedDuringFade.alpha == 33.0,
		"fade-in advances against the movie opacity without compounding our previous fade");

	Clip selectionTransition;
	fade.Bind({ &selectionTransition });
	Expect(fade.SetAlpha(80), "start fading the selected topic");
	for (const double alpha : { 70.0, 40.0 }) {
		selectionTransition.alpha = 0.0;  // the movie hides/rebuilds after selection
		const int beforeHide = selectionTransition.writes;
		Expect(fade.SetAlpha(alpha) && fade.SetAlpha(alpha - 10.0) &&
			selectionTransition.alpha == 0.0 && selectionTransition.writes == beforeHide,
			"repeated movie hides stay hidden between fade ticks instead of flashing");
		selectionTransition.alpha = 100.0;
		Expect(fade.SetAlpha(alpha) && selectionTransition.alpha == alpha,
			"fading resumes when the movie itself reveals the list");
	}
	Expect(fade.SetAlpha(75) && selectionTransition.alpha == 75.0 &&
		fade.SetAlpha(100) && selectionTransition.alpha == 100.0,
		"the next topic list returns fully after repeated selection transitions");

	Clip matchesTarget;
	fade.Bind({ &matchesTarget });
	Expect(fade.SetAlpha(0), "start fading before an external write matches the next target");
	matchesTarget.alpha = 50.0;
	const int beforeMatchingWrite = matchesTarget.writes;
	Expect(fade.SetAlpha(50) && matchesTarget.writes == beforeMatchingWrite,
		"matching an external value needs no write");
	Expect(fade.SetAlpha(75) && fade.SetAlpha(100) && matchesTarget.alpha == 50.0,
		"matching the fade target still invalidates the older restore value");

	Clip stillHidden{ 0.0, false };
	fade.Bind({ &stillHidden });
	Expect(fade.SetAlpha(50) && fade.SetAlpha(100) && fade.SetHidden(false) && stillHidden.alpha == 0.0 && !stillHidden.visible,
		"a movie that never releases its own hide is not forced to full opacity");

	Clip rounded;
	rounded.roundAlpha = true;
	fade.Bind({ &rounded });
	Expect(fade.SetAlpha(24.123456789) && fade.SetAlpha(76.987654321) && fade.SetAlpha(100) && rounded.alpha == 100.0,
		"rounded display writes do not replace the movie's restore value with our fade");

	Clip retry;
	fade.Bind({ &retry });
	Expect(fade.SetAlpha(0) && fade.SetHidden(true), "prepare failed write recovery");
	retry.writable = false;
	Expect(!fade.Release(), "failed writes retain restoration debt");
	retry.writable = true;
	Expect(fade.Release() && retry.alpha == 100 && retry.visible, "retry restores the same clip");

	Expect(fade.SetAlpha(0), "prepare a menu destroyed without observing an empty frame");
	Clip replacement;
	fade.Bind({ &replacement });
	Expect(retry.alpha == 100 && replacement.writes == 0,
		"direct replacement restores only the old clip");
	fade.Reset();
	Expect(fade.Release() && replacement.writes == 0, "load reset cannot resurrect old debt");
	std::cout << "Dialogue display ownership tests passed\n";
}

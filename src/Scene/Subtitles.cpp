#include "SD/Scene/Subtitles.h"

#include "SD/Core/Logging.h"
#include "SD/Render/SubtitlePlacement.h"

namespace SD::Scene
{
	namespace
	{
		// One subtitle this can move. Edge UI's dialoguemenu.swf places a text field
		// named SubtitleText on DialogueMenu_mc that grows downward from its top and
		// is only positioned once at load, so a _y written here stays. Edge UI
		// Explorer's hudmenu.swf keeps vanilla's SubtitleTextHolder with the field as
		// `textField`.
		struct Target
		{
			std::string_view menu;
			const char*      move;        // the object whose _y is written
			const char*      moveParent;  // its parent: the scale between stage and _y
			const char*      text;        // the field that is measured
			const char*      textParent;  // its parent
			std::string_view what;

			// Which movie originalY belongs to; a reopened menu is a new movie.
			RE::GPtr<RE::GFxMovieView> movie{};
			double                     originalY{ 0.0 };
			bool                       haveOriginal{ false };

			// The _y last written here. A different value read back means the movie moved
			// its own subtitle (its first-frame script, a HUD re-layout), and that move is
			// adopted into originalY rather than overwritten.
			double                     writtenY{ 0.0 };
			bool                       haveWritten{ false };
			bool                       missingReported{ false };
			int                        misses{ 0 };
			Render::SubtitleSlide      slide{};
		};

		std::array<Target, 2> targets{
			Target{ RE::DialogueMenu::MENU_NAME,
				"_root.DialogueMenu_mc.SubtitleText", "_root.DialogueMenu_mc",
				"_root.DialogueMenu_mc.SubtitleText", "_root.DialogueMenu_mc",
				"dialogue subtitle"sv },
			Target{ RE::HUDMenu::MENU_NAME,
				"_root.HUDMovieBaseInstance.SubtitleTextHolder", "_root.HUDMovieBaseInstance",
				"_root.HUDMovieBaseInstance.SubtitleTextHolder.textField", "_root.HUDMovieBaseInstance.SubtitleTextHolder",
				"HUD subtitle"sv },
		};

		bool moved{ false };

		// Dialogue menu elements that sit in the bottom bar. Edge UI's Exit prompt is
		// low on the right and was only hidden because the bars used to cover the
		// whole menu; with the bars under the menu it would show on top of them, so
		// it's faded while the subtitle is in the bar. _alpha rather than _visible:
		// the movie sets _visible itself on rebuild, and a faded button still works.
		struct Faded
		{
			const char*                path;
			RE::GPtr<RE::GFxMovieView> movie{};
			double                     alpha{ 100.0 };
			bool                       have{ false };
		};

		std::array<Faded, 1> faded{
			Faded{ "_root.DialogueMenu_mc.ExitButton" },
		};

		void FadeFurniture()
		{
			auto* ui = RE::UI::GetSingleton();
			auto  view = ui ? ui->GetMovieView(RE::DialogueMenu::MENU_NAME) : RE::GPtr<RE::GFxMovieView>{};
			for (auto& item : faded) {
				if (!view) {
					item = Faded{ item.path };
					continue;
				}
				if (view.get() != item.movie.get()) {
					item = Faded{ item.path };
					item.movie = view;
				}
				RE::GFxValue clip;
				if (!view->GetVariable(&clip, item.path) || !clip.IsDisplayObject()) {
					continue;
				}
				if (!item.have) {
					RE::GFxValue alpha;
					item.alpha = clip.GetMember("_alpha", &alpha) && alpha.IsNumber() ? alpha.GetNumber() : 100.0;
					item.have = true;
				}
				clip.SetMember("_alpha", RE::GFxValue{ 0.0 });
			}
		}

		void RestoreFurniture()
		{
			auto* ui = RE::UI::GetSingleton();
			auto  view = ui ? ui->GetMovieView(RE::DialogueMenu::MENU_NAME) : RE::GPtr<RE::GFxMovieView>{};
			for (auto& item : faded) {
				RE::GFxValue clip;
				if (item.have && view && view.get() == item.movie.get() &&
					view->GetVariable(&clip, item.path) && clip.IsDisplayObject()) {
					clip.SetMember("_alpha", RE::GFxValue{ item.alpha });
				}
				item = Faded{ item.path };
			}
		}

		[[nodiscard]] bool Number(const RE::GFxValue& a_object, const char* a_member, double& a_out)
		{
			RE::GFxValue value;
			if (!a_object.GetMember(a_member, &value) || !value.IsNumber()) {
				return false;
			}
			a_out = value.GetNumber();
			return std::isfinite(a_out);
		}

		// Where a point in a clip's coordinates lands on the stage, vertically. Asked
		// of the movie via localToGlobal, since HUD replacers nest and scale the
		// holder differently.
		[[nodiscard]] bool StageY(RE::GFxMovieView* a_view, const RE::GFxValue& a_clip, double a_y, double& a_out)
		{
			RE::GFxValue point;
			a_view->CreateObject(&point);
			if (!point.IsObject()) {
				return false;
			}
			point.SetMember("x", RE::GFxValue{ 0.0 });
			point.SetMember("y", RE::GFxValue{ a_y });

			// The argument shares its object with `point`; localToGlobal writes the answer
			// back into it.
			const std::array<RE::GFxValue, 1> args{ point };
			auto clip = a_clip;
			if (!clip.Invoke("localToGlobal", nullptr, args.data(), args.size())) {
				return false;
			}
			return Number(point, "y", a_out);
		}

		[[nodiscard]] bool Blank(const char* a_text)
		{
			if (!a_text) {
				return true;
			}
			for (const char* c = a_text; *c; ++c) {
				if (*c != ' ' && *c != '\t' && *c != '\r' && *c != '\n') {
					return false;
				}
			}
			return true;
		}

		struct Block
		{
			double top{ 0.0 };
			double height{ 0.0 };
			bool   haveText{ false };
		};

		// The lines on screen, in stage units. textHeight is the height of the text,
		// not the field; a text field draws its first row two pixels below its top.
		[[nodiscard]] bool Measure(RE::GFxMovieView* a_view, const RE::GFxValue& a_parent, const RE::GFxValue& a_text, Block& a_out)
		{
			double y = 0.0;
			if (!Number(a_text, "_y", y)) {
				return false;
			}
			double yscale = 100.0;
			static_cast<void>(Number(a_text, "_yscale", yscale));
			const double scale = yscale / 100.0;

			double top = 0.0;
			double bottom = 0.0;
			double textHeight = 0.0;
			double height = 0.0;
			if (Number(a_text, "textHeight", textHeight) && textHeight > 0.0) {
				top = y + 2.0 * scale;
				bottom = top + textHeight * scale;
			} else if (Number(a_text, "_height", height) && height > 0.0) {
				top = y;
				bottom = y + height;
			} else {
				return false;
			}

			double stageTop = 0.0;
			double stageBottom = 0.0;
			if (!StageY(a_view, a_parent, top, stageTop) || !StageY(a_view, a_parent, bottom, stageBottom) ||
				!(stageBottom > stageTop)) {
				return false;
			}

			RE::GFxValue visible;
			const bool   shown = !a_text.GetMember("_visible", &visible) || !visible.IsBool() || visible.GetBool();

			RE::GFxValue words;
			const bool   written = a_text.GetMember("text", &words) && words.IsString() && !Blank(words.GetString());

			a_out = { stageTop, stageBottom - stageTop, shown && written };
			return true;
		}

		void Forget(Target& a_target)
		{
			a_target.movie = RE::GPtr<RE::GFxMovieView>{};
			a_target.haveOriginal = false;
			a_target.haveWritten = false;
			a_target.slide.Reset();
		}

		void PlaceOne(Target& a_target, float a_bar, float a_delta)
		{
			auto* ui = RE::UI::GetSingleton();
			auto  view = ui ? ui->GetMovieView(a_target.menu) : RE::GPtr<RE::GFxMovieView>{};

			// The menu is gone, and so is whatever was moved inside it.
			if (!view) {
				Forget(a_target);
				return;
			}
			if (view.get() != a_target.movie.get()) {
				Forget(a_target);
				a_target.movie = view;
			}

			RE::GFxValue move, moveParent, text, textParent;
			const bool   found =
				view->GetVariable(&move, a_target.move) && move.IsDisplayObject() &&
				view->GetVariable(&moveParent, a_target.moveParent) && moveParent.IsDisplayObject() &&
				view->GetVariable(&text, a_target.text) && text.IsDisplayObject() &&
				view->GetVariable(&textParent, a_target.textParent) && textParent.IsDisplayObject();
			// Requested from the menu-open event as well as every frame, and the first can
			// arrive before the movie has placed its children. Two seconds of misses means
			// the movie doesn't have the path.
			if (!found) {
				if (++a_target.misses >= 60 && !a_target.missingReported) {
					a_target.missingReported = true;
					Log::Warn(Log::Category::kStaging,
						"Subtitles in the bar: no {} at {} in this movie; it stays where it is."sv,
						a_target.what, a_target.text);
				}
				return;
			}

			a_target.misses = 0;

			double y = 0.0;
			if (!Number(move, "_y", y)) {
				return;
			}

			// Read once per movie, before the first write; after that, _y is our own
			// value.
			if (!a_target.haveOriginal) {
				a_target.originalY = y;
				a_target.haveOriginal = true;
				a_target.haveWritten = false;
				a_target.slide.Reset();
				Log::Info(Log::Category::kStaging, "Subtitles in the bar: moving the {} (its own _y {:.1f})."sv,
					a_target.what, y);
			} else if (a_target.haveWritten && std::abs(y - a_target.writtenY) > 0.5) {
				Log::Info(Log::Category::kStaging, "Subtitles in the bar: the movie moved the {} by {:.1f}; following it."sv,
					a_target.what, y - a_target.writtenY);
				a_target.originalY += y - a_target.writtenY;
				a_target.haveWritten = false;
			}

			Block block{};
			if (!Measure(view.get(), textParent, text, block)) {
				return;
			}

			// Stage units per unit of the moved object's _y.
			double zero = 0.0;
			double hundred = 0.0;
			if (!StageY(view.get(), moveParent, 0.0, zero) || !StageY(view.get(), moveParent, 100.0, hundred)) {
				return;
			}
			const double scale = (hundred - zero) / 100.0;
			if (std::abs(scale) < 1e-4) {
				return;
			}

			// Where the text would be at the movie's own placement.
			const double ownTop = block.top - (y - a_target.originalY) * scale;

			const auto                 frame = view->GetVisibleFrameRect();
			const Render::SubtitleFrame placement{
				frame.top, frame.bottom, a_bar,
				static_cast<float>(ownTop), static_cast<float>(block.height)
			};
			const auto  spots = Render::SpotsFor(placement);
			const float weight = a_target.slide.Advance(spots.fits, block.haveText, a_delta);
			const float wanted = Render::Blend(spots, weight);

			const double newY = a_target.originalY + (static_cast<double>(wanted) - ownTop) / scale;
			if (std::abs(newY - y) > 0.05) {
				move.SetMember("_y", RE::GFxValue{ newY });
				moved = true;
			}

			// Read back rather than assumed: _y is rounded to a twentieth of a pixel, and
			// comparing with the unrounded value would look like the movie moving it every
			// frame.
			double stored = newY;
			static_cast<void>(Number(move, "_y", stored));
			a_target.writtenY = stored;
			a_target.haveWritten = true;
		}
	}

	void Subtitles::Place(float a_barFraction, bool a_hud, float a_delta)
	{
		PlaceOne(targets[0], a_barFraction, a_delta);
		FadeFurniture();

		if (a_hud) {
			PlaceOne(targets[1], a_barFraction, a_delta);
		} else if (targets[1].haveOriginal) {
			// Filming no longer needs the HUD's line; put it back.
			auto& hud = targets[1];
			auto* ui = RE::UI::GetSingleton();
			auto  view = ui ? ui->GetMovieView(hud.menu) : RE::GPtr<RE::GFxMovieView>{};
			RE::GFxValue move;
			if (view && view.get() == hud.movie.get() && view->GetVariable(&move, hud.move) && move.IsDisplayObject()) {
				move.SetMember("_y", RE::GFxValue{ hud.originalY });
			}
			Forget(hud);
		}
	}

	void Subtitles::Release()
	{
		RestoreFurniture();

		auto* ui = RE::UI::GetSingleton();

		for (auto& target : targets) {
			if (target.haveOriginal && ui) {
				auto view = ui->GetMovieView(target.menu);
				RE::GFxValue move;
				if (view && view.get() == target.movie.get() &&
					view->GetVariable(&move, target.move) && move.IsDisplayObject()) {
					move.SetMember("_y", RE::GFxValue{ target.originalY });
				}
			}
			Forget(target);
			target.missingReported = false;
		}

		if (moved) {
			moved = false;
			Log::Info(Log::Category::kStaging, "Subtitles in the bar: put back where their menus had them."sv);
		}
	}
}

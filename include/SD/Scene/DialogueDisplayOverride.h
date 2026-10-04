#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace SD::Scene
{
	// Node owns the display object's lifetime and supplies property reads/writes.
	// Restores always target that object, never a path in a replacement movie.
	template <class Node>
	class DialogueDisplayOverride
	{
	public:
		void Bind(Node a_node)
		{
			if (node == a_node) {
				return;
			}
			Reset();
			node = std::move(a_node);
		}

		bool SetAlpha(double a_alpha)
		{
			// Full opacity means relinquish our fade. It must not reveal a clip
			// that the movie itself keeps hidden while constructing its rows.
			if (a_alpha >= 100.0) {
				return RestoreAlpha();
			}
			double actual{};
			if (!node.ReadAlpha(actual)) {
				return false;
			}
			// The movie can rebuild or animate this same holder during our fade.
			// Once it writes a new alpha, the old restore value is no longer ours
			// to put back. Check before the no-write return as well.
			if (originalAlpha && actual != writtenAlpha) {
				originalAlpha.reset();
			}
			// Our fade may dim the movie, but must never brighten its own hide
			// or transition. Reapplying an absolute fade alpha over a movie-written
			// zero briefly revealed rows each time selection rebuilt the list.
			// Use the saved movie alpha while we still own the current write, so
			// fading back in is not capped by our own previous, darker frame.
			a_alpha = std::min(a_alpha, originalAlpha.value_or(actual));
			if (std::abs(actual - a_alpha) < 0.5) {
				return true;
			}
			if (!node.WriteAlpha(a_alpha)) {
				return false;
			}
			if (!originalAlpha) {
				originalAlpha = actual;
			}
			writtenAlpha = a_alpha;
			// Remember the stored value if the display API rounds our number.
			// Otherwise the next frame could mistake our own write for the movie's.
			double applied{};
			if (node.ReadAlpha(applied)) {
				writtenAlpha = applied;
			}
			return true;
		}

		bool SetHidden(bool a_hidden)
		{
			if (!a_hidden) {
				return RestoreVisible();
			}
			bool actual{};
			if (!node.ReadVisible(actual)) {
				return false;
			}
			if (!actual) {
				return true;  // already hidden; no visibility debt belongs to us
			}
			if (!node.WriteVisible(false)) {
				return false;
			}
			originalVisible = true;
			return true;
		}

		bool Release()
		{
			const bool alphaDone = RestoreAlpha();
			const bool visibleDone = RestoreVisible();
			return alphaDone && visibleDone;
		}

		void Reset()
		{
			Release();
			// A failed restore may retry only while this object is still bound.
			// Dropping it must never transfer its debt to another object.
			node = {};
			originalAlpha.reset();
			originalVisible.reset();
		}

	private:
		bool RestoreAlpha()
		{
			if (!originalAlpha) {
				return true;
			}
			double actual{};
			if (!node.ReadAlpha(actual)) {
				return false;
			}
			if (actual == writtenAlpha && !node.WriteAlpha(*originalAlpha)) {
				return false;
			}
			originalAlpha.reset();  // also relinquish if another writer changed it
			return true;
		}

		bool RestoreVisible()
		{
			if (!originalVisible) {
				return true;
			}
			bool actual{};
			if (!node.ReadVisible(actual)) {
				return false;
			}
			if (!actual && !node.WriteVisible(*originalVisible)) {
				return false;
			}
			originalVisible.reset();
			return true;
		}

		Node node{};
		std::optional<double> originalAlpha;
		std::optional<bool> originalVisible;
		double writtenAlpha{};
	};
}

#pragma once

#include <cstdint>
#include <optional>

namespace SD::Scene
{
	enum class MenuPhase : std::uint8_t
	{
		kUnknown = 0,  // absent or unrecognized movie state
		kGreeting,     // SHOW_GREETING    = 0
		kTopicList,    // TOPIC_LIST_SHOWN = 1
		kTopicClicked, // TOPIC_CLICKED    = 2
		kTransitioning // TRANSITIONING    = 3
	};

	struct DialoguePhase
	{
		MenuPhase phase{ MenuPhase::kUnknown };
		bool lineInFlight{ false };  // !bAllowProgress, when readable
		bool valid{ false };        // a recognized eMenuState was read

		[[nodiscard]] static constexpr DialoguePhase FromMovie(
			std::optional<double> a_state, std::optional<bool> a_allowProgress)
		{
			DialoguePhase out{};
			if (a_state) {
				// Compare before converting: fractional values and non-finite
				// numbers are not any of the movie's supported states.
				if (*a_state == 0.0) { out.phase = MenuPhase::kGreeting; }
				else if (*a_state == 1.0) { out.phase = MenuPhase::kTopicList; }
				else if (*a_state == 2.0) { out.phase = MenuPhase::kTopicClicked; }
				else if (*a_state == 3.0) { out.phase = MenuPhase::kTransitioning; }
				out.valid = out.phase != MenuPhase::kUnknown;
			}
			if (a_allowProgress) {
				out.lineInFlight = !*a_allowProgress;
			}
			return out;
		}

		[[nodiscard]] constexpr bool HasKnownPhase() const
		{
			return valid && phase != MenuPhase::kUnknown;
		}

		[[nodiscard]] constexpr bool TopicsLive() const
		{
			// Unreadable or unsupported state cannot justify hiding choices.
			return !HasKnownPhase() || phase == MenuPhase::kTopicList;
		}

		[[nodiscard]] constexpr bool CanArmFade() const
		{
			// The fail-open fallback is not evidence that choices have appeared.
			// Arming it would hide the list during a later greeting state.
			return HasKnownPhase() && phase == MenuPhase::kTopicList;
		}
	};
}

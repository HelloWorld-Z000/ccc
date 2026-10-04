#pragma once

#include <cstdint>
#include <span>

namespace SD::Dialogue
{
	// What a scene the game is playing has left to say.
	//
	// A scene is a sequence of phases, and its actions run across ranges of them.
	// A dialogue action flagged as looping is one the game repeats until something
	// else moves the scene on, usually the player (in the Helgen tower, "Move! Up
	// the tower!" until the player goes up). Across Skyrim.esm, 46 of 1,706 scenes
	// have a phase where every line loops, and all of them are waiting on the
	// player: the Greybeards waiting for a Shout, Brynjolf's distraction, the
	// archery targets, wounded NPCs asking for help.
	//
	// From the current phase, the first phase with any line decides: a line that
	// plays once means the scene continues; only looping lines means it's waiting;
	// no lines at all means it has nothing more to say. Phases with only movement
	// or timers are skipped.
	//
	// Pure, so it can be tested. The Director reads the engine's scene into it.
	struct ScriptLine
	{
		std::uint16_t firstPhase{ 0 };  // SNAM
		std::uint16_t lastPhase{ 0 };   // ENAM
		bool          loops{ false };   // FNAM bit 16
	};

	enum class Ahead : std::uint8_t
	{
		kScripted,     // a line still to come that plays once
		kMarkingTime,  // only lines the game repeats until something moves it on
		kNothing,      // no line left in the scene
	};

	// The first phase from a_phase on that has any line. Lines are cut off at
	// a_phaseCount; returns a_phaseCount when no phase has a line.
	[[nodiscard]] constexpr std::uint32_t NextSpokenPhase(std::span<const ScriptLine> a_lines,
		std::uint32_t a_phase, std::uint32_t a_phaseCount)
	{
		std::uint32_t best = a_phaseCount;
		for (const auto& line : a_lines) {
			if (line.lastPhase < line.firstPhase || line.lastPhase < a_phase) {
				continue;
			}
			const std::uint32_t from = line.firstPhase > a_phase ? line.firstPhase : a_phase;
			if (from < best) {
				best = from;
			}
		}
		return best;
	}

	[[nodiscard]] constexpr Ahead Look(std::span<const ScriptLine> a_lines, std::uint32_t a_phase,
		std::uint32_t a_phaseCount)
	{
		const auto phase = NextSpokenPhase(a_lines, a_phase, a_phaseCount);
		if (phase >= a_phaseCount) {
			return Ahead::kNothing;
		}
		for (const auto& line : a_lines) {
			if (line.firstPhase <= phase && phase <= line.lastPhase && !line.loops) {
				return Ahead::kScripted;
			}
		}
		return Ahead::kMarkingTime;
	}
}

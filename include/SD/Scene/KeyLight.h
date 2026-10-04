#pragma once

#include "SD/Scene/LightRig.h"

namespace SD::Scene
{
	// The lamps, placed. A bounded radius with zero ambient lights the face
	// without lifting the room, and a rim lamp separates the subject from an
	// already lit background (a key alone can't). The lamp layout per look is in
	// LightRig.h and isn't exposed as settings.
	class KeyLight
	{
	public:
		static void Engage();
		static void Release();

		// Everything the player sets, in one call; re-read when a conversation opens.
		// a_brightness scales every lamp; a_red/green/blue are the color (0-255),
		// applied to the whole look. Color and brightness stay separate: the color
		// saturates at white, while brightness drives fade. a_shadows makes every lamp
		// cast shadows; ini only and off by default, since it's by far the most
		// expensive option.
		static void Configure(bool a_enabled, int a_brightness, int a_red, int a_green, int a_blue,
			bool a_shadows, int a_fadeHundredths);

		// Which look in AllLooks() is running. Called when a conversation opens, and
		// on a cut when per-angle looks are on. Lamps are created and destroyed as the
		// look needs them; a zero-intensity lamp doesn't exist at all.
		static void SetLook(int a_look);

		// Nudge the whole rig, in camera space rather than world space, so it stays
		// put relative to the shot through cuts and turns:
		//
		//   X  left/right across the frame
		//   Y  toward the camera, or past the subject away from it
		//   Z  down/up
		//
		// World units; 0 is the look as authored.
		static void SetOffset(int a_x, int a_y, int a_z);

		// Which side of the eyeline the camera is on. Looks place the key at a
		// positive angle; flipping it with the camera keeps the key on the same side
		// of the frame on a reverse instead of jumping to the other cheek.
		static void SetSide(float a_side);

		// Placed relative to the camera and the subject, so the look lights the face
		// the shot is on. a_delta drives the cross-fade between looks.
		static void Aim(const RE::NiPoint3& a_camera, const RE::NiPoint3& a_subject, float a_delta);

		[[nodiscard]] static bool Engaged() noexcept;

		// How many lamps are registered with the shadow scene, for the menu's readout.
		[[nodiscard]] static int ActiveLamps() noexcept;
	};
}

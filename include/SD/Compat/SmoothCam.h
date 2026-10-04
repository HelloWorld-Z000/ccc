#pragma once

namespace SD::Compat
{
	// SmoothCam writes the camera every frame, as does the pose override, so
	// without coordination whichever ran last would win and the camera would
	// judder. SmoothCam has an interface for this handoff: Scene Director requests
	// the camera when a conversation opens and releases it when it ends. SmoothCam
	// works as normal otherwise, and its presets aren't touched.
	class SmoothCam
	{
	public:
		// Registers for SmoothCam's interface; it becomes available once SmoothCam
		// answers.
		static void Register();
		static void Request();

		[[nodiscard]] static bool Present() noexcept;

		// True when Scene Director may write the camera, including when SmoothCam
		// isn't installed. False means another consumer holds the camera, and the
		// conversation shouldn't be staged.
		[[nodiscard]] static bool Acquire();
		static void                Release();
		[[nodiscard]] static bool  Holding() noexcept;
	};
}

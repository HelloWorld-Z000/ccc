#pragma once

namespace SD::Compat
{
	// Improved Camera compatibility: detect it and stay off the fields it drives.
	//
	// SmoothCam has a request/release API, so one mod owns the camera at a time.
	// Improved Camera 1.1.x has no such interface (its only exports are the SKSE
	// entry points; tested on 1.1.2.4228). It moves between first and third person
	// by driving the third-person camera state's zoom fields (targetZoomOffset,
	// currentZoomOffset, savedZoomOffset, pitchZoomOffset), which are four of the
	// fields Director::RestoreCameraRest writes at the end of a conversation.
	// Writing them while Improved Camera is installed makes the view pump, so
	// they're skipped. Later versions reportedly expose camera ownership; if those
	// become a target, negotiate through that instead.
	//
	// Detection only; nothing here hooks, patches or calls into Improved Camera.
	class ImprovedCamera
	{
	public:
		// Called once at startup with the conflict report; idempotent. Also reads
		// Improved Camera's profile: its [EVENTS] bScripted setting decides whether it
		// fakes first person during every conversation (dialogue disables movement
		// controls, which it treats as a scripted event), so that's reported at load
		// with the file path.
		static void Detect();

		// Read on the conversation-end path, so it's cheap: Detect did the lookup.
		[[nodiscard]] static bool Present() noexcept;
	};
}

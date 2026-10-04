#pragma once

namespace SD::Scene
{
	// Subtitles in the black bar (off by default). The spoken line sits centred in
	// the bottom bar and follows it as it eases in and out. With no bar, or a bar
	// too thin for the text, the subtitle goes back over the picture, raised just
	// enough to clear the bar.
	//
	// Both subtitles are handled: DialogueMenu's (the player's conversation) and
	// HUDMenu's (other NPCs, used while filming them). Each is put back where its
	// movie had it when the camera lets go. The bars have to be drawn under the
	// interface for this to show; see Render::Letterbox::SetBeneath.
	class Subtitles
	{
	public:
		// Every staged frame while the setting is on. a_barFraction is the bottom bar
		// as a fraction of the screen height (0 when there's none). a_hud also moves
		// HUDMenu's subtitle.
		static void Place(float a_barFraction, bool a_hud, float a_delta);

		// Puts every moved subtitle back. Safe to call when nothing was moved.
		static void Release();
	};
}

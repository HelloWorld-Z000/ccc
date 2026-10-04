#pragma once

namespace SD::Core
{
	// Frame source. Three candidate hooks are installed and counted separately;
	// the log shows which one actually ticks (TESCamera::Update, for one, isn't
	// called every frame by the game).
	enum class Source : std::size_t
	{
		kPlayerCamera = 0,     // TESCamera::Update, vfunc 02; not called every frame, kept as the control
		kPlayerCharacter,      // Actor::Update(float), vfunc 0xAD; carries a real delta
		kThirdPersonState,     // TESCameraState::Update, vfunc 03; where the pose is written
		kCount
	};

	class Tick
	{
	public:
		static void Install();

		[[nodiscard]] static bool Installed() noexcept;

		// True once any source has fired. Different from Installed().
		[[nodiscard]] static bool Ticking() noexcept;

		[[nodiscard]] static std::uint64_t    Count(Source a_source) noexcept;
		[[nodiscard]] static std::uint64_t    Total() noexcept;
		[[nodiscard]] static std::string_view Name(Source a_source) noexcept;

		// The source driving Runtime::OnFrame, or kCount if none has fired yet.
		// Dispatch happens once per frame even with several sources live.
		[[nodiscard]] static Source Primary() noexcept;
	};
}

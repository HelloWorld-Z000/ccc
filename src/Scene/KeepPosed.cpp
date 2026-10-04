#include "SD/Scene/KeepPosed.h"

#include "SD/Core/Logging.h"

namespace SD::Scene
{
	namespace
	{
		// Compared by address, never dereferenced. ModifyAnimationUpdateData runs in
		// the engine's animation update, not on our tick, so the set is an array of
		// atomics read without a lock; a reader that sees half an update keeps one
		// actor posed a frame more or less, which doesn't matter.
		std::array<std::atomic<const RE::Actor*>, KeepPosed::kMaxActors> posed{};
		std::atomic<std::size_t>                                          posedCount{ 0 };

		std::atomic_bool installed{ false };
		Log::OnceFlag    firstPoseReported;

		[[nodiscard]] bool Wanted(const RE::Actor* a_actor)
		{
			const auto count = posedCount.load(std::memory_order_acquire);
			for (std::size_t i = 0; i < count; ++i) {
				if (posed[i].load(std::memory_order_relaxed) == a_actor) {
					return true;
				}
			}
			return false;
		}

		// BSAnimationUpdateData as the policy function fills it. CommonLibSSE leaves
		// these bytes unnamed; meanings from the 1.5.97 disassembly of
		// Actor::ModifyAnimationUpdateData (0x60F3A0):
		//
		//   +0x28  requested bone count: 0xFFFF for an actor rendered this frame, 0
		//          otherwise (only the root is written back)
		//   +0x2A  non-zero forces a full update, skipping animation interpolation
		//   +0x2C  "visible": rendered this frame
		template <std::size_t Slot>
		struct ModifyAnimationUpdateDataHook
		{
			static void thunk(RE::Actor* a_this, RE::BSAnimationUpdateData& a_data)
			{
				func(a_this, a_data);

				if (posedCount.load(std::memory_order_relaxed) == 0 || !Wanted(a_this)) {
					return;
				}
				a_data.flags = 0xFFFF;
				a_data.unk2A = true;
				a_data.unk2C = true;

				if (firstPoseReported.Take()) {
					Log::Info(Log::Category::kCamera,
						"Keeping the people being filmed posed in full while they are off screen."sv);
				}
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		constexpr std::size_t kModifyAnimationUpdateData = 0x79;
	}

	void KeepPosed::Install()
	{
		if (installed.exchange(true)) {
			return;
		}

		// Separate trampolines, like LineWatch: PlayerCharacter overrides the slot and
		// calls Actor's version, so one shared original would call whichever was
		// written last.
		REL::Relocation<std::uintptr_t> character{ RE::VTABLE_Character[0] };
		ModifyAnimationUpdateDataHook<0>::func =
			character.write_vfunc(kModifyAnimationUpdateData, ModifyAnimationUpdateDataHook<0>::thunk);

		REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[0] };
		ModifyAnimationUpdateDataHook<1>::func =
			player.write_vfunc(kModifyAnimationUpdateData, ModifyAnimationUpdateDataHook<1>::thunk);

		Log::Info(Log::Category::kCore,
			"Pose hook installed on Character and PlayerCharacter ModifyAnimationUpdateData (vfunc 0x79)."sv);
	}

	void KeepPosed::Set(std::span<RE::Actor* const> a_actors)
	{
		const auto count = std::min(a_actors.size(), kMaxActors);
		for (std::size_t i = 0; i < count; ++i) {
			posed[i].store(a_actors[i], std::memory_order_relaxed);
		}
		posedCount.store(count, std::memory_order_release);
	}

	void KeepPosed::Clear()
	{
		posedCount.store(0, std::memory_order_release);
	}
}

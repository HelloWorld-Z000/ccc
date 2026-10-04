#include "SD/Scene/KeyLight.h"

#include "SD/Core/Logging.h"

#include <cstring>

namespace SD::Scene
{
	namespace
	{
		// Parameters for the engine's own scene-light registration. The layout and
		// relocation IDs come from the game binary and aren't in CommonLibSSE; getting
		// either wrong gives a light that does nothing or corrupts the shadow scene.
		struct LightCreateParams
		{
			bool            dynamic{ true };
			bool            shadowLight{ false };
			bool            portalStrict{ false };
			bool            affectLand{ true };
			bool            affectWater{ false };
			bool            neverFades{ true };
			std::uint16_t   padding{ 0 };
			float           fov{ 0.0f };
			float           falloff{ 1.0f };
			float           nearDistance{ 5.0f };
			float           depthBias{ 1.0f };
			std::uint32_t   sceneGraphIndex{ 0 };
			std::uint32_t   padding2{ 0 };
			RE::NiAVObject* restrictedNode{ nullptr };
			void*           lensFlareData{ nullptr };
		};
		static_assert(sizeof(LightCreateParams) == 0x30);

		using CreatePointLight = RE::NiPointLight* (*)();
		using AddSceneLight = RE::BSLight* (*)(RE::ShadowSceneNode*, RE::NiLight*, const LightCreateParams*);
		using RemoveSceneLight = void (*)(RE::ShadowSceneNode*, RE::NiPointer<RE::BSLight>&);

		REL::Relocation<CreatePointLight> createPointLight{ REL::RelocationID(69582, 70966) };
		REL::Relocation<AddSceneLight>    addSceneLight{ REL::RelocationID(99692, 106326) };
		REL::Relocation<RemoveSceneLight> removeSceneLight{ REL::RelocationID(99698, 106332) };

		constexpr float kDegToRad = 0.01745329252f;

		// Below this a lamp is removed rather than kept nearly off, so lamps from
		// earlier looks don't stay registered with the shadow scene.
		constexpr float kFadeFloor = 0.004f;

		struct LampState
		{
			RE::NiPointer<RE::NiPointLight> node;
			RE::NiPointer<RE::BSLight>      scene;
			RE::ShadowSceneNode*            attached{ nullptr };

			LampSpec spec{};         // what the look asked for
			float    live{ 0.0f };   // where the fade has actually got to
			float    target{ 0.0f }; // where it is heading
		};

		std::array<LampState, kLampCount> lamps{};

		RE::ShadowSceneNode* sceneRoot{ nullptr };
		bool                 engaged{ false };
		bool                 haveLook{ false };

		bool        enabled{ true };
		float       brightness{ 1.0f };
		RE::NiColor colour{ 1.0f, 0.92f, 0.79f };
		bool        shadows{ false };
		float       fadeSeconds{ 0.18f };
		float       eyelineSide{ 1.0f };

		float offsetX{ 0.0f };
		float offsetY{ 0.0f };
		float offsetZ{ 0.0f };

		Log::OnceFlag firstEngageReported;

		[[nodiscard]] RE::ShadowSceneNode* FindShadowScene(RE::NiAVObject* a_object)
		{
			for (auto* node = a_object ? a_object->parent : nullptr; node; node = node->parent) {
				const auto* rtti = node->GetRTTI();
				if (rtti && rtti->name && std::strcmp(rtti->name, "ShadowSceneNode") == 0) {
					return reinterpret_cast<RE::ShadowSceneNode*>(node);
				}
			}
			return nullptr;
		}

		// Resolved on demand rather than cached at Engage. The shadow scene comes
		// through the player's 3D, which isn't always present when a conversation
		// opens (a cell load straight into dialogue).
		[[nodiscard]] RE::ShadowSceneNode* EnsureScene()
		{
			if (sceneRoot) {
				return sceneRoot;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* root = player ? player->Get3D(false) : nullptr;
			sceneRoot = FindShadowScene(root);
			return sceneRoot;
		}

		void Style(LampState& a_lamp)
		{
			auto* light = a_lamp.node.get();
			if (!light) {
				return;
			}

			auto& data = light->GetLightRuntimeData();

			data.diffuse = colour;

			// Ambient stays at zero: any ambient lifts the whole room and the lamp reads
			// as a wash. Zero ambient with a bounded radius is what lets the background
			// fall away behind the subject.
			data.ambient = RE::NiColor{ 0.0f, 0.0f, 0.0f };

			const auto radius = static_cast<float>(a_lamp.spec.radius);
			data.radius = RE::NiPoint3{ radius, radius, radius };
			data.fade = std::clamp(a_lamp.live * brightness, 0.0f, 4.0f);
		}

		[[nodiscard]] bool CreateLamp(LampState& a_lamp)
		{
			auto* scene = EnsureScene();
			if (!scene) {
				return false;
			}

			auto* created = createPointLight();
			if (!created) {
				Log::Error(Log::Category::kStaging, "Point light creation failed."sv);
				return false;
			}

			a_lamp.node = RE::NiPointer<RE::NiPointLight>{ created };
			Style(a_lamp);

			reinterpret_cast<RE::NiNode*>(scene)->AttachChild(created, true);

			LightCreateParams params{};
			params.shadowLight = shadows;

			auto* registered = addSceneLight(scene, created, &params);
			if (!registered) {
				Log::Error(Log::Category::kStaging, "Scene light registration failed."sv);
				if (created->parent) {
					created->parent->DetachChild(created);
				}
				a_lamp.node.reset();
				return false;
			}

			a_lamp.scene = RE::NiPointer<RE::BSLight>{ registered };
			a_lamp.attached = scene;
			return true;
		}

		void DestroyLamp(LampState& a_lamp)
		{
			if (a_lamp.attached && a_lamp.scene) {
				removeSceneLight(a_lamp.attached, a_lamp.scene);
			}
			if (a_lamp.node && a_lamp.node->parent) {
				a_lamp.node->parent->DetachChild(a_lamp.node.get());
			}
			a_lamp.scene.reset();
			a_lamp.node.reset();
			a_lamp.attached = nullptr;
			a_lamp.live = 0.0f;
		}

		// One lamp's position, from the look's description plus the player's nudge.
		// Everything is angular and relative to the camera distance, so a look lights
		// a master and an extreme close-up the same way.
		[[nodiscard]] RE::NiPoint3 Place(const LampSpec& a_spec, const RE::NiPoint3& a_camera,
			const RE::NiPoint3& a_subject, float a_distance)
		{
			// Bearing from the subject toward the camera, flattened. The camera's height
			// is ignored, so a low-angle shot doesn't drag the rig under the floor.
			float bx = a_camera.x - a_subject.x;
			float by = a_camera.y - a_subject.y;
			const float flat = std::sqrt(bx * bx + by * by);
			if (flat > 0.001f) {
				bx /= flat;
				by /= flat;
			} else {
				bx = 1.0f;
				by = 0.0f;
			}

			// Flipped with the camera's side of the eyeline, so the key stays on the same
			// side of the frame across a cut. See SetSide.
			const float side = eyelineSide < 0.0f ? -1.0f : 1.0f;

			const float azimuth = static_cast<float>(a_spec.azimuth) * kDegToRad * side;
			const float ca = std::cos(azimuth);
			const float sa = std::sin(azimuth);
			const float rx = bx * ca - by * sa;
			const float ry = bx * sa + by * ca;

			const float elevation = static_cast<float>(a_spec.elevation) * kDegToRad;
			const float reach = std::clamp(
				a_distance * (static_cast<float>(a_spec.distance) / 100.0f), 40.0f, 900.0f);

			const float horizontal = reach * std::cos(elevation);
			const float vertical = reach * std::sin(elevation);

			RE::NiPoint3 out{
				a_subject.x + rx * horizontal,
				a_subject.y + ry * horizontal,
				a_subject.z + vertical
			};

			// The player's nudge, in camera space. `b` points from the subject to the
			// camera (the frame's depth axis) and its perpendicular is the frame's
			// horizontal. Rebuilt every frame from the actual camera, and flipped with the
			// eyeline like the azimuth, so "left a bit" stays frame-left through a
			// reverse.
			const float px = -by * side;  // perpendicular to the camera bearing
			const float py = bx * side;

			out.x += px * offsetX + bx * offsetY;
			out.y += py * offsetX + by * offsetY;
			out.z += offsetZ;

			return out;
		}
	}

	void KeyLight::Configure(bool a_enabled, int a_brightness, int a_red, int a_green, int a_blue,
		bool a_shadows, int a_fadeHundredths)
	{
		enabled = a_enabled;
		brightness = std::clamp(a_brightness, 0, 300) / 100.0f;
		colour = ColourFrom(a_red, a_green, a_blue);
		fadeSeconds = std::clamp(a_fadeHundredths, 0, 300) / 100.0f;

		// The shadow flag is only read when a light is added, so the lamps are torn
		// down and rebuilt on the next SetLook. This makes the change apply in the
		// current conversation.
		if (shadows != a_shadows) {
			shadows = a_shadows;
			for (auto& lamp : lamps) {
				if (lamp.node) {
					DestroyLamp(lamp);
				}
			}
		}

		if (!enabled) {
			for (auto& lamp : lamps) {
				lamp.target = 0.0f;
			}
		}

		for (auto& lamp : lamps) {
			Style(lamp);
		}
	}

	void KeyLight::SetOffset(int a_x, int a_y, int a_z)
	{
		offsetX = static_cast<float>(std::clamp(a_x, -400, 400));
		offsetY = static_cast<float>(std::clamp(a_y, -400, 400));
		offsetZ = static_cast<float>(std::clamp(a_z, -400, 400));
	}

	void KeyLight::SetSide(float a_side)
	{
		eyelineSide = a_side < 0.0f ? -1.0f : 1.0f;
	}

	void KeyLight::Engage()
	{
		if (engaged || !enabled) {
			return;
		}

		engaged = true;
		haveLook = false;
		eyelineSide = 1.0f;

		// Creates nothing yet: which lamps exist depends on the look, which isn't set
		// until the conversation stages.
		if (!EnsureScene()) {
			Log::Warn(Log::Category::kStaging,
				"No shadow scene yet; lamps will be placed once the player's 3D resolves."sv);
		}

		if (firstEngageReported.Take()) {
			Log::Info(Log::Category::kStaging,
				"Lighting engaged: brightness {:.2f}, fade {:.2f}s, shadows {}."sv,
				brightness, fadeSeconds, shadows ? "on"sv : "off"sv);
		}
	}

	void KeyLight::Release()
	{
		if (!engaged) {
			return;
		}
		engaged = false;
		haveLook = false;

		for (auto& lamp : lamps) {
			DestroyLamp(lamp);
			lamp.target = 0.0f;
			lamp.spec = LampSpec{};
		}

		// Dropped rather than kept: the node comes through the player's 3D, which
		// doesn't survive a cell change.
		sceneRoot = nullptr;
	}

	void KeyLight::SetLook(int a_look)
	{
		if (!engaged) {
			return;
		}

		const auto looks = AllLooks();
		if (a_look < 0 || static_cast<std::size_t>(a_look) >= looks.size()) {
			a_look = DefaultLook();
		}

		const auto& spec = looks[static_cast<std::size_t>(a_look)];

		for (std::size_t i = 0; i < kLampCount; ++i) {
			auto& lamp = lamps[i];

			lamp.spec = spec.lamps[i];
			lamp.target = enabled ?
							  std::clamp(static_cast<float>(spec.lamps[i].intensity) / 100.0f, 0.0f, 3.0f) :
							  0.0f;

			// A lamp that didn't exist starts dark and fades up, so the cross-fade applies
			// to new lamps too.
			if (lamp.target > 0.0f && !lamp.node) {
				lamp.live = 0.0f;
			}

			Style(lamp);
		}

		haveLook = true;
	}

	bool KeyLight::Engaged() noexcept
	{
		return engaged;
	}

	int KeyLight::ActiveLamps() noexcept
	{
		int count = 0;
		for (const auto& lamp : lamps) {
			count += lamp.node ? 1 : 0;
		}
		return count;
	}

	void KeyLight::Aim(const RE::NiPoint3& a_camera, const RE::NiPoint3& a_subject, float a_delta)
	{
		if (!engaged || !haveLook) {
			return;
		}

		const RE::NiPoint3 toCamera{
			a_camera.x - a_subject.x, a_camera.y - a_subject.y, a_camera.z - a_subject.z
		};
		const float distance = std::sqrt(
			toCamera.x * toCamera.x + toCamera.y * toCamera.y + toCamera.z * toCamera.z);
		if (!(distance > 1.0f)) {
			return;
		}

		// A change of look fades rather than snaps, so it doesn't land as a second big
		// discontinuity on the same frame as the cut. Zero fade time gives a snap.
		const float step = fadeSeconds > 0.0f ?
							   std::clamp(a_delta / fadeSeconds, 0.0f, 1.0f) :
							   1.0f;

		for (std::size_t i = 0; i < kLampCount; ++i) {
			auto& lamp = lamps[i];

			lamp.live += (lamp.target - lamp.live) * step;
			if (lamp.target <= 0.0f && lamp.live < kFadeFloor) {
				lamp.live = 0.0f;
				if (lamp.node) {
					DestroyLamp(lamp);
				}
				continue;
			}

			if (lamp.live <= 0.0f && lamp.target <= 0.0f) {
				continue;
			}

			if (!lamp.node && !CreateLamp(lamp)) {
				continue;  // reported once inside; retried next frame
			}

			Style(lamp);

			auto* light = lamp.node.get();
			light->local.translate = Place(lamp.spec, a_camera, a_subject, distance);

			RE::NiUpdateData update{};
			update.time = 0.0f;
			update.flags = static_cast<RE::NiUpdateData::Flag>(0x2000);
			light->Update(update);
		}
	}
}

#include "Plugin.h"

#include "SD/Compat/DBReV.h"
#include "SD/Compat/SmoothCam.h"
#include "SD/Core/Logging.h"
#include "SD/Runtime.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		switch (a_message->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// Not at plugin load: SKSE loads plugins alphabetically, so SmoothCam isn't
			// registered yet. kPostLoad is the first point where every plugin has loaded.
			SD::Compat::SmoothCam::Register();

			// Same for DBReV: registering before DBReV.dll loads silently fails.
			SD::Compat::DBReV::Register();
			break;

		case SKSE::MessagingInterface::kDataLoaded:
			SD::Runtime::Initialize();
			break;

		case SKSE::MessagingInterface::kPreLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			SD::Runtime::AbandonForLoad();
			break;

		case SKSE::MessagingInterface::kPostLoadGame:
			SD::Runtime::OnGameLoaded();
			break;

		default:
			break;
		}
	}
}

EXTERN_C [[maybe_unused]] __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	SD::Log::Setup(Plugin::NAME);
	SD::Log::Info(SD::Log::Category::kCore, "{} {} loading."sv, Plugin::DISPLAY_NAME, Plugin::VERSION.string("."sv));

	// Pass false: CommonLibSSE-NG 7's a_log parameter defaults to true, and
	// log::init() reopens this file with truncate (wiping what Log::Setup just
	// wrote) and replaces the logger and pattern. Log::Setup owns the log.
	SKSE::Init(a_skse, false);

	// SE and AE only. Every vtable index written here (Actor::Update 0xAD,
	// UpdateInDialogue 0x4C, TESCamera::Update 0x02, camera state Update 0x03) is
	// a flat-Skyrim index; the VR slots differ, and the VTABLE addresses still
	// resolve under VR, so write_vfunc would overwrite the wrong slots.
	if (REL::Module::IsVR()) {
		SD::Log::Error(SD::Log::Category::kCore,
			"Skyrim VR detected. Scene Director hooks flat-Skyrim vtable slots and will not install."sv);
		return false;
	}

	if (const auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	} else {
		SD::Log::Error(SD::Log::Category::kCore, "Messaging interface unavailable."sv);
		return false;
	}

	return true;
}

EXTERN_C [[maybe_unused]] __declspec(dllexport) constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData data;
	data.PluginName(Plugin::NAME);
	data.PluginVersion(Plugin::VERSION);
	data.AuthorName("Scene Director");
	data.UsesAddressLibrary();

	// kVersionIndependentEx_NoStructUse covers plugins that either don't use game
	// structures or handle both the pre- and post-1.6.629 layouts. This build does
	// the latter: every version-dependent member goes through CommonLibSSE-NG's
	// RelocateMemberIfNewer accessors (GetActorRuntimeData() and friends), and the
	// target is built with both ENABLE_SKYRIM_SE and ENABLE_SKYRIM_AE. Without
	// this flag SKSE refuses the plugin on 1.6.629 and later. StructsPost629 would
	// be wrong: it drops support for 1.6.317-1.6.353.
	data.UsesNoStructs();
	return data;
}();

EXTERN_C [[maybe_unused]] __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
	a_info->name = SKSEPlugin_Version.pluginName;
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->version = SKSEPlugin_Version.pluginVersion;
	return true;
}

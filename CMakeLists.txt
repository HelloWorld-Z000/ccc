cmake_minimum_required(VERSION 3.20)

# Scene Director — cinematic dialogue staging and shot direction for Skyrim SE/AE.
#
# Standalone by design: shares no source, no build tree and no settings file with
# Modern Outfit System or Outfit Preview Selector. It is not a fork of either.
#
# What it is: vanilla dialogue holds one camera for a whole conversation. Mods
# that improve on it (Alternate Conversation Camera and its descendants) still
# hold one camera — a better-placed one, expressed as an offset from the player.
# An offset from the player cannot become a reverse shot, which is why no Skyrim
# mod has ever cut. Scene Director drives an absolute world pose instead, so the
# camera can sit behind the person the player is talking to.

project(
	SceneDirector
	VERSION 1.5.0
	LANGUAGES CXX
)

add_library("${PROJECT_NAME}" SHARED)

target_compile_features("${PROJECT_NAME}" PRIVATE cxx_std_23)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

file(GLOB_RECURSE SD_HEADERS LIST_DIRECTORIES false CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE SD_SOURCES LIST_DIRECTORIES false CONFIGURE_DEPENDS "src/*.cpp")

if(NOT SD_SOURCES)
	message(FATAL_ERROR "No sources found under ${CMAKE_CURRENT_SOURCE_DIR}/src.")
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${SD_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${SD_SOURCES})
target_sources("${PROJECT_NAME}" PRIVATE ${SD_HEADERS} ${SD_SOURCES})

configure_file(cmake/Plugin.h.in "${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h" @ONLY)
configure_file(cmake/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc" @ONLY)
target_sources("${PROJECT_NAME}" PRIVATE
	"${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h"
	"${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc")

target_precompile_headers("${PROJECT_NAME}" PRIVATE include/SD/Core/PCH.h)

target_include_directories("${PROJECT_NAME}"
	PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/cmake")

# CommonLibSSE-NG, vendored and pinned under extern/ rather than pulled from the
# colorglass vcpkg port. That port stopped at CharmedBaryon 3.7.0 in May 2023 and
# upstream now marks it unmaintained, which is why this build could not see
# Skyrim 1.7.x: its Module::load_version() classifies a runtime by minor version
# alone — 4 is VR, 6 is AE, and anything else falls through to SE. On 1.7.104
# that "anything else" is the live game. Read as SE, the address library is
# looked for under version-1-7-104-0.bin instead of versionlib-1-7-104-0.bin and
# never loads, every RelocationID resolves its SE half, and the pre-1.6.629
# struct layouts are picked on a post-629 exe. NG 7.2.0 reads minor >= 6 as AE
# and versions the fields 1.7.99 moved, so one DLL still covers 1.5.97 through
# 1.7.104.
#
# Tests are the library's own Catch2 suite; SD does not run them, and leaving
# them on would pull catch2 in for nothing.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# Scene Director draws nothing of its own. It has no renderer, no font atlas and
# no UI dependency — it moves a camera and reads dialogue state.
target_link_libraries("${PROJECT_NAME}"
	PUBLIC  CommonLibSSE::CommonLibSSE
	PRIVATE nlohmann_json::nlohmann_json
	        d3d11 dxgi d3dcompiler)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

if(MSVC)
	add_compile_definitions(_UNICODE)
	target_compile_definitions("${PROJECT_NAME}" PRIVATE "$<$<CONFIG:DEBUG>:DEBUG>")

	target_compile_options("${PROJECT_NAME}" PRIVATE
		# No /await here. It selects the pre-C++20 coroutine model, which moves
		# the coroutine types into std::experimental and takes them out of std,
		# and CommonLibSSE-NG's headers name std::coroutine_handle directly.
		# Nothing in this project is a coroutine; the flag was inherited.
		/MP /W4 /WX /permissive-
		/Zc:alignedNew /Zc:auto /Zc:__cplusplus /Zc:externC /Zc:externConstexpr
		/Zc:forScope /Zc:hiddenFriend /Zc:implicitNoexcept /Zc:lambda
		/Zc:noexceptTypes /Zc:preprocessor /Zc:referenceBinding /Zc:rvalueCast
		/Zc:sizedDealloc /Zc:strictStrings /Zc:ternary /Zc:threadSafeInit
		/Zc:trigraphs
		/wd4200)

	target_compile_options("${PROJECT_NAME}" PRIVATE
		"$<$<CONFIG:DEBUG>:/fp:strict;/ZI;/Od;/Gy>"
		"$<$<CONFIG:RELEASE>:/Zi;/fp:fast;/GL;/Gy-;/Gm-;/Gw;/sdl-;/GS-;/guard:cf-;/O2;/Ob2;/Oi;/Ot;/Oy;/fp:except->")

	target_link_options("${PROJECT_NAME}" PRIVATE
		/WX
		"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
		"$<$<CONFIG:RELEASE>:/LTCG;/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>")
endif()

# These exercise SD's pure geometry and timing policy without a game runtime.
# Keep the option separate from CommonLibSSE-NG's BUILD_TESTS switch above.
option(SD_BUILD_TESTS "Build Scene Director policy tests" ON)
if(SD_BUILD_TESTS)
	enable_testing()
	add_executable(SightGeometryTests tests/SightGeometry.test.cpp)
	add_executable(VisibilityRecoveryTests tests/VisibilityRecovery.test.cpp)
	add_executable(ShotSelectionTests tests/ShotSelection.test.cpp)
	add_executable(ShotAnglesTests tests/ShotAngles.test.cpp)
	add_executable(ExpressionModelTests tests/ExpressionModel.test.cpp)
	add_executable(BrowMotionTests tests/BrowMotion.test.cpp)
	add_executable(UpperFaceTests tests/UpperFace.test.cpp)
	add_executable(RegionalGeometryTests tests/RegionalGeometry.test.cpp)
	add_executable(DialogueDisplayOverrideTests tests/DialogueDisplayOverride.test.cpp)
	add_executable(DialoguePhaseTests tests/DialoguePhase.test.cpp)
	add_executable(PlayerVoiceHandoffTests tests/PlayerVoiceHandoff.test.cpp)
	add_executable(FaceFrameTests tests/FaceFrame.test.cpp)
	add_executable(ReactionShotsTests tests/ReactionShots.test.cpp)
	add_executable(ListenerPerformanceTests tests/ListenerPerformance.test.cpp)
	add_executable(ActingTests tests/Acting.test.cpp)
	add_executable(UpperFaceTestTests tests/UpperFaceTest.test.cpp)
	add_executable(ExpressionProfilesTests tests/ExpressionProfiles.test.cpp)
	add_executable(SettingsCacheTests tests/SettingsCache.test.cpp)
	add_executable(SubtitlePlacementTests tests/SubtitlePlacement.test.cpp)
	add_executable(PersuasionBeatTests tests/PersuasionBeat.test.cpp)
	add_executable(SceneTriggerTests tests/SceneTrigger.test.cpp)
	add_executable(SceneScriptTests tests/SceneScript.test.cpp)
	add_executable(PresetFileTests tests/PresetFile.test.cpp)
	target_link_libraries(PresetFileTests PRIVATE nlohmann_json::nlohmann_json)
	foreach(SD_TEST_TARGET IN ITEMS SightGeometryTests VisibilityRecoveryTests ShotSelectionTests ShotAnglesTests ExpressionModelTests BrowMotionTests UpperFaceTests RegionalGeometryTests DialogueDisplayOverrideTests DialoguePhaseTests PlayerVoiceHandoffTests FaceFrameTests ReactionShotsTests ListenerPerformanceTests ActingTests UpperFaceTestTests ExpressionProfilesTests SettingsCacheTests SubtitlePlacementTests PersuasionBeatTests SceneTriggerTests SceneScriptTests PresetFileTests)
		target_include_directories(${SD_TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
		target_compile_features(${SD_TEST_TARGET} PRIVATE cxx_std_20)
		if(MSVC)
			target_compile_options(${SD_TEST_TARGET} PRIVATE /W4 /WX /permissive-)
		else()
			target_compile_options(${SD_TEST_TARGET} PRIVATE -Wall -Wextra -Wpedantic -Werror)
		endif()
		add_test(NAME ${SD_TEST_TARGET} COMMAND ${SD_TEST_TARGET})
	endforeach()
	# Exercise the real Windows INI reader/writer in an isolated executable Data
	# directory, without loading Skyrim or touching the live installation.
	add_executable(ConfigIntegrationTests tests/ConfigIntegration.test.cpp src/Core/Config.cpp)
	set_target_properties(ConfigIntegrationTests PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/config-integration")
	target_include_directories(ConfigIntegrationTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
	target_precompile_headers(ConfigIntegrationTests PRIVATE include/SD/Core/PCH.h)
	target_compile_features(ConfigIntegrationTests PRIVATE cxx_std_23)
	target_compile_options(ConfigIntegrationTests PRIVATE /W4 /WX /permissive-)
	target_link_libraries(ConfigIntegrationTests PRIVATE CommonLibSSE::CommonLibSSE)
	add_test(NAME ConfigIntegrationTests COMMAND ConfigIntegrationTests)
endif()

# Stage a loose Data layout next to the build so a tester copies one folder.
set(SD_REGIONAL_ASSET "${PROJECT_SOURCE_DIR}/package/regional-assets/SKSE/Plugins/SceneDirector/UpperFace/HPHFemale.sduf"
	CACHE FILEPATH "Locally generated HPH female regional expression asset")
if(EXISTS "${SD_REGIONAL_ASSET}")
	add_custom_target(StageUpperFaceAsset ALL
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/stage/SKSE/Plugins/SceneDirector/UpperFace"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SD_REGIONAL_ASSET}"
			"${CMAKE_BINARY_DIR}/stage/SKSE/Plugins/SceneDirector/UpperFace/HPHFemale.sduf"
		VERBATIM)
endif()
add_custom_command(TARGET "${PROJECT_NAME}" POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/stage/SKSE/Plugins"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"$<TARGET_FILE:${PROJECT_NAME}>" "${CMAKE_BINARY_DIR}/stage/SKSE/Plugins/"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"${CMAKE_CURRENT_SOURCE_DIR}/config/SD.ini" "${CMAKE_BINARY_DIR}/stage/SKSE/Plugins/"
	VERBATIM)

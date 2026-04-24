#pragma once

// Shared wave subsystem helper for wave endpoints.
// Only included from .cpp files that define WAVE_HAS_SDK.

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "SDK/Engine_classes.hpp"
#  include "SDK/Chimera_classes.hpp"
#  define WAVE_HAS_SDK 1
#else
#  define WAVE_HAS_SDK 0
#endif

#if WAVE_HAS_SDK
inline SDK::UCrEnviroWaveSubsystem* GetWaveSubsystem()
{
	auto* world = static_cast<SDK::UWorld*>(SDK::UWorld::GetWorld());
	if (!world) return nullptr;

	// Use the BPF helper to retrieve the world subsystem by class.
	auto* subsys = SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
		world, SDK::UCrEnviroWaveSubsystem::StaticClass());

	return static_cast<SDK::UCrEnviroWaveSubsystem*>(subsys);
}
#endif

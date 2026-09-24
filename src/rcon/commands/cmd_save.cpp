#include "cmd_save.h"
#include "scan_helpers.h"
#include "command_handler.h"
#include "plugin_helpers.h"
#include "game_signatures.h"

#include <windows.h>

// SDK access: UCrSaveSubsystem and UObject::FindObjectFast are engine types.
// Only available when the full SDK is compiled in.
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "CoreUObject_classes.hpp"
#  define CMD_SAVE_HAS_SDK 1
#else
#  define CMD_SAVE_HAS_SDK 0
#endif

namespace Cmd_Save
{
	// -----------------------------------------------------------------------
	// UCrSaveSubsystem::SaveNextSaveGame(UCrSaveSubsystem* this)
	//
	// This is the game's internal function that determines whether the
	// server is dedicated or not and serialises the current world state
	// to the appropriate save file.  We call it directly to force an
	// immediate save on demand.  Pattern lives in game_signatures.h.
	// -----------------------------------------------------------------------
	static constexpr auto SAVE_PATTERN = GameSig::SAVE_SUBSYSTEM_SAVE_NEXT_SAVE_GAME;

	using SaveNextSaveGame_t = void(__fastcall*)(void* thisPtr);
	static SaveNextSaveGame_t g_saveFunc = nullptr;

	// -----------------------------------------------------------------------
	// SEH-protected save call
	//
	// Isolated into its own function because MSVC does not allow __try in
	// functions that contain C++ objects requiring unwinding (std::string).
	// Returns: 0 = success, non-zero = exception code.
	// -----------------------------------------------------------------------
	static DWORD TryCallSave(void* subsystem)
	{
		__try
		{
			g_saveFunc(subsystem);
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return GetExceptionCode();
		}
	}

	// -----------------------------------------------------------------------
	// Command handler — runs on the game thread (dispatched by CommandHandler)
	// -----------------------------------------------------------------------
	static std::string Handle(const std::string& /*args*/)
	{
		LOG_INFO("[RCON] Save command received via RCON.");

		if (!g_saveFunc)
		{
			LOG_ERROR("[RCON] SaveNextSaveGame function not resolved - cannot force save.");
			return "Error: save function not found (pattern not matched).\n";
		}

#if CMD_SAVE_HAS_SDK
		// Find the UCrSaveSubsystem *instance* (not the UClass or CDO).
		SDK::UObject* subsystem = nullptr;
		{
			auto& GObjects = SDK::UObject::GObjects;
			const int32_t count = GObjects->Num();
			for (int32_t i = 0; i < count; ++i)
			{
				SDK::UObject* obj = GObjects->GetByIndex(i);
				if (!obj || !obj->Class)
					continue;

				if (obj->IsDefaultObject())
					continue;

				if (obj->Class->GetName() == "CrSaveSubsystem")
				{
					subsystem = obj;
					break;
				}
			}
		}
#else
		void* subsystem = nullptr;
#endif

		if (!subsystem)
		{
			LOG_ERROR("[RCON] UCrSaveSubsystem instance not found - world may not be loaded yet.");
			return "Error: save subsystem not available (world may not be loaded yet).\n";
		}

#if CMD_SAVE_HAS_SDK
		LOG_INFO("[RCON] Forcing world save via UCrSaveSubsystem::SaveNextSaveGame "
		         "(instance at %p, name: %s)...", subsystem, subsystem->GetName().c_str());
#endif

		DWORD exCode = TryCallSave(subsystem);
		if (exCode == 0)
		{
			LOG_INFO("[RCON] World save completed successfully.");
			return "World saved successfully.\n";
		}

		LOG_ERROR("[RCON] Exception during save (0x%08lX) - save may be incomplete.", exCode);
		return "Error: exception occurred during save.\n";
	}

	// -----------------------------------------------------------------------
	// Registration
	// -----------------------------------------------------------------------
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		if (!self || !scanner)
			return;

		// Optional: a miss disables one RCON command, not the whole server
		// utility, which is what the old scan-at-register path did.
		uintptr_t addr = ScanUtil::ResolveFunction(self, scanner,
			"UCrSaveSubsystem::SaveNextSaveGame", SAVE_PATTERN);
		if (addr)
		{
			g_saveFunc = reinterpret_cast<SaveNextSaveGame_t>(addr);
			LOG_INFO("[RCON] UCrSaveSubsystem::SaveNextSaveGame resolved at 0x%llX",
			         static_cast<unsigned long long>(addr));
		}
		else
		{
			LOG_ERROR("[RCON] UCrSaveSubsystem::SaveNextSaveGame unresolved - "
				"save command will not work until the pattern is updated.");
		}
	}

	void Register(CommandHandler& handler)
	{

		handler.Register(
			{"save", "savegame", "forcesave"},
			"Force an immediate save of the current world state",
			Handle);
	}
}

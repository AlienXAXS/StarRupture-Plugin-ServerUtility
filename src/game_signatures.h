#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// game_signatures.h
//
// Single source of truth for every AOB pattern, struct-field offset and
// instruction-encoding constant the plugin depends on in the game binary.
//
// When a game update breaks the plugin, this is the only file that should
// need touching.  Each entry records where it came from (IDA) so it can be
// re-derived against the new build.
//
// Pattern syntax is the modloader scanner's: space-separated hex bytes with
// "??" for wildcards.
// ---------------------------------------------------------------------------
namespace GameSig
{
	// =======================================================================
	// Function prologue patterns (resolved via IPluginHookScanner)
	// =======================================================================

	// UCrDedicatedServerSettingsComp::ParseSettings
	// Hooked to inject command-line settings in place of DSSettings.txt.
	inline constexpr auto DEDSERVER_SETTINGS_COMP_PARSE_SETTINGS =
		"48 8B C4 55 41 54 48 8D 6C 24";

	// ACrGameModeBase::GetProfessionForNewPlayer
	// Prologue is patched to `mov eax, 1; ret` (force Soldier).
	inline constexpr auto GAMEMODE_GET_PROFESSION_FOR_NEW_PLAYER =
		"48 8B C4 48 89 50 ?? 55 53 41 56 48 8D 68";

	// ACrGameModeBase::PreLogin
	// Body contains the hardcoded `cmp ebx, 4` player limit we patch.
	inline constexpr auto GAMEMODE_PRELOGIN =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 41 56 41 57 48 83 EC ?? 80 3D ?? ?? ?? ?? ?? 49 8B E9";

	// UCrSaveSubsystem::SaveNextSaveGame(UCrSaveSubsystem* this)
	// Called directly by the RCON `save` command.
	inline constexpr auto SAVE_SUBSYSTEM_SAVE_NEXT_SAVE_GAME =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B F9 E8 ?? ?? ?? ?? 33 ED 48 8B D8 48 85 C0 74 ?? E8 ?? ?? ?? ?? 48 8B 53 ?? 4C 8D 40 ?? 48 63 40 ?? 3B 42 ?? 7F ?? 48 8B C8 48 8B 42 ?? ?? ?? ?? ?? 74 ?? 48 8B DD 48 8D 54 24 ?? 48 8B CB E8 ?? ?? ?? ?? 48 63 5C 24";

	// FWindowsPlatformMisc::RequestExit(bool Force, const wchar_t* CallSite)
	// Called directly by the RCON `stop` command.
	inline constexpr auto PLATFORM_MISC_REQUEST_EXIT =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 44 0F B6 05";


	// =======================================================================
	// Struct-field offsets
	// =======================================================================

	// UCrDedicatedServerSettingsComp
	// Derived from the IDA decompilation of ParseSettings.
	namespace DedicatedServerSettingsComp
	{
		inline constexpr size_t OFFSET_SESSION_NAME     = 0xB8; // FString — (char*)this + 184
		inline constexpr size_t OFFSET_SAVEGAME_NAME    = 0xC8; // FString — (char*)this + 200
		inline constexpr size_t OFFSET_SAVE_INTERVAL    = 0xD8; // int32   — (_DWORD*)this + 54 → 216
		inline constexpr size_t OFFSET_START_NEW_GAME   = 0xDC; // bool    — (_BYTE*)this + 220
		inline constexpr size_t OFFSET_LOAD_SAVED_GAME  = 0xDD; // bool    — (_BYTE*)this + 221
	}

	// =======================================================================
	// Intra-function offsets
	// =======================================================================

	// Bytes to scan forward inside PreLogin for the `cmp ebx, imm8` instruction.
	inline constexpr size_t PRELOGIN_SCAN_WINDOW = 0x200;

	// =======================================================================
	// Instruction encodings used for in-body scanning / patching
	// =======================================================================

	// PreLogin player-limit check:
	//   83 FB xx   cmp  ebx, imm8
	//   7C xx      jl   short rel8
	inline constexpr uint8_t PRELOGIN_CMP_EBX_OPCODE = 0x83;
	inline constexpr uint8_t PRELOGIN_CMP_EBX_MODRM  = 0xFB; // /7 ebx
	inline constexpr uint8_t PRELOGIN_JL_OPCODE      = 0x7C;

	// GetProfessionForNewPlayer prologue patch:
	//   B8 01 00 00 00   mov eax, 1   (EProfessionType::Soldier)
	//   C3               ret
	inline constexpr uint8_t GET_PROFESSION_PATCH_BYTES[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
}

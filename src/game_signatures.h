#pragma once

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// game_signatures.h
//
// The AOB patterns and instruction-encoding constants the plugin depends on
// in the game binary. The loader's version map is asked first for every
// function (the keys are in server_utility_fields.h); the patterns here are
// the fallback for a loader or build whose map has no row. Each entry records
// where it came from (IDA) so it can be re-derived against the new build.
//
// Pattern syntax is the modloader scanner's: space-separated hex bytes with
// "??" for wildcards.
// ---------------------------------------------------------------------------
namespace GameSig
{
	// =======================================================================
	// Function prologue patterns (the fallback when the map has no row)
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

	// UReplicationGraph::AddNetworkActor(AActor*) / RemoveNetworkActor(AActor*)
	// Used by the gatherable respawn fix to re-route ACrGatherableSpawnersRepActor
	// after its routing policy has been rewritten. Server binary 0x145ad8aa0 /
	// 0x145af3ca0 (image base 0x140000000).
	inline constexpr auto REPGRAPH_ADD_NETWORK_ACTOR =
		"48 85 D2 0F 84 ?? ?? ?? ?? 48 89 54 24 ?? 55 53 41 56";
	inline constexpr auto REPGRAPH_REMOVE_NETWORK_ACTOR =
		"40 57 41 54 41 56 48 83 EC 70";


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
	//   B8 xx xx xx xx   mov eax, imm32   (the profession, read by name at run time)
	//   C3               ret
	inline constexpr uint8_t GET_PROFESSION_MOV_EAX_IMM32_OPCODE = 0xB8;
	inline constexpr uint8_t GET_PROFESSION_RET_OPCODE           = 0xC3;
	inline constexpr size_t  GET_PROFESSION_PATCH_SIZE           = 6;

	// The profession the patch returns: looked up by name from the loaded enum.
	inline constexpr const char* PROFESSION_ENUM_NAME  = "EProfessionType";
	inline constexpr const char* PROFESSION_ENUM_ENTRY = "Soldier";
}

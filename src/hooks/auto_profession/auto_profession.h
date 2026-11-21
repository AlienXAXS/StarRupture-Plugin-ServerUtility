#pragma once

// ---------------------------------------------------------------------------
// Auto-profession patch for GetProfessionForNewPlayer.
//
// When the MaxPlayers patch is active (>4 players), the game's profession
// selection UI can break for extra players. This patches
// ACrGameModeBase::GetProfessionForNewPlayer to always return Soldier,
// forcing the server to assign a profession immediately so every player
// gets a pawn without relying on the UI.
//
// The patch is a simple prologue overwrite:
//   mov eax, <Soldier>   ; B8 + the value, read by name from EProfessionType
//   ret                  ; C3
// ---------------------------------------------------------------------------

struct IPluginSelf;
struct IPluginHookScanner;

namespace AutoProfessionHook
{
	// Resolve GetProfessionForNewPlayer. Callable only from the plugin's
	// OnPluginLoadHooks export -- the loader refuses scans made anywhere else.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// Patch the resolved prologue. Call once, from the engine init callback
	// (the loader's init thread, engine already up), when MaxPlayers is
	// enabled. Logs and does nothing when the EProfessionType enum or its
	// Soldier entry is not on this build.
	void Install();

	// Restore the original prologue bytes on shutdown.
	void Remove();
}

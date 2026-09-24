#pragma once

struct IPluginSelf;
struct IPluginHookScanner;

namespace Cmd_Save
{
	// Resolve UCrSaveSubsystem::SaveNextSaveGame. Callable only from the plugin's
	// OnPluginLoadHooks export -- the loader refuses scans made anywhere else.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// Register the save command with the mod loader's console.
	// Aliases: savegame, forcesave
	void Register();
}

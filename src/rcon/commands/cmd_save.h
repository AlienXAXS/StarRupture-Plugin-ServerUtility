#pragma once

class CommandHandler;

struct IPluginSelf;
struct IPluginHookScanner;

namespace Cmd_Save
{
	// Resolve UCrSaveSubsystem::SaveNextSaveGame. Callable only from the plugin's
	// OnPluginLoadHooks export -- the loader refuses scans made anywhere else.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// Register the save command.
	// Aliases: save, savegame, forcesave
	void Register(CommandHandler& handler);
}

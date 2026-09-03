#pragma once

class CommandHandler;

struct IPluginSelf;
struct IPluginHookScanner;

namespace Cmd_Stop
{
	// Resolve FWindowsPlatformMisc::RequestExit. Callable only from the plugin's
	// OnPluginLoadHooks export -- the loader refuses scans made anywhere else.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// Register the stop/shutdown command.
	// Aliases: stop, quit, exit, shutdown
	void Register(CommandHandler& handler);

	// Invoke RequestExit directly. Safe to call from any thread after Register().
	void TriggerShutdown();
}

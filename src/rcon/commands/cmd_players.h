#pragma once

namespace Cmd_Players
{
	// Register the players command with the mod loader's console. No
	// aliases: `list` and `who` are the loader's own (plugins, clients).
	void Register();
}

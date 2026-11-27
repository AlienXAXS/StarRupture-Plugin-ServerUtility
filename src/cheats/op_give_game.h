#pragma once

#include "plugin_interface.h"

// The game side of the cheat.give op: finds the item, adds it to the caller's inventory through
// UAuItemsComponent::AddNewItem on the server, and logs the grant. Server builds only. Game thread.
namespace OpGiveGame
{
	// From OnPluginLoadHooks, after ServerUtilityFields::Resolve: the AddNewItem address from the
	// version map. A server with no row for it leaves the op off.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// From PluginInit: gives OpGive its game side.
	void Init();
}

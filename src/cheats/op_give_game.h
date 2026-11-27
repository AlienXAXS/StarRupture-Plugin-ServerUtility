#pragma once

#include "plugin_interface.h"

#include <string>

// The game side of the cheat.give op: finds the item, adds it to the caller's inventory through
// UAuItemsComponent::AddNewItem on the server, and logs the grant. Server builds only. Game thread.
namespace OpGiveGame
{
	// From OnPluginLoadHooks, after ServerUtilityFields::Resolve: the AddNewItem address from the
	// version map. A server with no row for it leaves the op off.
	void Resolve(IPluginSelf* self, IPluginHookScanner* scanner);

	// From PluginInit: gives OpGive its game side.
	void Init();

	enum class AddStatus { Added, Unavailable, NoSubsystem, NoItem, NoCharacter, NothingAdded, Fault };

	struct Added
	{
		AddStatus   status = AddStatus::Fault;
		int         given  = 0;
		std::string name;     // the item's display name; empty when the game gave none
	};

	// Finds the item at assetPath and adds count of it to the character the controller possesses,
	// through AddNewItem. The cheat.give op and the admin give endpoint both come through here; this
	// writes no ledger line. err (errCap bytes, the caller's) holds the lookup's error JSON on NoItem.
	// Game thread; the controller is not kept.
	Added AddItem(void* controller, const char* assetPath, int count, char* err, int errCap);
}

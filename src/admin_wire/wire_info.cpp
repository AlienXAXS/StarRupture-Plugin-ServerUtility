// Both halves declare the admin protocol as their wire id, so the loader can tell
// whether a client and a server speak the same messages.
#include "plugin_interface.h"
#include "admin_wire/admin_wire.h"

// Released plugin versions that share this protocol number. None yet: protocol 1
// is the first one. A later build that keeps protocol 1 needs no entry.
static const PluginWireInfo kWire = { AdminWire::kProtocol, "" };

extern "C" __declspec(dllexport) const PluginWireInfo* GetPluginWireInfo()
{
	return &kWire;
}

#pragma once

// The Server Admin window of the in-game admin tool: panel, keybind and config toggle.

#include "plugin_interface.h"

namespace AdminWindow
{
	void Init(IPluginSelf* self);       // registers the panel, the keybind and the Open toggle
	void Shutdown(IPluginSelf* self);   // unregisters them; call before the state model is freed

	// The panel's render callback. Reads only the state model's copy-out accessor.
	void Render(IModLoaderImGui* imgui);
}

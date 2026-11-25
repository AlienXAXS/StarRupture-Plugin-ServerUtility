#pragma once

// Config of the in-game admin window (client build only; the server build uses plugin_config.h).

#include "plugin_interface.h"

namespace ServerUtilityClientConfig
{
	static constexpr ConfigEntry CONFIG_ENTRIES[] = {
		{
			"Window",
			"ToggleKey",
			ConfigValueType::Keybind,
			"F8",
			"Key that opens and closes the Server Admin window."
		},
		{
			"Window",
			"Open",
			ConfigValueType::Boolean,
			"false",
			"Open the Server Admin window."
		}
	};

	static constexpr ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};
}

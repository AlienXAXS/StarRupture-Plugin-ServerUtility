#include "cmd_stacksize.h"
#include "command_handler.h"
#include "cmd_args.h"
#include "../../host_rules/admin_ops.h"
#include "../../cheats/op_stack_game.h"

#include <string>

namespace Cmd_StackSize
{
	namespace
	{
		const char* const kUsage =
			"Usage:\n"
			"  stacksize            the server stack size\n"
			"  stacksize <0-9999>   raise every stackable item's stack to this size\n"
			"  stacksize off        use the game's own sizes (same as 0)\n";

		std::string State()
		{
			if (!OpStackGame::Available())
				return "stack size: not available on this game build\n";
			const int size = OpStackGame::Get();
			std::string out = "stack size: " + (size > 0 ? std::to_string(size) : std::string("game default"));
			if (size > 0)
				out += ", applied to " + std::to_string(OpStackGame::Raised()) + "/" + std::to_string(OpStackGame::Tracked()) + " items";
			return out + "\n";
		}

		std::string Handle(const std::string& args)
		{
			std::string rest = args;
			const std::string word = CmdArgs::NextWord(rest);
			if (word.empty()) return State();
			if (!rest.empty()) return kUsage;

			const AdminOps::Result r = AdminOps::Run(AdminOps::Caller::FromRcon(), AdminOps::Op{ "server.stacksize", { word } });
			if (!r.ok) return "Error: " + r.text + "\n";
			return "Saved.\n" + State();
		}
	}

	void Register()
	{
		PluginCommands::Register("stacksize", nullptr,
			"stacksize [<0-9999>|off]",
			"Show or set the server-wide item stack size",
			Handle);
	}
}

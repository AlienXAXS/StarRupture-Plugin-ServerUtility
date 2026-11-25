#include "cmd_hostrules.h"
#include "command_handler.h"
#include "cmd_args.h"
#include "../../host_rules/admin_ops.h"
#include "../../host_rules/host_rules.h"
#include "../../host_rules/ini_file.h"
#include "../../host_rules/known_features.h"
#include "../../host_rules/player_list.h"

#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace Cmd_HostRules
{
	namespace
	{
		using CmdArgs::NextWord;
		using IniFile::Lower;
		using CmdArgs::ResolveId;

		const char* const kUsage =
			"Usage:\n"
			"  hostrules [list]                        the rules and whether the mod loader receives them\n"
			"  hostrules allow|deny <Plugin.feature>   one feature (Plugin.* = the whole mod)\n"
			"  hostrules unset <Plugin.feature>        take a rule away\n"
			"  hostrules default allow|deny            what a feature with no rule gets\n"
			"  hostrules message [text|off]            the line players see when a feature is off\n"
			"  hostrules minversion [version|off]      minimum mod loader version (players below it are removed)\n"
			"  hostrules modmin [<plugin> <version|wire|off>]  minimum version of one mod, or wire = a copy the wire check accepts\n"
			"  hostrules reload                        re-read HostRules.ini\n"
			"  hostrules known                         the mod features the server has seen players report\n"
			"  hostrules who <id|number>               what a player's mod loader reported, and its mods\n"
			"Note: a mod name with a space goes in quotes: hostrules deny \"Best Builder.some_feature\", hostrules modmin \"Best Builder\" 1.2.0\n";

		std::string Error(const std::string& text) { return "Error: " + text + "\n"; }

		constexpr size_t kMaxKnownListed = 30;   // keeps the reply inside one RCON packet

		// The remembered catalog, each row with where it came from, and the rules
		// that match none of it.
		std::string Known()
		{
			const RulesModel::Model m = HostRules::Get();
			std::ostringstream oss;
			const std::vector<KnownFeatures::Entry> known = KnownFeatures::Snapshot();
			std::vector<std::string> keys;
			for (const auto& r : m.rules) keys.push_back(r.plugin + "." + r.id);
			oss << KnownFeatures::UnmatchedWarnings(KnownFeatures::UnmatchedKeys(keys, known), kMaxKnownListed);

			if (known.empty())
			{
				oss << "  known features: none yet (a player's mod loader lists them when they join)\n";
				return oss.str();
			}
			oss << "  known features (" << known.size() << "):\n";
			size_t shown = 0;
			for (const KnownFeatures::Entry& e : known)
			{
				if (++shown > kMaxKnownListed)
				{
					oss << "    ... and " << (known.size() - kMaxKnownListed) << " more\n";
					break;
				}
				oss << "    " << e.plugin << "." << e.id << " [" << e.source << "]";
				if (!e.title.empty()) oss << " " << e.title;
				if (!e.version.empty()) oss << " v" << e.version;
				if (!e.offBuilds.empty())
				{
					oss << " off on:";
					for (const std::string& b : e.offBuilds) oss << " " << b;
				}
				oss << " (last seen " << e.lastSeen << ")\n";
			}
			return oss.str();
		}

		std::string State()
		{
			const RulesModel::Model m = HostRules::Get();
			std::ostringstream oss;
			oss << "Host rules (" << HostRules::PushStatus() << "):\n";
			oss << "  default: " << (m.defaultDeny ? "deny" : "allow") << "\n";
			oss << "  message: " << (m.message.empty() ? "(none)" : m.message) << "\n";
			// Default rows are built in; an exact rule above replaces one. Admins are exempt.
			const std::vector<RulesModel::DefaultRow> defaults = RulesModel::ActiveDefaultRows(m);
			if (m.rules.empty() && defaults.empty()) oss << "  rules: none\n";
			for (const auto& r : m.rules) oss << "  " << r.plugin << "." << r.id << " = " << (r.deny ? "deny" : "allow") << "\n";
			for (const auto& d : defaults) oss << "  " << d.plugin << "." << d.id << " = deny (default)\n";
			oss << "  minimum loader version: " << (m.minLoaderVersion.empty() ? "off" : m.minLoaderVersion) << "\n";
			if (m.modMins.empty()) oss << "  minimum mod versions: none\n";
			else for (const auto& r : m.modMins) oss << "  minimum " << r.plugin << ": " << r.version << "\n";
			if (!HostRules::LoaderReceives())
				oss << "  This mod loader has no host rules support, so the rules are saved but not sent.\n";
			oss << "  version minimums: " << HostRules::MinimumsStatus() << "\n";
			return oss.str();
		}

		std::string Saved() { return "Saved.\n" + State(); }

		// One op from RCON; the reply is Saved() or the error.
		std::string Apply(const char* op, std::vector<std::string> args)
		{
			const AdminOps::Result r = AdminOps::Run(AdminOps::Caller::FromRcon(), AdminOps::Op{ op, std::move(args) });
			return r.ok ? Saved() : Error(r.text);
		}

		std::string Who(const std::string& arg)
		{
			std::string id, error;
			if (!ResolveId(arg, id, error)) return Error(error);

			std::string out = "Player " + id;
			PlayerList::Entry entry;
			if (PlayerList::Find(id, entry) && !entry.label.empty())
				out += " (" + entry.label + ")";
			out += ": ";

			HostRules::Reported r;
			if (!HostRules::WhoReported(id, r))
				out += "no mod loader feature list has reached the server.\n";
			else
				out += "the mod loader reported its features " + std::to_string(r.count) + " time(s), last " +
				       std::to_string((GetTickCount64() - r.lastMs) / 1000) + " s ago.\n";

			std::string text;
			switch (HostRules::ClientFeatures(id, text))
			{
			case HostRules::FeaturesState::Ok:
			{
				// First line: loader=<tag> interface=<n>; the rest, one mod feature per line.
				std::istringstream lines(text);
				std::string line;
				bool first = true;
				while (std::getline(lines, line))
				{
					if (first) out += "  mod loader: " + line + "\n  mods:\n";
					else       out += "    " + line + "\n";
					first = false;
				}
				break;
			}
			case HostRules::FeaturesState::NoReport:
				out += "  no report (vanilla or older loader)\n";
				break;
			case HostRules::FeaturesState::NotConnected:
				out += "  not connected\n";
				break;
			case HostRules::FeaturesState::NoLoaderSupport:
				out += "  this mod loader cannot list a player's mods\n";
				break;
			}
			return out;
		}

		std::string Handle(const std::string& args)
		{
			std::string rest = args;
			const std::string sub = Lower(NextWord(rest));

			if (sub.empty() || sub == "list") return State();
			if (sub == "reload")
			{
				const AdminOps::Result r = AdminOps::Run(AdminOps::Caller::FromRcon(), AdminOps::Op{ "rules.reload", {} });
				if (!r.ok) return Error(r.text);
				return "Host rules reloaded.\n" + State();
			}
			if (sub == "known") return Known();
			if (sub == "who")
			{
				const std::string who = NextWord(rest);
				return who.empty() ? std::string(kUsage) : Who(who);
			}

			if (sub == "allow" || sub == "deny" || sub == "unset")
			{
				const std::string key = NextWord(rest);
				if (key.empty() || !rest.empty()) return kUsage;
				return Apply("rule.set", { key, sub });
			}

			if (sub == "default")
			{
				const std::string v = Lower(NextWord(rest));
				if ((v != "allow" && v != "deny") || !rest.empty()) return kUsage;
				return Apply("policy.default", { v });
			}

			if (sub == "message")
			{
				if (rest.empty()) return State();
				return Apply("policy.message", { rest });
			}

			if (sub == "minversion")
			{
				if (rest.empty()) return State();
				const std::string word = NextWord(rest);
				if (!rest.empty()) return kUsage;
				return Apply("min.loader", { word });
			}

			if (sub == "modmin")
			{
				if (rest.empty()) return State();
				const std::string plugin = NextWord(rest);
				const std::string v = NextWord(rest);
				if (v.empty() || !rest.empty()) return kUsage;
				return Apply("min.mod", { plugin, v });
			}

			return kUsage;
		}
	}

	void Register()
	{
		PluginCommands::Register("hostrules", nullptr,
			"hostrules [list|known|allow|deny|unset|default|message|minversion|modmin|reload|who]",
			"Show and edit the host's rules for mod features and versions",
			Handle);
	}
}

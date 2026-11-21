#include "cmd_hostrules.h"
#include "command_handler.h"
#include "cmd_args.h"
#include "../../host_rules/host_rules.h"
#include "../../host_rules/player_list.h"

#include <sstream>

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
		using CmdArgs::Lower;
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
			"  hostrules who <id|number>               what a player's mod loader reported, and its mods\n";

		std::string Error(const std::string& text) { return "Error: " + text + "\n"; }

		std::string State()
		{
			const RulesModel::Model m = HostRules::Get();
			std::ostringstream oss;
			oss << "Host rules (" << HostRules::PushStatus() << "):\n";
			oss << "  default: " << (m.defaultDeny ? "deny" : "allow") << "\n";
			oss << "  message: " << (m.message.empty() ? "(none)" : m.message) << "\n";
			if (m.rules.empty()) oss << "  rules: none\n";
			else for (const auto& r : m.rules) oss << "  " << r.plugin << "." << r.id << " = " << (r.deny ? "deny" : "allow") << "\n";
			oss << "  minimum loader version: " << (m.minLoaderVersion.empty() ? "off" : m.minLoaderVersion) << "\n";
			if (m.modMins.empty()) oss << "  minimum mod versions: none\n";
			else for (const auto& r : m.modMins) oss << "  minimum " << r.plugin << ": " << r.version << "\n";
			if (!HostRules::LoaderReceives())
				oss << "  This mod loader has no host rules support, so the rules are saved but not sent.\n";
			oss << "  version minimums: " << HostRules::MinimumsStatus() << "\n";
			return oss.str();
		}

		std::string Saved() { return "Saved.\n" + State(); }

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
			std::string error;

			if (sub.empty() || sub == "list") return State();
			if (sub == "reload")
			{
				if (!HostRules::Reload(error)) return Error(error);
				return "Host rules reloaded.\n" + State();
			}
			if (sub == "who")
			{
				const std::string who = NextWord(rest);
				return who.empty() ? std::string(kUsage) : Who(who);
			}

			if (sub == "allow" || sub == "deny" || sub == "unset")
			{
				const std::string key = NextWord(rest);
				if (key.empty() || !rest.empty()) return kUsage;
				const bool deny = sub == "deny";
				const bool unset = sub == "unset";
				if (!HostRules::Change(error, [&](RulesModel::Model& m, std::string& e)
				    { return unset ? RulesModel::UnsetRule(m, key, e) : RulesModel::SetRule(m, key, deny, e); }))
					return Error(error);
				return Saved();
			}

			if (sub == "default")
			{
				const std::string v = Lower(NextWord(rest));
				if ((v != "allow" && v != "deny") || !rest.empty()) return kUsage;
				if (!HostRules::Change(error, [&](RulesModel::Model& m, std::string&) { return RulesModel::SetDefault(m, v == "deny"); }))
					return Error(error);
				return Saved();
			}

			if (sub == "message")
			{
				if (rest.empty()) return State();
				const std::string text = Lower(rest) == "off" ? std::string() : rest;
				if (!HostRules::Change(error, [&](RulesModel::Model& m, std::string& e) { return RulesModel::SetMessage(m, text, e); }))
					return Error(error);
				return Saved();
			}

			if (sub == "minversion")
			{
				if (rest.empty()) return State();
				const std::string word = NextWord(rest);
				if (!rest.empty()) return kUsage;
				const std::string v = Lower(word) == "off" ? std::string() : word;
				if (!HostRules::Change(error, [&](RulesModel::Model& m, std::string& e) { return RulesModel::SetMinLoader(m, v, e); }))
					return Error(error);
				return Saved();
			}

			if (sub == "modmin")
			{
				if (rest.empty()) return State();
				const std::string plugin = NextWord(rest);
				const std::string v = NextWord(rest);
				if (v.empty() || !rest.empty()) return kUsage;
				const std::string version = Lower(v) == "off" ? std::string() : v;
				if (!HostRules::Change(error, [&](RulesModel::Model& m, std::string& e) { return RulesModel::SetModMin(m, plugin, version, e); }))
					return Error(error);
				return Saved();
			}

			return kUsage;
		}
	}

	void Register()
	{
		PluginCommands::Register("hostrules", nullptr,
			"hostrules [list|allow|deny|unset|default|message|minversion|modmin|reload|who]",
			"Show and edit the host's rules for mod features and versions",
			Handle);
	}
}

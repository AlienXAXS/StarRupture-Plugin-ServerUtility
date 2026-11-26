#include "cmd_hostrules.h"
#include "command_handler.h"
#include "cmd_args.h"
#include "../../host_rules/admin_ops.h"
#include "../../host_rules/host_rules.h"
#include "../../host_rules/ini_file.h"
#include "../../host_rules/known_features.h"
#include "../../host_rules/line_text.h"
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
			"  hostrules allow|admins|deny <Plugin.feature>  one feature (Plugin.* = the whole mod); admins = only a proven admin\n"
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

		// An RCON packet holds 4096 bytes. A reply stays under kMaxReply: each line is cut
		// to kMaxLine and a list stops with "... and M more" when the next line would not fit.
		constexpr size_t kMaxReply       = 3800;
		constexpr size_t kMaxLine        = 120;
		constexpr size_t kMaxKnownListed = 10;   // unmatched-rule warnings

		// Adds one line (cut to kMaxLine) when it fits under limit; false when it does not.
		bool AddLine(std::string& out, const std::string& line, size_t limit = kMaxReply)
		{
			const std::string cut = LineText::Clip(LineText::OneLine(line), kMaxLine);
			if (out.size() + cut.size() + 1 > limit) return false;
			out += cut + "\n";
			return true;
		}

		// The remembered catalog, each row with where it came from, and the rules
		// that match none of it.
		std::string Known()
		{
			const RulesModel::Model m = HostRules::Get();
			const std::vector<KnownFeatures::Entry> known = KnownFeatures::Snapshot();
			std::vector<std::string> keys;
			for (const auto& r : m.rules) keys.push_back(r.plugin + "." + r.id);

			// Each warning line goes through AddLine, so the reply holds its cap.
			std::string out;
			std::istringstream warnings(KnownFeatures::UnmatchedWarnings(KnownFeatures::UnmatchedKeys(keys, known), kMaxKnownListed));
			std::string warning;
			while (std::getline(warnings, warning))
				if (!AddLine(out, warning)) break;

			if (known.empty())
			{
				AddLine(out, "  known features: none yet (a player's mod loader lists them when they join)");
				return out;
			}
			AddLine(out, "  known features (" + std::to_string(known.size()) + "):");
			size_t shown = 0;
			for (const KnownFeatures::Entry& e : known)
			{
				std::ostringstream row;
				row << "    " << e.plugin << "." << e.id << " [" << e.source << "]";
				if (!e.title.empty()) row << " " << e.title;
				if (!e.version.empty()) row << " v" << e.version;
				if (!e.offBuilds.empty())
				{
					row << " off on:";
					for (const std::string& b : e.offBuilds) row << " " << b;
				}
				row << " (last seen " << e.lastSeen << ")";
				if (!AddLine(out, row.str()))
				{
					out += "    ... and " + std::to_string(known.size() - shown) + " more\n";
					break;
				}
				++shown;
			}
			return out;
		}

		// Lines of a list go in while they fit under limit; the rest are counted as "... and M more".
		void AddList(std::string& out, const std::vector<std::string>& lines, size_t limit)
		{
			for (size_t i = 0; i < lines.size(); ++i)
				if (!AddLine(out, lines[i], limit))
				{
					out += "  ... and " + std::to_string(lines.size() - i) + " more\n";
					return;
				}
		}

		std::string State()
		{
			const RulesModel::Model m = HostRules::Get();
			std::string out;
			AddLine(out, std::string("Host rules (") + HostRules::PushStatus() + "):");
			AddLine(out, std::string("  default: ") + (m.defaultDeny ? "deny" : "allow"));
			AddLine(out, "  message: " + (m.message.empty() ? std::string("(none)") : m.message));

			// The lines after the lists are short and fixed; the lists give up room for them.
			std::string tail;
			AddLine(tail, "  minimum loader version: " + (m.minLoaderVersion.empty() ? std::string("off") : m.minLoaderVersion));
			if (!HostRules::LoaderReceives())
				AddLine(tail, "  This mod loader has no host rules support, so the rules are saved but not sent.");
			AddLine(tail, std::string("  version minimums: ") + HostRules::MinimumsStatus());
			const size_t limit = kMaxReply - tail.size() - 40;   // 40: the "... and M more" line

			// Default rows are built in (admins only); an exact rule above replaces one.
			const std::vector<RulesModel::DefaultRow> defaults = RulesModel::ActiveDefaultRows(m);
			std::vector<std::string> rules;
			for (const auto& r : m.rules) rules.push_back("  " + r.plugin + "." + r.id + " = " + (r.adminsOnly ? "admins" : r.deny ? "deny" : "allow"));
			for (const auto& d : defaults) rules.push_back(std::string("  ") + d.plugin + "." + d.id + " = admins (default)");
			if (rules.empty()) AddLine(out, "  rules: none");
			else AddList(out, rules, limit);

			if (m.modMins.empty()) AddLine(out, "  minimum mod versions: none");
			else
			{
				std::vector<std::string> mins;
				for (const auto& r : m.modMins) mins.push_back("  minimum " + r.plugin + ": " + r.version);
				AddList(out, mins, limit);
			}
			return out + tail;
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

			std::string head = "Player " + id;
			PlayerList::Entry entry;
			if (PlayerList::Find(id, entry) && !entry.label.empty())
				head += " (" + entry.label + ")";
			head += ": ";

			HostRules::Reported r;
			if (!HostRules::WhoReported(id, r))
				head += "no mod loader feature list has reached the server.";
			else
				head += "the mod loader reported its features " + std::to_string(r.count) + " time(s), last " +
				        std::to_string((GetTickCount64() - r.lastMs) / 1000) + " s ago.";
			std::string out;
			AddLine(out, head);

			std::string text;
			switch (HostRules::ClientFeatures(id, text))
			{
			case HostRules::FeaturesState::Ok:
			{
				// First line: loader=<tag> interface=<n>; the rest, one mod feature per line.
				std::istringstream lines(text);
				std::string line;
				bool first = true, full = false;
				size_t more = 0;
				while (std::getline(lines, line))
				{
					if (full) { ++more; continue; }
					const bool ok = first ? AddLine(out, "  mod loader: " + line) && AddLine(out, "  mods:")
					                      : AddLine(out, "    " + line);
					if (!ok) { full = true; ++more; }
					first = false;
				}
				if (more) out += "    ... and " + std::to_string(more) + " more\n";
				break;
			}
			case HostRules::FeaturesState::NoReport:
				AddLine(out, "  no report (vanilla or older loader)");
				break;
			case HostRules::FeaturesState::NotConnected:
				AddLine(out, "  not connected");
				break;
			case HostRules::FeaturesState::NoLoaderSupport:
				AddLine(out, "  this mod loader cannot list a player's mods");
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

			if (sub == "allow" || sub == "admins" || sub == "deny" || sub == "unset")
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
			"hostrules [list|known|allow|admins|deny|unset|default|message|minversion|modmin|reload|who]",
			"Show and edit the host's rules for mod features and versions",
			Handle);
	}
}

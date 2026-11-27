// The Server Admin window. The render callback only reads the state model's copy
// (GetView) and calls its actions: no UObjects, no file I/O.
#include "client/admin_window.h"
#include "client/admin_client.h"
#include "client/admin_view.h"
#include "client/client_config.h"
#include "plugin_helpers.h"

#include <atomic>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <map>
#include <string>

AdminClient::Client* GetAdminClient();   // client_plugin.cpp

using namespace AdminWire;

namespace
{
	// ImGui values the plugin SDK header does not carry (imgui.h, 1.92).
	constexpr int kTableResizable = 1 << 0;
	constexpr int kTableSizingFixedFit = 1 << 13;
	constexpr int kTableScrollX = 1 << 24;
	constexpr int kTableRowBg = 1 << 6;
	constexpr int kTableBordersInnerH = 1 << 7;
	constexpr int kTableBordersInnerV = 1 << 9;
	constexpr int kColumnWidthStretch = 1 << 3;
	constexpr int kWindowAlwaysAutoResize = 1 << 6;
	constexpr int kTableFlags = kTableResizable | kTableRowBg | kTableBordersInnerH | kTableBordersInnerV;

	constexpr const char* kPanelName = "Server Admin";

	IPluginSelf* g_self = nullptr;
	PanelHandle g_panel = nullptr;
	std::atomic<bool> g_panelOpen{ false };

	// Window state that lives between frames.
	struct UiState
	{
		char pairCode[16] = {};
		bool tooManyChanges = false;

		// Mods tab. The message box is re-filled only when the effective message changes.
		char message[AdminWire::kMaxMessageText + 1] = {};
		std::string messageSynced;
		char modFilter[64] = {};

		// Players tab.
		char playerFilter[64] = {};
		char addId[40] = {};
		char kickReason[128] = {};
		std::string kickId, kickName;
		bool openKick = false;

		// Versions tab. Each box is re-filled only when its effective value changes.
		struct MinBox { char text[64] = {}; std::string synced; bool init = false; };
		MinBox loaderMin;
		std::map<std::string, MinBox> modMin;

		// Server tab, filled the same way.
		MinBox stackSize;
	};
	UiState g_ui;

	// Everything one frame draws from.
	struct Frame
	{
		IModLoaderImGui* ui = nullptr;
		AdminClient::Client* client = nullptr;
		AdminClient::View view;
		Snapshot eff;   // snapshot with the staged ops applied
	};

	void Message(Frame& f, const char* text) { f.ui->TextWrapped(text); }

	void ResultLine(Frame& f)
	{
		if (g_ui.tooManyChanges)
			f.ui->TextColored(1.0f, 0.4f, 0.4f, 1.0f, "Too many changes for one batch. Apply first.");
		if (f.view.lastResult.empty()) return;
		if (f.view.lastResultIsError) f.ui->TextColored(1.0f, 0.4f, 0.4f, 1.0f, f.view.lastResult.c_str());
		else f.ui->TextWrapped(f.view.lastResult.c_str());
	}

	void DrawPairing(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		Message(f, "This server needs this PC paired before you can change settings.");
		ui->TextDisabled("The code comes from the server console: RCON \"admin pair <id>\".");
		ui->AlignTextToFramePadding();
		ui->Text("Pairing code");
		ui->SameLine(0, -1);
		ui->InputText("##paircode", g_ui.pairCode, sizeof(g_ui.pairCode));
		ui->SameLine(0, -1);
		ui->BeginDisabled(f.view.pairPending);
		if (ui->Button("Pair"))
		{
			std::string code = g_ui.pairCode;
			for (char& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			f.client->Pair(code);
		}
		ui->EndDisabled();
		ResultLine(f);
	}

	void DrawApplyBar(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		const size_t n = f.view.staged.size();
		char label[48];
		std::snprintf(label, sizeof(label), n ? "Apply (%zu changes)##apply" : "Apply##apply", n);
		ui->BeginDisabled(n == 0 || f.view.applyPending);
		if (ui->Button(label)) { g_ui.tooManyChanges = false; f.client->Apply(); }
		ui->EndDisabled();
		ui->SameLine(0, -1);
		ui->BeginDisabled(n == 0);
		if (ui->Button("Revert")) { g_ui.tooManyChanges = false; f.client->Revert(); }
		ui->EndDisabled();
		if (f.view.applyPending) { ui->SameLine(0, -1); ui->TextDisabled("Waiting for the server..."); }
	}

	Op MakeOp(const char* name, std::initializer_list<std::string> args)
	{
		Op op;
		op.name = name;
		op.args.assign(args.begin(), args.end());
		return op;
	}

	// One edit = one staged op per target: drop the earlier ones for it, stage the new
	// one unless it changes nothing on the server.
	void Edit(Frame& f, const Op& op)
	{
		g_ui.tooManyChanges = !f.client->Replace(op);
	}

	// A key with no rule reads "Default", or "Admins only (default)" when a built-in default row
	// covers it and leaves it to admins. "Deny (default)" when the server predates "admins only",
	// or when default=deny or a "<mod>.*=deny" rule means admins get no default rows either.
	const char* RuleLabel(const std::string& value, bool defaultDenied = false, bool defaultAdminsOnly = true)
	{
		if (value == "allow") return "Allow";
		if (value == "admins") return "Admins only";
		if (value == "deny") return "Deny";
		if (!defaultDenied) return "Default";
		return defaultAdminsOnly ? "Admins only (default)" : "Deny (default)";
	}

	// Default / Allow / Admins only / Deny for one rule key ("Admins only" when the server knows it).
	void DrawRuleCombo(Frame& f, const std::string& key)
	{
		IModLoaderImGui* ui = f.ui;
		const std::string cur = AdminView::RuleValue(f.eff, key);
		const bool defaultDenied = AdminView::IsDefaultDenied(f.eff, key);
		ui->SetNextItemWidth(-FLT_MIN);
		const bool adminsSupported = f.eff.adminsOnlySupported;
		const bool defaultAdminsOnly = adminsSupported && AdminView::IsDefaultAdminsOnly(f.eff, key);
		if (ui->BeginCombo("##rule", RuleLabel(cur, defaultDenied, defaultAdminsOnly)))
		{
			static const char* const values[] = { "unset", "allow", "admins", "deny" };
			for (const char* v : values)
			{
				const std::string val = v;
				if (val == "admins" && !adminsSupported && cur != "admins") continue;
				const std::string shown = RuleLabel(val == "unset" ? std::string() : val, defaultDenied, defaultAdminsOnly);
				if (ui->Selectable(shown.c_str(), (val == "unset" ? std::string() : val) == cur))
					Edit(f, MakeOp("rule.set", { key, val }));
			}
			ui->EndCombo();
		}
	}

	std::string Marked(bool changed, const std::string& text)
	{
		return changed ? "* " + text : text;
	}

	void DrawModPolicy(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		const Snapshot& base = f.view.snapshot;
		const bool policyChanged = base.defaultPolicy != f.eff.defaultPolicy;
		ui->AlignTextToFramePadding();
		ui->Text(Marked(policyChanged, "Mods without a rule are").c_str());
		ui->SameLine(0, -1);
		if (ui->BeginCombo("##defaultpolicy", f.eff.defaultPolicy == "deny" ? "Denied" : "Allowed"))
		{
			if (ui->Selectable("Allowed", f.eff.defaultPolicy != "deny")) Edit(f, MakeOp("policy.default", { "allow" }));
			if (ui->Selectable("Denied", f.eff.defaultPolicy == "deny")) Edit(f, MakeOp("policy.default", { "deny" }));
			ui->EndCombo();
		}

		if (f.eff.message != g_ui.messageSynced)
		{
			std::snprintf(g_ui.message, sizeof(g_ui.message), "%s", f.eff.message.c_str());
			g_ui.messageSynced = f.eff.message;
		}
		ui->Text(Marked(base.message != f.eff.message, "Message shown to refused players").c_str());
		ui->SetNextItemWidth(-FLT_MIN);
		if (ui->InputText("##message", g_ui.message, sizeof(g_ui.message)))
			Edit(f, MakeOp("policy.message", { g_ui.message }));
	}

	void DrawModRow(Frame& f, const char* label, const std::string& key, int players, bool changed)
	{
		IModLoaderImGui* ui = f.ui;
		ui->TableNextRow(0, 0.0f);
		ui->TableNextColumn();
		ui->AlignTextToFramePadding();
		ui->Text(Marked(changed, label).c_str());
		ui->TableNextColumn();
		ui->PushIDStr(key.c_str());
		DrawRuleCombo(f, key);
		ui->PopID();
		ui->TableNextColumn();
		char count[16];
		std::snprintf(count, sizeof(count), "%d", players);
		ui->Text(count);
	}

	void DrawMod(Frame& f, const ModInfo& mod, const std::map<std::string, int>& players)
	{
		IModLoaderImGui* ui = f.ui;
		const auto it = players.find(mod.plugin);
		const int count = it == players.end() ? 0 : it->second;
		const bool changed = AdminView::ModChanged(f.view.snapshot, f.eff, mod);

		std::string title = mod.plugin;
		if (!mod.version.empty()) title += " " + mod.version;
		title = Marked(changed, title) + "##mod_" + mod.plugin;
		if (!ui->CollapsingHeader(title.c_str())) return;

		ui->PushIDStr(mod.plugin.c_str());
		ui->TextDisabled("Default means no rule for that row.");
		if (ui->BeginTable("##features", 3, kTableFlags))
		{
			ui->TableSetupColumn("Feature", kColumnWidthStretch, 3.0f);
			ui->TableSetupColumn("Rule", kColumnWidthStretch, 2.0f);
			ui->TableSetupColumn("Players", kColumnWidthStretch, 1.0f);
			ui->TableHeadersRow();
			DrawModRow(f, "Whole mod", mod.plugin + ".*", count,
				AdminView::RuleChanged(f.view.snapshot, f.eff, mod.plugin + ".*"));
			for (const Feature& feature : mod.features)
			{
				const std::string key = mod.plugin + "." + feature.id;
				DrawModRow(f, feature.label.empty() ? feature.id.c_str() : feature.label.c_str(), key, count,
					AdminView::RuleChanged(f.view.snapshot, f.eff, key));
			}
			ui->EndTable();
		}
		ui->PopID();
	}

	void DrawUnknownRules(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		const std::vector<std::string> keys = AdminView::UnknownRuleKeys(f.view.snapshot);
		if (keys.empty()) return;
		char title[96];
		std::snprintf(title, sizeof(title), "Rules that match no known feature (%zu)##unknown", keys.size());
		if (!ui->CollapsingHeader(title)) return;
		for (const std::string& key : keys)
		{
			const std::string value = AdminView::RuleValue(f.eff, key);
			ui->PushIDStr(key.c_str());
			ui->AlignTextToFramePadding();
			ui->Text(Marked(AdminView::RuleChanged(f.view.snapshot, f.eff, key), key).c_str());
			ui->SameLine(0, -1);
			ui->TextDisabled(value.empty() ? "(removed)" : RuleLabel(value));
			ui->SameLine(0, -1);
			ui->BeginDisabled(value.empty());
			if (ui->SmallButton("Unset")) Edit(f, MakeOp("rule.set", { key, "unset" }));
			ui->EndDisabled();
			ui->PopID();
		}
	}

	void DrawModsTab(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		DrawModPolicy(f);
		ui->Separator();
		ui->SetNextItemWidth(-FLT_MIN);
		ui->InputTextWithHint("##modfilter", "Filter mods and features", g_ui.modFilter, sizeof(g_ui.modFilter));
		const std::map<std::string, int> players = AdminView::PlayersPerMod(f.eff);
		bool any = false;
		for (const ModInfo& mod : f.eff.mods)
		{
			if (!AdminView::ModMatches(mod, g_ui.modFilter)) continue;
			any = true;
			DrawMod(f, mod, players);
		}
		if (!any) ui->TextDisabled(f.eff.mods.empty() ? "The server lists no mods." : "No mod matches the filter.");
		DrawUnknownRules(f);
	}

	// A flag checkbox for one player: stages the full wanted flag state.
	void DrawPlayerFlag(Frame& f, const PlayerInfo& p, AdminView::PlayerFlags flags, bool AdminView::PlayerFlags::* member, const char* id)
	{
		bool value = flags.*member;
		if (f.ui->Checkbox(id, &value))
		{
			flags.*member = value;
			Edit(f, MakeOp("player.set", { p.id, AdminView::FormatWireFlags(flags) }));
		}
	}

	void DrawPlayerSections(Frame& f, const PlayerInfo& p, AdminView::PlayerFlags flags)
	{
		IModLoaderImGui* ui = f.ui;
		char label[32];
		if (flags.sections.empty()) std::snprintf(label, sizeof(label), "All##sections");
		else std::snprintf(label, sizeof(label), "%zu chosen##sections", flags.sections.size());
		if (ui->SmallButton(label)) ui->OpenPopup("##sectionspopup", 0);
		if (ui->BeginPopup("##sectionspopup", 0))
		{
			ui->TextDisabled("Sections this admin may change. None ticked means all.");
			for (const std::string& key : AdminView::SectionKeys(f.eff))
			{
				bool on = false;
				for (const std::string& s : flags.sections) if (s == key) on = true;
				if (!ui->Checkbox(key.c_str(), &on)) continue;
				AdminView::PlayerFlags next = flags;
				next.sections.clear();
				for (const std::string& s : flags.sections) if (s != key) next.sections.push_back(s);
				if (on) next.sections.push_back(key);
				Edit(f, MakeOp("player.set", { p.id, AdminView::FormatWireFlags(next) }));
			}
			ui->EndPopup();
		}
	}

	void DrawPlayerMods(Frame& f, const PlayerInfo& p)
	{
		IModLoaderImGui* ui = f.ui;
		const std::string label = AdminView::CountLabel(p.mods.size(), "mod") + "##mods";
		ui->BeginDisabled(p.mods.empty());
		if (ui->SmallButton(label.c_str())) ui->OpenPopup("##modspopup", 0);
		ui->EndDisabled();
		if (ui->BeginPopup("##modspopup", 0))
		{
			for (const PlayerMod& m : p.mods)
				ui->Text((m.name + (m.version.empty() ? "" : " " + m.version)).c_str());
			ui->EndPopup();
		}
	}

	void DrawPlayerRow(Frame& f, const PlayerInfo& p)
	{
		IModLoaderImGui* ui = f.ui;
		const AdminView::PlayerFlags flags = AdminView::ParseFlags(p.flags);
		const bool changed = AdminView::PlayerChanged(f.view.snapshot, f.eff, p.id);
		ui->TableNextRow(0, 0.0f);
		ui->PushIDStr(p.id.c_str());
		ui->TableNextColumn();
		ui->AlignTextToFramePadding();
		std::string name = p.name.empty() ? p.id : p.name;
		if (p.online) name += " (online)";
		ui->Text(Marked(changed, name).c_str());
		ui->TableNextColumn();
		DrawPlayerFlag(f, p, flags, &AdminView::PlayerFlags::admin, "##admin");
		ui->TableNextColumn();
		DrawPlayerFlag(f, p, flags, &AdminView::PlayerFlags::exemptVersion, "##exemptversion");
		ui->TableNextColumn();
		DrawPlayerFlag(f, p, flags, &AdminView::PlayerFlags::exemptRules, "##exemptrules");
		ui->TableNextColumn();
		DrawPlayerSections(f, p, flags);
		ui->TableNextColumn();
		DrawPlayerMods(f, p);
		ui->TableNextColumn();
		if (p.online && ui->SmallButton("Kick"))
		{
			g_ui.kickId = p.id;
			g_ui.kickName = p.name.empty() ? p.id : p.name;
			g_ui.kickReason[0] = '\0';
			g_ui.openKick = true;
		}
		if (p.online) ui->SameLine(0, -1);
		if (ui->SmallButton("Remove")) Edit(f, MakeOp("player.remove", { p.id }));
		ui->PopID();
	}

	void DrawKickPopup(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		if (g_ui.openKick) { ui->OpenPopup("Kick player", 0); g_ui.openKick = false; }
		if (!ui->BeginPopupModal("Kick player", nullptr, kWindowAlwaysAutoResize)) return;
		ui->Text(("Kick " + g_ui.kickName + " from the server.").c_str());
		ui->InputTextWithHint("##kickreason", "Reason (optional)", g_ui.kickReason, sizeof(g_ui.kickReason));
		if (ui->Button("Kick"))
		{
			f.client->Kick(g_ui.kickId, g_ui.kickReason);
			ui->CloseCurrentPopup();
		}
		ui->SameLine(0, -1);
		if (ui->Button("Cancel")) ui->CloseCurrentPopup();
		ui->EndPopup();
	}

	void DrawPlayersTab(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		ui->SetNextItemWidth(-FLT_MIN);
		ui->InputTextWithHint("##playerfilter", "Filter players", g_ui.playerFilter, sizeof(g_ui.playerFilter));
		ui->Separator();
		ui->InputTextWithHint("##addid", "Add a player by ID", g_ui.addId, sizeof(g_ui.addId));
		ui->SameLine(0, -1);
		ui->BeginDisabled(g_ui.addId[0] == '\0');
		if (ui->Button("Add"))
		{
			Edit(f, MakeOp("player.set", { g_ui.addId, "-admin -exemptversion -exemptrules -sections" }));
			g_ui.addId[0] = '\0';
		}
		ui->EndDisabled();
		ui->Separator();
		const std::vector<size_t> order = AdminView::PlayerOrder(f.eff, g_ui.playerFilter);
		if (order.empty())
			ui->TextDisabled(f.eff.players.empty() ? "No players known yet." : "No player matches the filter.");
		else if (ui->BeginTable("##players", 7, kTableFlags | kTableSizingFixedFit | kTableScrollX))
		{
			ui->TableSetupColumn("Player", 0, 0.0f);
			ui->TableSetupColumn("Admin", 0, 0.0f);
			ui->TableSetupColumn("Exempt version", 0, 0.0f);
			ui->TableSetupColumn("Exempt rules", 0, 0.0f);
			ui->TableSetupColumn("Sections", 0, 0.0f);
			ui->TableSetupColumn("Mods", 0, 0.0f);
			ui->TableSetupColumn("Actions", 0, 0.0f);
			ui->TableHeadersRow();
			for (size_t i : order) DrawPlayerRow(f, f.eff.players[i]);
			ui->EndTable();
		}
		DrawKickPopup(f);
	}

	// A minimum-version row: text box, Set and Off.
	void DrawMinEditor(Frame& f, UiState::MinBox& box, const std::string& eff)
	{
		IModLoaderImGui* ui = f.ui;
		if (!box.init || eff != box.synced)
		{
			std::snprintf(box.text, sizeof(box.text), "%s", eff.c_str());
			box.synced = eff;
			box.init = true;
		}
		ui->SetNextItemWidth(-FLT_MIN);
		ui->InputTextWithHint("##min", "Off", box.text, sizeof(box.text));
	}

	void MinButtons(Frame& f, UiState::MinBox& box, const std::string& plugin)
	{
		IModLoaderImGui* ui = f.ui;
		auto stage = [&](const std::string& value) {
			if (plugin.empty()) Edit(f, MakeOp("min.loader", { value }));
			else Edit(f, MakeOp("min.mod", { plugin, value }));
		};
		ui->BeginDisabled(box.text[0] == '\0');
		if (ui->SmallButton("Set")) stage(box.text);
		ui->EndDisabled();
		ui->SameLine(0, -1);
		if (ui->SmallButton("Off")) stage("off");
	}

	void DrawVersionsTab(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		ui->TextWrapped("Players below a minimum are refused. A mod's minimum may be a version or \"wire\" (the server's own version of it). Off removes the minimum.");
		ui->Separator();
		ui->AlignTextToFramePadding();
		ui->Text(Marked(AdminView::MinChanged(f.view.snapshot, f.eff, std::string()), "Loader minimum").c_str());
		ui->PushIDStr("##loadermin");
		DrawMinEditor(f, g_ui.loaderMin, AdminView::MinValue(f.eff, std::string()));
		MinButtons(f, g_ui.loaderMin, std::string());
		ui->PopID();
		ui->Separator();
		const std::vector<std::string> plugins = AdminView::MinPlugins(f.eff);
		if (plugins.empty()) { ui->TextDisabled("The server lists no mods."); return; }
		if (!ui->BeginTable("##mins", 3, kTableFlags)) return;
		ui->TableSetupColumn("Mod", kColumnWidthStretch, 3.0f);
		ui->TableSetupColumn("Minimum", kColumnWidthStretch, 3.0f);
		ui->TableSetupColumn("Actions", kColumnWidthStretch, 1.5f);
		ui->TableHeadersRow();
		for (const std::string& plugin : plugins)
		{
			ui->TableNextRow(0, 0.0f);
			ui->PushIDStr(plugin.c_str());
			ui->TableNextColumn();
			ui->AlignTextToFramePadding();
			ui->Text(Marked(AdminView::MinChanged(f.view.snapshot, f.eff, plugin), plugin).c_str());
			ui->TableNextColumn();
			UiState::MinBox& box = g_ui.modMin[plugin];
			DrawMinEditor(f, box, AdminView::MinValue(f.eff, plugin));
			ui->TableNextColumn();
			MinButtons(f, box, plugin);
			ui->PopID();
		}
		ui->EndTable();
	}

	void DrawServerTab(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		ui->TextWrapped("Raises every stackable item's stack to this size on the server. 0 or Off uses the game's own sizes.");
		ui->Separator();
		const bool available = f.view.snapshot.stackAvailable;
		if (!available) ui->TextDisabled("Not available on this game build.");
		ui->BeginDisabled(!available);
		ui->AlignTextToFramePadding();
		ui->Text(Marked(AdminView::ServerChanged(f.view.snapshot, f.eff), "Stack size").c_str());
		UiState::MinBox& box = g_ui.stackSize;
		const std::string eff = f.eff.stackSize > 0 ? std::to_string(f.eff.stackSize) : std::string();
		if (!box.init || eff != box.synced)
		{
			std::snprintf(box.text, sizeof(box.text), "%s", eff.c_str());
			box.synced = eff;
			box.init = true;
		}
		ui->SetNextItemWidth(-FLT_MIN);
		ui->InputTextWithHint("##stacksize", "Game default", box.text, sizeof(box.text));
		ui->BeginDisabled(box.text[0] == '\0');
		if (ui->SmallButton("Set")) Edit(f, MakeOp("server.stacksize", { box.text }));
		ui->EndDisabled();
		ui->SameLine(0, -1);
		if (ui->SmallButton("Off")) Edit(f, MakeOp("server.stacksize", { "off" }));
		ui->EndDisabled();
	}

	void DrawReady(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		if (!f.view.haveSnapshot)
		{
			Message(f, "Loading the server settings...");
			return;
		}
		ResultLine(f);
		// The tab bar stays put; each tab scrolls inside its own child, which
		// leaves one button row at the bottom for Apply and Revert.
		if (ui->BeginTabBar("##admintabs", 0))
		{
			if (ui->BeginTabItem("Mods", nullptr, 0))
			{
				if (ui->BeginChild("##modsbody", 0.0f, -ui->GetFrameHeightWithSpacing(), false)) DrawModsTab(f);
				ui->EndChild();
				ui->EndTabItem();
			}
			if (ui->BeginTabItem("Players", nullptr, 0))
			{
				if (ui->BeginChild("##playersbody", 0.0f, -ui->GetFrameHeightWithSpacing(), false)) DrawPlayersTab(f);
				ui->EndChild();
				ui->EndTabItem();
			}
			if (ui->BeginTabItem("Versions", nullptr, 0))
			{
				if (ui->BeginChild("##versionsbody", 0.0f, -ui->GetFrameHeightWithSpacing(), false)) DrawVersionsTab(f);
				ui->EndChild();
				ui->EndTabItem();
			}
			if (f.view.snapshot.stackSize >= 0 && ui->BeginTabItem("Server", nullptr, 0))
			{
				if (ui->BeginChild("##serverbody", 0.0f, -ui->GetFrameHeightWithSpacing(), false)) DrawServerTab(f);
				ui->EndChild();
				ui->EndTabItem();
			}
			ui->EndTabBar();
		}
		DrawApplyBar(f);
	}

	void DrawStatus(Frame& f)
	{
		switch (f.view.status)
		{
		case AdminClient::Status::Waiting:
			f.ui->TextDisabled("Waiting for the server...");
			break;
		case AdminClient::Status::NoInGameAdmin:
			Message(f, "This server does not offer in-game admin.");
			break;
		case AdminClient::Status::NotAdmin:
			Message(f, "You are not an admin on this server.");
			break;
		case AdminClient::Status::UpdateClient:
			Message(f, "Update the ServerUtility admin client: this server speaks a newer version.");
			break;
		case AdminClient::Status::PairNeeded:
			DrawPairing(f);
			break;
		case AdminClient::Status::Ready:
			DrawReady(f);
			break;
		}
	}

	void OnPanelClosed(PanelHandle handle)
	{
		if (handle == g_panel) g_panelOpen = false;
	}

	void SetOpen(bool open)
	{
		if (!g_self || !g_self->hooks->UI || !g_panel) return;
		if (open == g_panelOpen) return;
		if (open) g_self->hooks->UI->SetPanelOpen(g_panel);
		else g_self->hooks->UI->SetPanelClose(g_panel);
		g_panelOpen = open;
	}

	void OnToggleKey(EModKey, EModKeyEvent event)
	{
		if (event != EModKeyEvent::Pressed) return;
		SetOpen(!g_panelOpen);
	}

	void OnConfigChanged(const char* section, const char* key, const char* newValue)
	{
		if (!section || !key || !newValue) return;
		if (std::string(section) != "Window" || std::string(key) != "Open") return;
		const std::string v = newValue;
		SetOpen(v == "true" || v == "1" || v == "True");
	}

	std::string ToggleKey()
	{
		char buf[64] = {};
		g_self->config->ReadString(g_self, "Window", "ToggleKey", buf, sizeof(buf), "F8");
		return buf;
	}
}

namespace AdminWindow
{
	void Render(IModLoaderImGui* imgui)
	{
		AdminClient::Client* client = GetAdminClient();
		if (!client) return;
		Frame f;
		f.ui = imgui;
		f.client = client;
		client->GetView(f.view);
		if (f.view.haveSnapshot) f.eff = AdminView::ApplyStaged(f.view.snapshot, f.view.staged);
		DrawStatus(f);
	}

	void Init(IPluginSelf* self)
	{
		g_self = self;
		if (!self->hooks || !self->hooks->UI) return;

		self->config->InitializeFromSchema(self, &ServerUtilityClientConfig::SCHEMA);

		static PluginPanelDesc desc = { kPanelName, kPanelName, &Render };
		g_panel = self->hooks->UI->RegisterPanel(&desc);
		self->hooks->UI->RegisterOnPanelWindowClosed(&OnPanelClosed);
		self->hooks->UI->RegisterOnConfigChanged(self, &OnConfigChanged);
		if (self->hooks->Input)
			self->hooks->Input->RegisterKeybindByName(ToggleKey().c_str(), EModKeyEvent::Pressed, &OnToggleKey);
		if (self->config->ReadBool(self, "Window", "Open", false)) SetOpen(true);
	}

	void Shutdown(IPluginSelf* self)
	{
		if (self->hooks && self->hooks->UI)
		{
			if (self->hooks->Input)
				self->hooks->Input->UnregisterKeybindByName(ToggleKey().c_str(), EModKeyEvent::Pressed, &OnToggleKey);
			self->hooks->UI->UnregisterOnConfigChanged(self, &OnConfigChanged);
			self->hooks->UI->UnregisterOnPanelWindowClosed(&OnPanelClosed);
			if (g_panel) self->hooks->UI->UnregisterPanel(g_panel);
		}
		g_panel = nullptr;
		g_panelOpen = false;
		g_self = nullptr;
	}
}

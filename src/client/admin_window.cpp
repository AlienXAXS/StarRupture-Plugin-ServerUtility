// The Server Admin window. The render callback only reads the state model's copy
// (GetView) and calls its actions: no UObjects, no file I/O.
#include "client/admin_window.h"
#include "client/admin_client.h"
#include "client/admin_view.h"
#include "client/client_config.h"
#include "plugin_helpers.h"

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
	constexpr int kTableRowBg = 1 << 6;
	constexpr int kTableBordersInnerH = 1 << 7;
	constexpr int kTableBordersInnerV = 1 << 9;
	constexpr int kColumnWidthStretch = 1 << 3;
	constexpr int kWindowAlwaysAutoResize = 1 << 6;
	constexpr int kTableFlags = kTableResizable | kTableRowBg | kTableBordersInnerH | kTableBordersInnerV;

	constexpr const char* kPanelName = "Server Admin";

	IPluginSelf* g_self = nullptr;
	PanelHandle g_panel = nullptr;
	bool g_panelOpen = false;

	// Window state that lives between frames.
	struct UiState
	{
		char pairCode[16] = {};
		bool tooManyChanges = false;

		// Mods tab. The message box is re-filled only when the effective message changes.
		char message[512] = {};
		std::string messageSynced;
		char modFilter[64] = {};
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
		if (ui->Button("Pair"))
		{
			std::string code = g_ui.pairCode;
			for (char& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			f.client->Pair(code);
		}
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
		for (size_t i : AdminView::StagedWithTarget(f.view.staged, op)) f.client->Unstage(i);
		g_ui.tooManyChanges = false;
		if (AdminView::IsNoop(f.view.snapshot, op)) return;
		if (!f.client->Stage(op)) g_ui.tooManyChanges = true;
	}

	const char* RuleLabel(const std::string& value)
	{
		if (value == "allow") return "Allow";
		if (value == "deny") return "Deny";
		return "Default";
	}

	// Default / Allow / Deny for one rule key.
	void DrawRuleCombo(Frame& f, const std::string& key)
	{
		IModLoaderImGui* ui = f.ui;
		const std::string cur = AdminView::RuleValue(f.eff, key);
		ui->SetNextItemWidth(-FLT_MIN);
		if (ui->BeginCombo("##rule", RuleLabel(cur)))
		{
			static const char* const values[] = { "unset", "allow", "deny" };
			for (const char* v : values)
			{
				const std::string val = v;
				const std::string shown = RuleLabel(val == "unset" ? std::string() : val);
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

	void DrawReady(Frame& f)
	{
		IModLoaderImGui* ui = f.ui;
		if (!f.view.haveSnapshot)
		{
			Message(f, "Loading the server settings...");
			return;
		}
		ResultLine(f);
		// Leave one button row at the bottom for Apply and Revert.
		const bool child = ui->BeginChild("##content", 0.0f, -ui->GetFrameHeightWithSpacing(), false);
		if (child && ui->BeginTabBar("##admintabs", 0))
		{
			if (ui->BeginTabItem("Mods", nullptr, 0))
			{
				DrawModsTab(f);
				ui->EndTabItem();
			}
			ui->EndTabBar();
		}
		ui->EndChild();
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

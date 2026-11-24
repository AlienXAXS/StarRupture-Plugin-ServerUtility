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
		ResultLine(f);
	}

	void DrawReady(Frame& f)
	{
		if (!f.view.haveSnapshot)
		{
			Message(f, "Loading the server settings...");
			return;
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

#ifdef MODLOADER_CLIENT_BUILD

#include "UI.h"
#include "Config/Config.h"
#include "Network/Network.h"
#include "PersistentId/PersistentIdSystem.h"
#include "Stats/Stats.h"
#include "plugin.h"
#include "plugin_helpers.h"


#include <atomic>
#include <cstdint>
#include <cstdio>

namespace
{
	WidgetHandle g_widget = nullptr;
	std::atomic_bool g_visible{ false };
	bool g_keybindRegistered = false;
	std::atomic_bool g_lateAttachmentWarningPending{ false };
	std::atomic_bool g_lateAttachmentPopupOpened{ false };

	PluginWindowHints g_hints = {};

	constexpr float kWindowWidth = 270.0f;
	constexpr float kWindowHeight = 180.0f;

	/*
	 * No NoMove: the user can move the window when the ModLoader UI/cursor
	 * is available.
	 *
	 * No NoSavedSettings: ImGui can retain the user's window position.
	 */
	constexpr int kWindowFlags =
		PluginWindowFlags_NoResize |
		PluginWindowFlags_NoScrollbar;

	void UpdateHints()
	{
		g_hints.width = kWindowWidth;
		g_hints.height = kWindowHeight;

		/*
		 * Let ImGui place the widget on first use and retain the user's
		 * subsequent position.
		 */
		g_hints.pos_x = -1.0f;
		g_hints.pos_y = -1.0f;

		g_hints.pivot_x = 0.0f;
		g_hints.pivot_y = 0.0f;

		g_hints.size_cond = 0;
		g_hints.pos_cond = 1;

		g_hints.extra_window_flags = kWindowFlags;
	}

	const char* GetSessionName(
		EPluginNetMode netMode)
	{
		switch (netMode)
		{
		case EPluginNetMode::Standalone:
			return "Solo";

		case EPluginNetMode::ListenServer:
			return "Local Multiplayer";

		case EPluginNetMode::DedicatedServer:
			return "Dedicated Server";

		case EPluginNetMode::Client:
			return "Remote Multiplayer";

		default:
			return "Unknown";
		}
	}

	void Render(
		IModLoaderImGui* ui)
	{
		if (ui == nullptr)
		{
			return;
		}

		if (g_lateAttachmentWarningPending)
		{
			constexpr const char* popupName =
				"PersistentIdFix - ID reuse disabled";

			if (!g_lateAttachmentPopupOpened)
			{
				ui->OpenPopup(popupName, 0);
				g_lateAttachmentPopupOpened = true;
			}

			bool popupOpen = true;

			constexpr int popupFlags =
				(1 << 6) | PluginWindowFlags_NoSavedSettings;

			if (ui->BeginPopupModal(popupName, &popupOpen, popupFlags))
			{
				constexpr float warningContentWidth = 540.0f;

				// AlwaysAutoResize follows measured content. Give wrapped text
				// an explicit endpoint so measurement is independent of any
				// stale/narrow popup work rectangle loaded by ImGui.
				ui->Dummy(warningContentWidth, 0.0f);
				ui->PushTextWrapPos(
					ui->GetCursorPosX() + warningContentWidth);
				ui->TextWrapped(
					"PersistentIdFix was loaded after this game world had already begun.");
				ui->Spacing();
				ui->TextWrapped(
					"The plugin did not observe the complete load sequence, so persistent ID reuse cannot be enabled safely for this running world.");
				ui->Spacing();
				ui->TextWrapped(
					"PersistentIdFix will remain fail-closed and use bounded monotonic allocation only.");
				ui->Spacing();
				ui->TextWrapped(
					"Return to the main menu and load the world again with PersistentIdFix already active before relying on ID reuse.");
				ui->Spacing();
				ui->TextWrapped(
					"Existing saves can normally be loaded again safely. PersistentIdFix cannot repair a save in which persistent-ID exhaustion or overflow had already occurred.");
				ui->PopTextWrapPos();
				ui->Spacing();

				constexpr float okButtonWidth = 120.0f;
				float availableWidth = 0.0f;
				float availableHeight = 0.0f;
				ui->GetContentRegionAvail(
					&availableWidth,
					&availableHeight);
				(void)availableHeight;

				const float currentCursorX =
					ui->GetCursorPosX();

				if (availableWidth > okButtonWidth)
				{
					ui->SetCursorPosX(
						currentCursorX +
						(availableWidth - okButtonWidth) * 0.5f);
				}

				if (ui->ButtonSized("OK", okButtonWidth, 0.0f))
				{
					ui->CloseCurrentPopup();
					g_lateAttachmentWarningPending = false;
					g_lateAttachmentPopupOpened = false;

					if (!g_visible &&
						g_self != nullptr &&
						g_self->hooks != nullptr &&
						g_self->hooks->UI != nullptr &&
						g_widget != nullptr)
					{
						g_self->hooks->UI->SetWidgetVisible(
							g_widget,
							false);
					}
				}

				ui->EndPopup();
			}
			else if (!popupOpen)
			{
				g_lateAttachmentWarningPending = false;
				g_lateAttachmentPopupOpened = false;
			}
		}

		if (!g_visible)
		{
			return;
		}

		/*
		 * Use a semi-transparent standard ImGui window background.
		 *
		 * The normal window background and border remain enabled because
		 * NoBackground is no longer part of kWindowFlags.
		 */
		ui->SetNextWindowBgAlpha(0.80f);

		if (g_self == nullptr ||
			g_self->hooks == nullptr ||
			g_self->hooks->NetMode == nullptr ||
			g_self->hooks->NetMode->GetNetMode == nullptr)
		{
			ui->Text(
				"Game Session: Unknown");

			return;
		}

		const EPluginNetMode localNetMode =
			g_self->hooks->NetMode->GetNetMode();

		/*
		 * A client has no local PersistentIdFix statistics subsystem.
		 *
		 * Its statistics therefore come entirely from the remote authority.
		 */
		if (localNetMode == EPluginNetMode::Client)
		{
			const PersistentIdFixNetwork::RemoteDisplayState remoteState =
				PersistentIdFixNetwork::GetRemoteDisplayState();

			ui->Text(
				"Remote Session:");

			ui->SameLine(
				0.0f,
				8.0f);

			if (remoteState.hasData)
			{
				ui->Text(
					GetSessionName(
						remoteState.snapshot.netMode));

				char line[128];

				std::snprintf(
					line,
					sizeof(line),
					"Total Entities: %u",
					remoteState.snapshot.totalEntities);

				ui->Text(line);

				std::snprintf(
					line,
					sizeof(line),
					"ID Counter value: %u",
					remoteState.snapshot.idCounterValue);

				ui->Text(line);

				std::snprintf(
					line,
					sizeof(line),
					"Remaining ID count: %llu",
					static_cast<unsigned long long>(
						remoteState.snapshot.remainingIDCount));

				ui->Text(line);

				std::snprintf(
					line,
					sizeof(line),
					"Reusable ID count: %llu",
					static_cast<unsigned long long>(
						remoteState.snapshot.reusableIDCount));

				ui->Text(line);

				std::snprintf(
					line,
					sizeof(line),
					"Reusable ID ranges: %llu",
					static_cast<unsigned long long>(
						remoteState.snapshot.reusableIDRanges));

				ui->Text(line);

				if (remoteState.snapshot.issuedIDsPerMinuteAvailable)
				{
					const int issuedIDsPerMinute =
						static_cast<int>(
							remoteState.snapshot.issuedIDsPerMinute + 0.5);

					std::snprintf(
						line,
						sizeof(line),
						"Issued IDs per minute: %d",
						issuedIDsPerMinute);

					ui->Text(line);
				}
				else
				{
					ui->Text(
						"Issued IDs per minute: ...");
				}

				return;
			}

			ui->Text(
				"Waiting for remote PersistentIdFix...");

			if (remoteState.missingPlugin)
			{
				ui->Text(
					"The remote host does not have PersistentIdFix installed");
			}

			return;
		}

		/*
		 * Standalone and listen-server sessions use the local statistics.
		 */
		const PersistentIdFixStats::Snapshot snapshot =
			PersistentIdFixStats::GetSnapshot();

		char line[128];

		ui->Text(
			"Local Session:");

		ui->SameLine(
			0.0f,
			8.0f);

		ui->Text(
			GetSessionName(
				snapshot.netMode));

		std::snprintf(
			line,
			sizeof(line),
			"Total Entities: %u",
			snapshot.totalEntities);

		ui->Text(line);

		std::snprintf(
			line,
			sizeof(line),
			"ID Counter value: %u",
			snapshot.idCounterValue);

		ui->Text(line);

		std::snprintf(
			line,
			sizeof(line),
			"Remaining ID count: %llu",
			static_cast<unsigned long long>(
				snapshot.remainingIDCount));

		ui->Text(line);

		std::snprintf(
			line,
			sizeof(line),
			"Reusable ID count: %llu",
			static_cast<unsigned long long>(
				snapshot.reusableIDCount));

		ui->Text(line);

		std::snprintf(
			line,
			sizeof(line),
			"Reusable ID ranges: %llu",
			static_cast<unsigned long long>(
				snapshot.reusableIDRanges));

		ui->Text(line);

		if (snapshot.issuedIDsPerMinuteAvailable)
		{
			const int issuedIDsPerMinute =
				static_cast<int>(
					snapshot.issuedIDsPerMinute + 0.5);

			std::snprintf(
				line,
				sizeof(line),
				"Issued IDs per minute: %d",
				issuedIDsPerMinute);

			ui->Text(line);
		}
		else
		{
			ui->Text(
				"Issued IDs per minute: ...");
		}
	}

	void OnUIToggleKeyPressed(
		EModKey key,
		EModKeyEvent event)
	{
		(void)key;
		(void)event;

		if (g_visible)
		{
			PersistentIdFixUI::Hide();
		}
		else
		{
			PersistentIdFixUI::Show();
		}
	}

	bool RegisterUIToggleKeybind()
	{
		if (g_keybindRegistered)
			return true;

		if (g_self == nullptr ||
			g_self->hooks == nullptr ||
			g_self->hooks->Input == nullptr)
		{
			LOG_ERROR(
				"PersistentIdFix: UI toggle key registration failed: "
				"input interface is unavailable");

			return false;
		}

		const char* toggleKey =
			PersistentIdFixConfig::GetUIToggleKey();

		if (toggleKey == nullptr ||
			toggleKey[0] == '\0')
		{
			LOG_ERROR(
				"PersistentIdFix: UI toggle key registration failed: "
				"configured key is empty");

			return false;
		}

		g_self->hooks->Input->RegisterKeybindByName(
			toggleKey,
			EModKeyEvent::Pressed,
			&OnUIToggleKeyPressed);

		g_keybindRegistered = true;

		return true;
	}

	void UnregisterUIToggleKeybind()
	{
		if (!g_keybindRegistered)
			return;

		if (g_self != nullptr &&
			g_self->hooks != nullptr &&
			g_self->hooks->Input != nullptr)
		{
			const char* toggleKey =
				PersistentIdFixConfig::GetUIToggleKey();

			if (toggleKey != nullptr &&
				toggleKey[0] != '\0')
			{
				g_self->hooks->Input->UnregisterKeybindByName(
					toggleKey,
					EModKeyEvent::Pressed,
					&OnUIToggleKeyPressed);
			}
		}

		g_keybindRegistered = false;
	}
}

namespace PersistentIdFixUI
{
	bool Initialize(IPluginSelf* self)
	{
		if (self == nullptr ||
			self->hooks == nullptr ||
			self->hooks->UI == nullptr)
		{
			return false;
		}

		if (g_widget != nullptr)
			return true;

		g_visible = false;
		g_lateAttachmentWarningPending = false;
		g_lateAttachmentPopupOpened = false;

		UpdateHints();

		static PluginWidgetDesc widgetDesc = {
			"PersistentIdFix Live Statistics",
			&Render,
			&g_hints
		};

		g_widget =
			self->hooks->UI->RegisterWidget(
				&widgetDesc);

		if (g_widget == nullptr)
		{
			LOG_ERROR(
				"PersistentIdFix: failed to register statistics widget");

			return false;
		}

		self->hooks->UI->SetWidgetVisible(
			g_widget,
			false);

		if (!RegisterUIToggleKeybind())
		{
			self->hooks->UI->UnregisterWidget(
				g_widget);

			g_widget = nullptr;

			return false;
		}

		LOG_INFO(
			"PersistentIdFix: statistics widget initialized");

		return true;
	}

	void Shutdown()
	{
		UnregisterUIToggleKeybind();

		if (g_self != nullptr &&
			g_self->hooks != nullptr &&
			g_self->hooks->UI != nullptr &&
			g_widget != nullptr)
		{
			g_self->hooks->UI->SetWidgetVisible(
				g_widget,
				false);

			g_self->hooks->UI->UnregisterWidget(
				g_widget);
		}

		g_widget = nullptr;
		g_visible = false;
		g_lateAttachmentWarningPending = false;
		g_lateAttachmentPopupOpened = false;
		g_hints = {};
	}

	void Show()
	{
		if (g_self == nullptr ||
			g_self->hooks == nullptr ||
			g_self->hooks->NetMode == nullptr ||
			g_self->hooks->NetMode->GetNetMode == nullptr)
		{
			LOG_WARN(
				"PersistentIdFix: Show() ignored: "
				"network mode interface is unavailable");

			return;
		}

		const EPluginNetMode netMode =
			g_self->hooks->NetMode->GetNetMode();

		/*
		 * Local sessions own the PersistentIdFix statistics subsystem and
		 * therefore require an active local game session.
		 *
		 * Remote clients do not have a local PersistentIdFix game session.
		 * Their UI is backed entirely by the remote network session instead.
		 */
		const bool localSessionActive =
			PersistentIdFixSystem::IsSessionActive();

		const bool remoteSessionActive =
			netMode == EPluginNetMode::Client &&
			PersistentIdFixNetwork::IsSessionActive();

		if (!localSessionActive &&
			!remoteSessionActive)
		{
			LOG_INFO(
				"PersistentIdFix: Show() ignored: "
				"no active game session");

			g_visible = false;

			PersistentIdFixNetwork::SetUIVisible(false);

			return;
		}

		g_visible = true;

		/*
		 * Notify the network layer. For a remote client this sends the
		 * statistics subscription to the remote authority. For local
		 * sessions the network layer ignores the notification.
		 */
		PersistentIdFixNetwork::SetUIVisible(true);

		if (g_self->hooks->UI == nullptr ||
			g_widget == nullptr)
		{
			LOG_ERROR(
				"PersistentIdFix: Show() cannot display widget: "
				"UI interface or widget is unavailable");

			return;
		}

		g_self->hooks->UI->SetWidgetVisible(
			g_widget,
			true);

	}



	void Hide()
	{
		g_visible = false;

		/*
		 * This sends the client-side unsubscribe packet when this is a
		 * remote multiplayer session.
		 */
		PersistentIdFixNetwork::SetUIVisible(false);

		if (g_self != nullptr &&
			g_self->hooks != nullptr &&
			g_self->hooks->UI != nullptr &&
			g_widget != nullptr)
		{
			g_self->hooks->UI->SetWidgetVisible(
				g_widget,
				false);

		}
		else
		{
			LOG_ERROR(
				"PersistentIdFix: Hide() cannot hide widget: "
				"UI interface or widget is unavailable");
		}
	}

	void ShowLateAttachmentWarning()
	{
		if (g_self == nullptr ||
			g_self->hooks == nullptr ||
			g_self->hooks->UI == nullptr ||
			g_widget == nullptr)
		{
			LOG_ERROR(
				"PersistentIdFix: cannot display late-attachment ImGui warning: UI is unavailable");
			return;
		}

		g_lateAttachmentWarningPending = true;
		g_lateAttachmentPopupOpened = false;

		// The render callback must run even when the statistics
		// window itself is not logically visible.
		g_self->hooks->UI->SetWidgetVisible(
			g_widget,
			true);

		LOG_WARN(
			"PersistentIdFix: queued in-game late-attachment warning");
	}

	bool IsVisible()
	{
		return g_visible;
	}
}

#endif // MODLOADER_CLIENT_BUILD


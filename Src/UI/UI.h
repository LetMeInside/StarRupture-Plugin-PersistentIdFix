#ifdef MODLOADER_CLIENT_BUILD

#pragma once

#include <cstdint>

struct IPluginSelf;

namespace PersistentIdFixUI
{
    bool Initialize(IPluginSelf* self);

    void Shutdown();

    void Show();

    void Hide();

    // Displays a one-shot in-game modal. This is deliberately
    // rendered through ImGui so fullscreen users can see it.
    void ShowLateAttachmentWarning();

    bool IsVisible();
}

#endif // MODLOADER_CLIENT_BUILD

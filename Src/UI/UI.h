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

    bool IsVisible();
}

#endif // MODLOADER_CLIENT_BUILD

#include "Config.h"

#include "plugin_helpers.h"

#ifdef MODLOADER_CLIENT_BUILD
#include <cstring>
#endif

namespace
{
    static const ConfigEntry CONFIG_ENTRIES[] =
    {
        {
            "Statistics",
            "LogIntervalMinutes",
            ConfigValueType::Integer,
            "10",
            "Interval in minutes between statistics log entries",
            1,
            60
        }
#ifdef MODLOADER_CLIENT_BUILD
        , {
            "UI",
            "UIToggleKey",
            ConfigValueType::Keybind,
            "F3",
            "Keyboard key used to toggle the statistics UI"
        }
#endif
    };

    static const ConfigSchema SCHEMA =
    {
        CONFIG_ENTRIES,
        sizeof(CONFIG_ENTRIES) / sizeof(CONFIG_ENTRIES[0])
    };

    int g_logIntervalMinutes =
        PersistentIdFixConfig::DefaultLogIntervalMinutes;

#ifdef MODLOADER_CLIENT_BUILD
    char g_uiToggleKey[64] = "F3";
#endif
}

namespace PersistentIdFixConfig
{
    bool Initialize(IPluginSelf* self)
    {
        if (self == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: config initialization failed: self is null");
            return false;
        }

        if (self->config == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: config initialization failed: config interface is null");
            return false;
        }

        if (!self->config->InitializeFromSchema(self, &SCHEMA))
        {
            LOG_ERROR(
                "PersistentIdFix: config schema initialization failed");
            return false;
        }

        self->config->ValidateConfig(self, &SCHEMA);

        g_logIntervalMinutes =
            self->config->ReadInt(
                self,
                "Statistics",
                "LogIntervalMinutes",
                DefaultLogIntervalMinutes);

        if (g_logIntervalMinutes < 1)
        {
            LOG_WARN(
                "PersistentIdFix: LogIntervalMinutes=%d is invalid; using default %d",
                g_logIntervalMinutes,
                DefaultLogIntervalMinutes);

            g_logIntervalMinutes = DefaultLogIntervalMinutes;
        }

#ifdef MODLOADER_CLIENT_BUILD
        self->config->ReadString(
            self,
            "UI",
            "UIToggleKey",
            g_uiToggleKey,
            sizeof(g_uiToggleKey),
            DefaultUIToggleKey);

        if (g_uiToggleKey[0] == '\0')
        {
            LOG_WARN(
                "PersistentIdFix: UIToggleKey is empty; using default %s",
                DefaultUIToggleKey);

            strcpy_s(
                g_uiToggleKey,
                sizeof(g_uiToggleKey),
                DefaultUIToggleKey);
        }
#endif

        LOG_INFO(
            "PersistentIdFix: configuration initialized");

        LOG_INFO(
            "PersistentIdFix: LogIntervalMinutes = %d",
            g_logIntervalMinutes);

#ifdef MODLOADER_CLIENT_BUILD
        LOG_INFO(
            "PersistentIdFix: UIToggleKey = %s",
            g_uiToggleKey);
#endif

        return true;
    }

    int GetLogIntervalMinutes()
    {
        return g_logIntervalMinutes;
    }

#ifdef MODLOADER_CLIENT_BUILD
    const char* GetUIToggleKey()
    {
        return g_uiToggleKey;
    }
#endif
}

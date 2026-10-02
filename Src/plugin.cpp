#include "plugin.h"
#include "plugin_helpers.h"
#include "PersistentId/PersistentIdSystem.h"
#include "Config/Config.h"
#include "Network/Network.h"
#include "Native/Fingerprints.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/UI.h"
#endif

#include <cstdint>
#include <chrono>

IPluginSelf* g_self = nullptr;
IPluginSelf* GetSelf() { return g_self; }

static HookHandle g_getOrAddIDForHandleHook = nullptr;
static GetOrAddIDForHandleFn g_originalGetOrAddIDForHandle = nullptr; // Not actively used, but is an SDK requirement.

static uintptr_t g_getOrAddIDForHandleAddress = 0;

static uintptr_t g_setIDHandlePairAddress = 0;

static bool g_networkSessionActive = false;
static bool g_gameWorldActive = false;

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.2.0"
#endif

#ifdef MODLOADER_SERVER_BUILD
#define PERSISTENT_ID_FIX_TARGET PLUGIN_TARGET_SERVER
#else
#define PERSISTENT_ID_FIX_TARGET PLUGIN_TARGET_CLIENT
#endif

static PluginInfo s_pluginInfo = {
    "PersistentIdFix",
    MODLOADER_BUILD_TAG,
    "Kian369",
    "Reuses unused persistent IDs from the loaded save to prevent persistent ID exhaustion",
    PLUGIN_INTERFACE_VERSION,
    PERSISTENT_ID_FIX_TARGET
};

static void OnTick(
    float delta)
{
    (void)delta;

    if (g_self == nullptr ||
        g_self->hooks == nullptr ||
        g_self->hooks->NetMode == nullptr ||
        g_self->hooks->NetMode->GetNetMode == nullptr)
    {
        return;
    }

    const EPluginNetMode netMode =
        g_self->hooks->NetMode->GetNetMode();

    /*
     * -----------------------------------------------------------------------
     * Network session lifecycle.
     *
     * Network startup is tied to the actual game-world lifecycle rather than
     * merely to NetMode.
     *
     * This prevents preliminary sessions from being created in worlds such
     * as Map_MainMenu or DedicatedServerStart.
     *
     * On a remote client, OnWorldBeginPlay can occur while NetMode is still
     * Unknown. Once NetMode becomes Client, this block starts the network
     * session.
     *
     * g_networkSessionActive is the per-world guard. It is reset by
     * OnAfterWorldEndPlay().
     * -----------------------------------------------------------------------
     */
    if (g_gameWorldActive &&
        !g_networkSessionActive &&
        (netMode == EPluginNetMode::Client ||
            netMode == EPluginNetMode::ListenServer ||
            netMode == EPluginNetMode::DedicatedServer))
    {
        if (PersistentIdFixNetwork::BeginSession())
        {
            g_networkSessionActive = true;

            LOG_INFO(
                "PersistentIdFix: network session started");
        }
        else
        {
            LOG_WARN(
                "PersistentIdFix: network session could not be started");
        }
    }

    if (netMode == EPluginNetMode::Unknown)
        return;

    PersistentIdFixSystem::Tick();
}

static void OnWorldBeginPlay(SDK::UWorld* world)
{
    (void)world;

    if (g_self == nullptr ||
        g_self->hooks == nullptr ||
        g_self->hooks->NetMode == nullptr ||
        g_self->hooks->NetMode->GetNetMode == nullptr)
    {
        return;
    }

    LOG_INFO(
        "PersistentIdFix: OnWorldBeginPlay called");

    const EPluginNetMode netMode =
        g_self->hooks->NetMode->GetNetMode();

    LOG_INFO(
        "PersistentIdFix: OnWorldBeginPlay NetMode = %u",
        static_cast<unsigned int>(netMode));

    /*
     * OnWorldBeginPlay marks the beginning of the actual game world.
     *
     * This is intentionally independent of NetMode. On a remote client
     * StarRupture may still report Unknown here, but the world is already
     * active. OnTick() will start the network session later once the runtime
     * network state becomes usable.
     */
    g_gameWorldActive = true;

    /*
     * Server/listen-server sessions normally have their final NetMode
     * available already at WorldBeginPlay, so start networking immediately.
     *
     * A remote client may still report Unknown and is therefore deliberately
     * left for OnTick().
     */
    const bool networkSessionShouldBeActive =
        (netMode == EPluginNetMode::Client ||
            netMode == EPluginNetMode::ListenServer ||
            netMode == EPluginNetMode::DedicatedServer);

    if (!networkSessionShouldBeActive)
    {
        LOG_INFO(
            "PersistentIdFix: network session deferred until runtime NetMode becomes available");

        return;
    }

    if (g_networkSessionActive)
    {
        LOG_INFO(
            "PersistentIdFix: network session already active");

        return;
    }

    if (PersistentIdFixNetwork::BeginSession())
    {
        g_networkSessionActive = true;

        LOG_INFO(
            "PersistentIdFix: network session started");
    }
    else
    {
        LOG_WARN(
            "PersistentIdFix: network session could not be started");
    }
}

// ---------------------------------------------------------------------------
// World-end callback.
//
// The persistent ID subsystem belongs to the current world/save. Clear our
// bookkeeping when the world ends so we never retain a pointer to a subsystem
// that has already been destroyed.
//
// Do not dereference 'world' here. The callback is only used as a lifecycle
// boundary for our own state.
// ---------------------------------------------------------------------------

static void OnAfterWorldEndPlay(
    SDK::UWorld* world,
    const char* worldName)
{
    LOG_INFO(
        "PersistentIdFix: world ended: %s",
        worldName != nullptr ? worldName : "<null>");

    /*
     * The actual game world has ended. Prevent OnTick() from starting a new
     * network session while the process is transitioning through a menu or
     * another intermediate world.
     */
    g_gameWorldActive = false;

    PersistentIdFixSystem::EndSession();

    /*
     * End the network session independently of the local persistent-ID
     * session.
     *
     * This is required for remote clients because they do not necessarily
     * create a local PersistentIdFix persistent-ID session.
     */
    if (g_networkSessionActive)
    {
        PersistentIdFixNetwork::ResetSession();
        g_networkSessionActive = false;
    }

#ifdef MODLOADER_CLIENT_BUILD
    /*
     * The UI belongs to the game world, not to g_gameSessionActive.
     *
     * A remote client can have a visible statistics window without ever
     * setting g_gameSessionActive because the server owns the persistent-ID
     * subsystem. Therefore the UI must also be hidden when the game world
     * ends on a remote client.
     *
     * PersistentIdFixNetwork::ResetSession() has already happened above, so
     * Hide() will not attempt to send a subscription packet for the ended
     * session.
     */
    if (PersistentIdFixUI::IsVisible())
    {
        PersistentIdFixUI::Hide();
    }
#endif
}

// ---------------------------------------------------------------------------
// Pattern resolution.
//
// This is the ONLY place where the scanner is used. The returned addresses
// remain valid after this callback; the scanner table itself does not.
//
// The actual fingerprint scanning and structural validation live in
// PersistentIdFixFingerprints::Resolve().
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport)
void OnPluginLoadHooks(
    IPluginSelf* self,
    IPluginHookScanner* scanner)
{
    if (self == nullptr || scanner == nullptr)
        return;

    PersistentIdFixFingerprints::ResolvedAddresses addresses;

    if (!PersistentIdFixFingerprints::Resolve(
        self,
        scanner,
        addresses))
    {
        return;
    }

    g_getOrAddIDForHandleAddress =
        addresses.getOrAddIDForHandle;

    g_setIDHandlePairAddress =
        addresses.setIDHandlePair;
}

// ---------------------------------------------------------------------------
// Plugin lifecycle.
// ---------------------------------------------------------------------------

extern "C"
{

    __declspec(dllexport)
        PluginInfo* GetPluginInfo()
    {
        return &s_pluginInfo;
    }

    __declspec(dllexport)
        bool PluginInit(IPluginSelf* self)
    {
        g_self = self;

        if (g_self == nullptr || g_self->hooks == nullptr)
        {
            g_self = nullptr;
            return false;
        }

        bool bInitConfig = false;
        bool bInitNativeHook = false;
        bool bInitWorldCallbacks = false;
        bool bInitNetwork = false;

#ifdef MODLOADER_CLIENT_BUILD
        bool bInitUI = false;
#endif

        bool bInitEngineTick = false;

        while (true)
        {
            //
            // Configuration
            //
            if (!PersistentIdFixConfig::Initialize(
                g_self))
            {
                LOG_ERROR(
                    "PersistentIdFix: configuration initialization failed");
                break;
            }

            bInitConfig = true;

            //
            // Resolve and validate native function addresses
            //
            if (g_getOrAddIDForHandleAddress == 0)
            {
                LOG_ERROR(
                    "PersistentIdFix: GetOrAddIDForHandle address was not resolved");
                break;
            }

            if (g_setIDHandlePairAddress == 0)
            {
                LOG_ERROR(
                    "PersistentIdFix: SetIDHandlePair address was not resolved");
                break;
            }

            if (g_self->hooks->Hooks == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: native hook interface is unavailable");
                break;
            }

            LOG_INFO(
                "PersistentIdFix: GetOrAddIDForHandle resolved at %p",
                reinterpret_cast<void*>(g_getOrAddIDForHandleAddress));

            LOG_INFO(
                "PersistentIdFix: SetIDHandlePair resolved at %p",
                reinterpret_cast<void*>(g_setIDHandlePairAddress));

            //
            // Install GetOrAddIDForHandle hook
            //
            g_getOrAddIDForHandleHook =
                g_self->hooks->Hooks->Install(
                    g_getOrAddIDForHandleAddress,
                    reinterpret_cast<void*>(&PersistentIdFixSystem::GetOrAddIDForHandleDetour),
                    reinterpret_cast<void**>(
                        &g_originalGetOrAddIDForHandle));

            if (g_getOrAddIDForHandleHook == nullptr ||
                g_originalGetOrAddIDForHandle == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: failed to install GetOrAddIDForHandle hook");

                g_getOrAddIDForHandleHook = nullptr;
                g_originalGetOrAddIDForHandle = nullptr;

                break;
            }

            bInitNativeHook = true;

            LOG_INFO(
                "PersistentIdFix: GetOrAddIDForHandle hook installed");

            PersistentIdFixSystem::Configure(
                g_self,
                reinterpret_cast<SetIDHandlePairFn>(
                    g_setIDHandlePairAddress),
                g_originalGetOrAddIDForHandle);

            //
            // World callbacks
            //
            if (g_self->hooks->World == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: World hooks are unavailable");
                break;
            }

            g_self->hooks->World->RegisterOnWorldBeginPlay(
                &OnWorldBeginPlay);

            LOG_INFO(
                "PersistentIdFix: registered OnWorldBeginPlay callback");

            g_self->hooks->World->RegisterOnSaveLoaded(
                &PersistentIdFixSystem::OnSaveLoaded);

            LOG_INFO(
                "PersistentIdFix: registered OnSaveLoaded callback");

            g_self->hooks->World->RegisterOnAfterWorldEndPlay(
                &OnAfterWorldEndPlay);

            LOG_INFO(
                "PersistentIdFix: registered OnAfterWorldEndPlay callback");

            bInitWorldCallbacks = true;

            //
            // Network communication.
            //
            // This only initializes the network communication layer.
            // BeginSession() is deliberately deferred to OnTick(), where
            // the runtime NetMode is available.
            //
            if (!PersistentIdFixNetwork::Initialize(g_self))
            {
                LOG_WARN(
                    "PersistentIdFix: network communication initialization failed");
            }
            else
            {
                bInitNetwork = true;
            }

            g_networkSessionActive = false;
            g_gameWorldActive = false;

#ifdef MODLOADER_CLIENT_BUILD

            //
            // Client UI
            //
            if (!PersistentIdFixUI::Initialize(
                g_self))
            {
                LOG_ERROR(
                    "PersistentIdFix: failed to initialize statistics UI");
                break;
            }

            bInitUI = true;

#endif

            //
            // Engine tick callback
            //
            if (g_self->hooks->Engine == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: Engine hooks are unavailable");
                break;
            }

            g_self->hooks->Engine->RegisterOnTick(
                &OnTick);

            LOG_INFO(
                "PersistentIdFix: registered OnTick callback");

            bInitEngineTick = true;

            LOG_INFO(
                "PersistentIdFix: initialization complete");

            return true;
        }

        //
        // Initialization failed.
        //
        // Unwind everything that was successfully initialized,
        // in reverse initialization order.
        //

        if (bInitEngineTick)
        {
            //
            // There is currently no Engine::UnregisterOnTick() call
            // in the existing code, so there is nothing to unwind here.
            // Keep the flag because the initialization step is still
            // explicitly tracked and can be made reversible later.
            //
        }

#ifdef MODLOADER_CLIENT_BUILD

        if (bInitUI)
        {
            PersistentIdFixUI::Shutdown();
        }

#endif

        if (bInitNetwork)
        {
            PersistentIdFixNetwork::Shutdown();
        }

        if (bInitWorldCallbacks)
        {
            if (g_self->hooks->World != nullptr)
            {
                g_self->hooks->World->UnregisterOnWorldBeginPlay(
                    &OnWorldBeginPlay);

                g_self->hooks->World->UnregisterOnSaveLoaded(
                    &PersistentIdFixSystem::OnSaveLoaded);

                g_self->hooks->World->UnregisterOnAfterWorldEndPlay(
                    &OnAfterWorldEndPlay);
            }
        }

        if (bInitNativeHook)
        {
            if (g_self->hooks->Hooks != nullptr &&
                g_getOrAddIDForHandleHook != nullptr)
            {
                g_self->hooks->Hooks->Remove(
                    g_getOrAddIDForHandleHook);
            }

            g_getOrAddIDForHandleHook = nullptr;
            g_originalGetOrAddIDForHandle = nullptr;
        }

        if (bInitConfig)
        {
            //
            // PersistentIdFixConfig currently has no Shutdown()
            // operation, so there is nothing to unwind here.
            //
            // Keep bInitConfig because the initialization step is
            // explicitly tracked and can be made reversible later.
            //
        }

        g_networkSessionActive = false;
        g_gameWorldActive = false;

        PersistentIdFixSystem::Reset();

        g_self = nullptr;

        return false;
    }


    extern "C"
    {

        __declspec(dllexport)
            void PluginShutdown()
        {
            LOG_INFO(
                "PersistentIdFix: shutting down");

            PersistentIdFixNetwork::Shutdown();
            g_networkSessionActive = false;
            g_gameWorldActive = false;

#ifdef MODLOADER_CLIENT_BUILD
            PersistentIdFixUI::Shutdown();
#endif

            if (g_self != nullptr &&
                g_self->hooks != nullptr)
            {
                if (g_self->hooks->Engine != nullptr)
                {
                    g_self->hooks->Engine->UnregisterOnTick(
                        &OnTick);
                }

                if (g_self->hooks->World != nullptr)
                {
                    g_self->hooks->World->UnregisterOnWorldBeginPlay(
                        &OnWorldBeginPlay);

                    g_self->hooks->World->UnregisterOnSaveLoaded(
                        &PersistentIdFixSystem::OnSaveLoaded);

                    g_self->hooks->World->UnregisterOnAfterWorldEndPlay(
                        &OnAfterWorldEndPlay);
                }

                if (g_getOrAddIDForHandleHook != nullptr &&
                    g_self->hooks->Hooks != nullptr)
                {
                    g_self->hooks->Hooks->Remove(
                        g_getOrAddIDForHandleHook);
                }
            }

            PersistentIdFixSystem::Reset();
            g_gameWorldActive = false;

            g_getOrAddIDForHandleHook = nullptr;
            g_originalGetOrAddIDForHandle = nullptr;

            g_getOrAddIDForHandleAddress = 0;
            g_setIDHandlePairAddress = 0;

            g_self = nullptr;
        }

    }

}
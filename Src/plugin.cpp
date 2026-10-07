#include "plugin.h"
#include "plugin_helpers.h"
#include "PersistentId/PersistentIdSystem.h"
#include "Config/Config.h"
#include "Network/Network.h"
#include "Native/Fingerprints.h"
#include "SourceData/SourceReadObserver.h"
#include "SourceData/MassFragmentClassifier.h"
#include "SourceData/IndependentSourceCollector.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/UI.h"
#endif

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

IPluginSelf* g_self = nullptr;
IPluginSelf* GetSelf() { return g_self; }

static HookHandle g_getOrAddIDForHandleHook = nullptr;
static HookHandle g_getSaveDataHook = nullptr;
static HookHandle g_onPreLoadMapHook = nullptr;

// SDK hook output; passed to PersistentIdFixSystem for native delegation.
static GetOrAddIDForHandleFn g_originalGetOrAddIDForHandle = nullptr;

// Native source-read observer trampoline. Step 2 is intentionally passive:
// it forwards the call unchanged and does not inspect or retain source data.
static GetSaveDataFn g_originalGetSaveData = nullptr;
static OnPreLoadMapFn g_originalOnPreLoadMap = nullptr;

static uintptr_t g_getOrAddIDForHandleAddress = 0;
static uintptr_t g_getSaveDataAddress = 0;
static uintptr_t g_onPreLoadMapAddress = 0;

static uintptr_t g_setIDHandlePairAddress = 0;

static bool g_authoritativeWorldReadyObserved = false;

static void ResetProtectedIdPublication();

static bool GetSaveDataDetour(
    void* saveSubsystem,
    void* sectionName,
    const void* structType,
    void* destination)
{
    return PersistentIdFixSourceReadObserver::ObserveGetSaveData(
        g_originalGetSaveData,
        saveSubsystem,
        sectionName,
        structType,
        destination);
}

static void OnPreLoadMapDetour(
    void* saveSubsystem,
    const void* mapName)
{
    // Reset before native OnPreLoadMap broadcasts OnPreSaveLoaded.
    // Any GetSaveData calls made by that delegate therefore belong to
    // the fresh generation rather than inheriting prior-world state.
    g_authoritativeWorldReadyObserved = false;
    ResetProtectedIdPublication();

    PersistentIdFixSourceReadObserver::BeginLoadGeneration(
        saveSubsystem);

    const auto coverage =
        PersistentIdFixSourceReadObserver::GetCoverageSnapshot();

    PersistentIdFixSystem::BeginLoadGeneration(
        coverage.loadGeneration);

    if (g_originalOnPreLoadMap != nullptr)
    {
        g_originalOnPreLoadMap(
            saveSubsystem,
            mapName);
    }
}

static bool g_networkSessionActive = false;
static bool g_gameWorldActive = false;

// G1 publication is diagnostic only. The allocator does not read this
// vector yet.
static std::vector<std::uint32_t> g_publishedProtectedIds;
static std::uint64_t g_publishedProtectedGeneration = 0;

enum class AuthorityReadiness : std::uint8_t
{
    Unresolved = 0,
    Authoritative,
    RemoteClient
};

static const char* NetModeName(EPluginNetMode netMode)
{
    switch (netMode)
    {
    case EPluginNetMode::Standalone:
        return "Standalone";
    case EPluginNetMode::ListenServer:
        return "ListenServer";
    case EPluginNetMode::DedicatedServer:
        return "DedicatedServer";
    case EPluginNetMode::Client:
        return "Client";
    default:
        return "Unknown";
    }
}

static AuthorityReadiness GetAuthorityReadiness(
    EPluginNetMode netMode)
{
    switch (netMode)
    {
    case EPluginNetMode::Standalone:
    case EPluginNetMode::ListenServer:
    case EPluginNetMode::DedicatedServer:
        return AuthorityReadiness::Authoritative;

    case EPluginNetMode::Client:
        return AuthorityReadiness::RemoteClient;

    default:
        return AuthorityReadiness::Unresolved;
    }
}

static const char* AuthorityReadinessName(
    AuthorityReadiness readiness)
{
    switch (readiness)
    {
    case AuthorityReadiness::Authoritative:
        return "Authoritative";
    case AuthorityReadiness::RemoteClient:
        return "RemoteClient";
    default:
        return "Unresolved";
    }
}

static bool IsAuthoritativeWorldReady()
{
    const auto coverage =
        PersistentIdFixSourceReadObserver::GetCoverageSnapshot();

    const EPluginNetMode netMode =
        PersistentIdFixSystem::GetSessionNetMode();

    return
        coverage.sourceCoverageComplete &&
        coverage.coverageAttachedToCurrentGeneration &&
        GetAuthorityReadiness(netMode) ==
            AuthorityReadiness::Authoritative;
}

static bool PublishProtectedIds()
{
    const auto coverage =
        PersistentIdFixSourceReadObserver::GetCoverageSnapshot();

    if (!IsAuthoritativeWorldReady() ||
        coverage.loadGeneration == 0)
    {
        return false;
    }

    if (!PersistentIdFixSourceReadObserver::BeginPublicationFreeze())
    {
        LOG_ERROR(
            "PersistentIdFix: protected-ID publication failed to acquire source freeze");
        return false;
    }

    struct PublicationFreezeRelease
    {
        ~PublicationFreezeRelease()
        {
            PersistentIdFixSourceReadObserver::EndPublicationFreeze();
        }
    } publicationFreezeRelease;

    const auto activityBefore =
        PersistentIdFixSourceReadObserver::GetObserverActivitySnapshot();

    if (activityBefore.activeFrames != 0)
    {
        LOG_ERROR(
            "PersistentIdFix: protected-ID publication acquired source freeze but active observer frames=%llu",
            static_cast<unsigned long long>(activityBefore.activeFrames));
        return false;
    }

    std::vector<std::uint32_t> staged;

    if (!PersistentIdFixMassFragmentClassifier::AppendCollectedValues(staged) ||
        !PersistentIdFixIndependentSourceCollector::AppendCollectedValues(staged))
    {
        LOG_ERROR(
            "PersistentIdFix: protected-ID publication failed while copying collected source values");
        return false;
    }

    const auto activityAfter =
        PersistentIdFixSourceReadObserver::GetObserverActivitySnapshot();

    if (activityAfter.activeFrames != 0 ||
        activityAfter.frameTransitions != activityBefore.frameTransitions)
    {
        LOG_ERROR(
            "PersistentIdFix: protected-ID publication source freeze coherence failure");
        return false;
    }

    const std::size_t occurrences = staged.size();

    std::sort(staged.begin(), staged.end());
    staged.erase(
        std::unique(staged.begin(), staged.end()),
        staged.end());

    const std::uint32_t sentinel =
        (std::numeric_limits<std::uint32_t>::max)();

    const bool hasZero =
        std::binary_search(staged.begin(), staged.end(), 0u);
    const bool hasSentinel =
        std::binary_search(staged.begin(), staged.end(), sentinel);

    const std::size_t ordinaryUnique =
        staged.size() -
        (hasZero ? 1u : 0u) -
        (hasSentinel ? 1u : 0u);

    try
    {
        g_publishedProtectedIds = std::move(staged);
        g_publishedProtectedGeneration = coverage.loadGeneration;
    }
    catch (...)
    {
        g_publishedProtectedIds.clear();
        g_publishedProtectedGeneration = 0;
        LOG_ERROR(
            "PersistentIdFix: protected-ID publication failed while committing staged values");
        return false;
    }

    LOG_INFO(
        "PersistentIdFix: protected-ID publication: generation=%llu occurrences=%llu unique=%llu ordinaryUnique=%llu zeroPresent=%u sentinelPresent=%u observerTransitions=%llu",
        static_cast<unsigned long long>(g_publishedProtectedGeneration),
        static_cast<unsigned long long>(occurrences),
        static_cast<unsigned long long>(g_publishedProtectedIds.size()),
        static_cast<unsigned long long>(ordinaryUnique),
        hasZero ? 1u : 0u,
        hasSentinel ? 1u : 0u,
        static_cast<unsigned long long>(activityBefore.frameTransitions));

    return true;
}

static void ResetProtectedIdPublication()
{
    g_publishedProtectedIds.clear();
    g_publishedProtectedGeneration = 0;
}

static void LogReadiness(
    const char* phase)
{
    const auto coverage =
        PersistentIdFixSourceReadObserver::GetCoverageSnapshot();

    const EPluginNetMode netMode =
        PersistentIdFixSystem::GetSessionNetMode();

    const AuthorityReadiness authority =
        GetAuthorityReadiness(netMode);

    const bool authoritativeWorldReady =
        IsAuthoritativeWorldReady();

    LOG_INFO(
        "PersistentIdFix: readiness [%s]: netMode=%s authority=%s sourceCoverage=%u currentGeneration=%u authoritativeWorldReady=%u",
        phase != nullptr ? phase : "<null>",
        NetModeName(netMode),
        AuthorityReadinessName(authority),
        coverage.sourceCoverageComplete ? 1u : 0u,
        coverage.coverageAttachedToCurrentGeneration ? 1u : 0u,
        authoritativeWorldReady ? 1u : 0u);
}

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "1.2.0"
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

    if (g_gameWorldActive)
    {
        const EPluginNetMode before =
            PersistentIdFixSystem::GetSessionNetMode();

        PersistentIdFixSystem::SetSessionNetMode(netMode);

        const EPluginNetMode after =
            PersistentIdFixSystem::GetSessionNetMode();

        if (before == EPluginNetMode::Unknown &&
            after != EPluginNetMode::Unknown)
        {
            LogReadiness("role-settled");
        }
    }

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

    if (g_gameWorldActive &&
        !g_authoritativeWorldReadyObserved &&
        IsAuthoritativeWorldReady())
    {
        g_authoritativeWorldReadyObserved = true;

        LOG_INFO(
            "PersistentIdFix: authoritative world readiness became true during active gameplay");

        LogReadiness("ready-transition");

        if (!PublishProtectedIds())
        {
            LOG_WARN(
                "PersistentIdFix: protected-ID publication was not accepted for the ready generation");
        }
        else
        {
            const auto candidate =
                PersistentIdFixSystem::BuildCandidatePoolDiagnostic(
                    g_publishedProtectedGeneration,
                    g_publishedProtectedIds);

            if (!candidate.valid)
            {
                LOG_WARN(
                    "PersistentIdFix: certified candidate-pool diagnostic was not accepted");
            }
            else if (!PersistentIdFixSystem::ActivateCertifiedPool(
                         g_publishedProtectedGeneration))
            {
                LOG_WARN(
                    "PersistentIdFix: certified reusable pool activation was rejected");
            }
        }

        PersistentIdFixSystem::LogAssignmentLedger(
            "ready-transition");
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
    PersistentIdFixSystem::SetSessionNetMode(netMode);

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

    PersistentIdFixSourceReadObserver::AttachCurrentGenerationToGameWorld();

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

static void OnSaveLoaded()
{
    PersistentIdFixSystem::OnSaveLoaded();

    PersistentIdFixSystem::LogAssignmentLedger(
        "save-loaded");

    PersistentIdFixSourceReadObserver::LogCoverage(
        "save-loaded");

    LogReadiness(
        "save-loaded");
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

    PersistentIdFixSourceReadObserver::LogCoverage(
        "world-end");

    LogReadiness(
        "world-end");

    /*
     * Detach source coverage only when this callback is ending the
     * actual active game world. OnPreLoadMap can start an incoming
     * generation before an outgoing menu/intermediate world ends;
     * those callbacks must not invalidate the incoming generation.
     */
    if (g_gameWorldActive)
    {
        PersistentIdFixSourceReadObserver::DetachGameWorld();
    }

    /*
     * The actual game world has ended. Prevent OnTick() from starting a new
     * network session while the process is transitioning through a menu or
     * another intermediate world.
     */
    g_gameWorldActive = false;
    ResetProtectedIdPublication();

    PersistentIdFixSystem::LogAssignmentLedger(
        "world-end");

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
     * The UI belongs to the game world, not to the local persistent-ID session.
     *
     * A remote client can have a visible statistics window without ever
     * creating a local persistent-ID session because the server owns the
     * persistent-ID subsystem. Therefore the UI must also be hidden when the
     * game world ends on a remote client.
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

    g_getSaveDataAddress =
        addresses.getSaveData;

    g_onPreLoadMapAddress =
        addresses.onPreLoadMap;
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

            if (g_getSaveDataAddress == 0)
            {
                LOG_ERROR(
                    "PersistentIdFix: GetSaveData address was not resolved");
                break;
            }

            if (g_onPreLoadMapAddress == 0)
            {
                LOG_ERROR(
                    "PersistentIdFix: OnPreLoadMap address was not resolved");
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

            LOG_INFO(
                "PersistentIdFix: GetSaveData resolved at %p",
                reinterpret_cast<void*>(g_getSaveDataAddress));

            LOG_INFO(
                "PersistentIdFix: OnPreLoadMap resolved at %p",
                reinterpret_cast<void*>(g_onPreLoadMapAddress));

            //
            // Resolve and certify the finite Step 3C descriptor registry before
            // any GetSaveData callback can attempt Mass fragment traversal.
            //
            if (!PersistentIdFixMassFragmentClassifier::InitializeDescriptorRegistry(
                g_self->hooks->Engine))
            {
                LOG_WARN(
                    "PersistentIdFix: Mass descriptor registry unavailable; Step 3C classification remains fail-closed");
            }

            //
            // Install OnPreLoadMap generation-boundary hook.
            //
            // This remains observational: it only starts a fresh source
            // observation generation before native OnPreSaveLoaded runs.
            //
            g_onPreLoadMapHook =
                g_self->hooks->Hooks->Install(
                    g_onPreLoadMapAddress,
                    reinterpret_cast<void*>(&OnPreLoadMapDetour),
                    reinterpret_cast<void**>(
                        &g_originalOnPreLoadMap));

            if (g_onPreLoadMapHook == nullptr ||
                g_originalOnPreLoadMap == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: failed to install OnPreLoadMap hook");

                g_onPreLoadMapHook = nullptr;
                g_originalOnPreLoadMap = nullptr;

                break;
            }

            LOG_INFO(
                "PersistentIdFix: OnPreLoadMap generation hook installed");

            //
            // Install GetSaveData source-read observer hook.
            //
            // Step 2 deliberately forwards every call unchanged. Collection
            // and coverage tracking are introduced in later guarded changes.
            //
            g_getSaveDataHook =
                g_self->hooks->Hooks->Install(
                    g_getSaveDataAddress,
                    reinterpret_cast<void*>(&GetSaveDataDetour),
                    reinterpret_cast<void**>(
                        &g_originalGetSaveData));

            if (g_getSaveDataHook == nullptr ||
                g_originalGetSaveData == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: failed to install GetSaveData hook");

                g_getSaveDataHook = nullptr;
                g_originalGetSaveData = nullptr;

                break;
            }

            LOG_INFO(
                "PersistentIdFix: GetSaveData observer hook installed");

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
                &OnSaveLoaded);

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
            g_authoritativeWorldReadyObserved = false;
            ResetProtectedIdPublication();

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
                    &OnSaveLoaded);

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

        if (g_getSaveDataHook != nullptr)
        {
            if (g_self->hooks->Hooks != nullptr)
            {
                g_self->hooks->Hooks->Remove(
                    g_getSaveDataHook);
            }

            g_getSaveDataHook = nullptr;
            g_originalGetSaveData = nullptr;
        }

        if (g_onPreLoadMapHook != nullptr)
        {
            if (g_self->hooks->Hooks != nullptr)
            {
                g_self->hooks->Hooks->Remove(
                    g_onPreLoadMapHook);
            }

            g_onPreLoadMapHook = nullptr;
            g_originalOnPreLoadMap = nullptr;
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
        g_authoritativeWorldReadyObserved = false;
        ResetProtectedIdPublication();

        PersistentIdFixSourceReadObserver::Reset();
        PersistentIdFixMassFragmentClassifier::ResetDescriptorRegistry();
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
            g_authoritativeWorldReadyObserved = false;
            ResetProtectedIdPublication();

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
                        &OnSaveLoaded);

                    g_self->hooks->World->UnregisterOnAfterWorldEndPlay(
                        &OnAfterWorldEndPlay);
                }

                if (g_getOrAddIDForHandleHook != nullptr &&
                    g_self->hooks->Hooks != nullptr)
                {
                    g_self->hooks->Hooks->Remove(
                        g_getOrAddIDForHandleHook);
                }

                if (g_getSaveDataHook != nullptr &&
                    g_self->hooks->Hooks != nullptr)
                {
                    g_self->hooks->Hooks->Remove(
                        g_getSaveDataHook);
                }

                if (g_onPreLoadMapHook != nullptr &&
                    g_self->hooks->Hooks != nullptr)
                {
                    g_self->hooks->Hooks->Remove(
                        g_onPreLoadMapHook);
                }
            }

            PersistentIdFixSourceReadObserver::Reset();
            PersistentIdFixMassFragmentClassifier::ResetDescriptorRegistry();
            PersistentIdFixSystem::Reset();
            g_gameWorldActive = false;

            g_getOrAddIDForHandleHook = nullptr;
            g_originalGetOrAddIDForHandle = nullptr;

            g_getSaveDataHook = nullptr;
            g_originalGetSaveData = nullptr;

            g_onPreLoadMapHook = nullptr;
            g_originalOnPreLoadMap = nullptr;

            g_getOrAddIDForHandleAddress = 0;
            g_getSaveDataAddress = 0;
            g_onPreLoadMapAddress = 0;
            g_setIDHandlePairAddress = 0;

            g_self = nullptr;
        }

    }

}
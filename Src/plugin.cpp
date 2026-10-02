#include "plugin.h"
#include "plugin_helpers.h"
#include "PersistentIdReuse.h"
#include "Native/NativeIdLookup.h"
#include "Stats/Stats.h"
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

static PersistentIdReuse g_persistentIdReuse;
static SDK::UCrMassPersistentIDSubsystem* g_persistentIdSubsystem = nullptr;

static HookHandle g_getOrAddIDForHandleHook = nullptr;
static GetOrAddIDForHandleFn g_originalGetOrAddIDForHandle = nullptr; // Not actively used, but is an SDK requirement.
static SetIDHandlePairFn g_setIDHandlePair = nullptr;

static uintptr_t g_getOrAddIDForHandleAddress = 0;

static uintptr_t g_setIDHandlePairAddress = 0;

static bool g_reusePoolReady = false;
static bool g_gameSessionActive = false;

static std::chrono::steady_clock::time_point g_nextStatisticsLogTime{};
static std::chrono::steady_clock::time_point g_nextLiveStatisticsRefreshTime{};
static bool g_statisticsTimerActive = false;

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


// forward declaration:
static bool PersistentIdSystemInit();
static void OnNewGame();

// ---------------------------------------------------------------------------
// New-game initialisation.
//
// A new game seeds persistent IDs before OnWorldBeginPlay and does not
// produce an OnSaveLoaded callback. During that initial seeding phase the
// persistent-ID subsystem starts empty:
//
//     MaxID == 0
//     IDHandleMap.Num() == 0
//
// The first GetOrAddIDForHandle call therefore provides the earliest reliable
// indication that a new-game persistent-ID system is being populated.
// ---------------------------------------------------------------------------

static void OnNewGame()
{
    LOG_INFO(
        "PersistentIdFix: new game detected");

    if (!PersistentIdSystemInit())
    {
        LOG_ERROR(
            "PersistentIdFix: persistent ID system initialization failed for new game");
        return;
    }

    LOG_INFO(
        "PersistentIdFix: new-game persistent ID system initialized");
}
// ---------------------------------------------------------------------------
// GetOrAddIDForHandle detour.
//
// The native function returns FCrMassPersistentEntityID by value. The MSVC
// ABI passes the hidden return buffer as the second argument:
//
//   RCX = subsystem
//   RDX = return-value buffer
//   R8  = FMassEntityHandle
//
// Existing handles are resolved directly through the native HandleIDMap
// representation. They are not forwarded to the native implementation.
//
// Only genuinely new handles are allowed to consume an ID from our pool.
// If the pool is empty, the native allocation semantics are reproduced
// using MaxID and SetIDHandlePair.
// ---------------------------------------------------------------------------

static SDK::FCrMassPersistentEntityID* GetOrAddIDForHandleDetour(
    void* subsystem,
    SDK::FCrMassPersistentEntityID* returnValue,
    SDK::FMassEntityHandle handle)
{
    if (returnValue == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: GetOrAddIDForHandle return buffer is null");

        return nullptr;
    }

    *returnValue = {};

    if (subsystem == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: GetOrAddIDForHandle subsystem is null");

        return returnValue;
    }

    /*
     * Determine whether this process is a remote client.
     *
     * During actual client-world startup GetNetMode() can remain Unknown
     * for some time. In that state, IsServer() provides the additional
     * information needed to distinguish a remote client from a server.
     *
     * Do not treat Unknown by itself as Client: Solo and listen-server
     * startup can also pass through Unknown.
     */
    bool isRemoteClient = false;

    if (g_self != nullptr &&
        g_self->hooks != nullptr &&
        g_self->hooks->NetMode != nullptr)
    {
        if (g_self->hooks->NetMode->GetNetMode != nullptr)
        {
            const EPluginNetMode netMode =
                g_self->hooks->NetMode->GetNetMode();

            if (netMode == EPluginNetMode::Client)
            {
                isRemoteClient = true;
            }
            else if (netMode == EPluginNetMode::Unknown &&
                g_self->hooks->NetMode->IsServer != nullptr &&
                !g_self->hooks->NetMode->IsServer())
            {
                isRemoteClient = true;
            }
        }
    }

    /*
     * The native function remains authoritative for remote clients.
     *
     * A remote client does not own the persistent-ID state that
     * PersistentIdFix is intended to manage. In particular, it must not
     * trigger new-game detection or build a local reuse pool merely because
     * GetOrAddIDForHandle happens to execute on the client.
     */
    if (isRemoteClient)
    {
        if (g_originalGetOrAddIDForHandle == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: native GetOrAddIDForHandle is unavailable "
                "while running as a remote client");

            return returnValue;
        }

        return g_originalGetOrAddIDForHandle(
            subsystem,
            returnValue,
            handle);
    }

    auto* persistentIDSubsystem =
        static_cast<SDK::UCrMassPersistentIDSubsystem*>(subsystem);

    /*
     * First reproduce the native HandleIDMap lookup.
     *
     * This must happen before allocating anything.
     */
    const SDK::FCrMassPersistentEntityID* existingId =
        FindPersistentIdByHandle(
            persistentIDSubsystem,
            handle);

    if (existingId != nullptr)
    {
        *returnValue = *existingId;
        return returnValue;
    }

    /*
     * A new game begins with an empty persistent-ID subsystem. StarRupture
     * starts allocating persistent IDs before OnWorldBeginPlay and without
     * producing OnSaveLoaded, so the first cache miss while the subsystem is
     * completely empty is our new-game initialization point.
     *
     * The remote-client exclusion above deliberately runs first because
     * GetNetMode() may still be Unknown during client startup.
     */
    if (!g_reusePoolReady &&
        persistentIDSubsystem->MaxID == 0 &&
        persistentIDSubsystem->IDHandleMap.Num() == 0)
    {
        OnNewGame();
    }

    /*
     * On a local multiplayer session, the game can call
     * GetOrAddIDForHandle before OnSaveLoaded has built the PersistentIdFix
     * reuse pool. This was observed in the listen-server test:
     *
     *     MaxID=1256
     *     reuse pool not yet built
     *
     * During this initialization window the native allocator remains
     * authoritative. OnSaveLoaded will subsequently inspect the resulting
     * persistent-ID state and build the reuse pool from it.
     *
     * The same fallback also protects us if persistent-ID initialization
     * unexpectedly has not completed for another local reason.
     */
    if (!g_reusePoolReady)
    {
        LOG_INFO(
            "PersistentIdFix: GetOrAddIDForHandle before reuse pool was built; "
            "delegating to native allocator: handle=(%u,%u) MaxID=%u",
            handle.Index,
            handle.SerialNumber,
            persistentIDSubsystem->MaxID);

        if (g_originalGetOrAddIDForHandle == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: native GetOrAddIDForHandle is unavailable "
                "during persistent ID initialization");

            return returnValue;
        }

        return g_originalGetOrAddIDForHandle(
            subsystem,
            returnValue,
            handle);
    }

    /*
     * No existing mapping.
     *
     * First consume an ID hole that existed when the save was loaded.
     */
    switch (g_persistentIdReuse.TryAllocate(handle, *returnValue))
    {
    case PersistentIdAllocationResult::Allocated:
        PersistentIdFixStats::RecordAssignment();
        return returnValue;

    case PersistentIdAllocationResult::NoReusableId:
        break;

    case PersistentIdAllocationResult::Failed:
        // This is logged inside TryAllocate.
        return returnValue;
    }

    /*
     * No reusable IDs remain.
     *
     * Allocate exactly as the native function does.
     */
    if (persistentIDSubsystem->MaxID == UINT32_MAX)
    {
        LOG_ERROR(
            "PersistentIdFix: persistent ID space exhausted; "
            "no reusable IDs remain and MaxID is UINT32_MAX");

        return returnValue;
    }

    ++persistentIDSubsystem->MaxID;

    SDK::FCrMassPersistentEntityID persistentId{};

    persistentId.ID =
        persistentIDSubsystem->MaxID;

    persistentId.CachedHandle =
        handle;

    if (g_setIDHandlePair == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: SetIDHandlePair is unavailable");

        return returnValue;
    }

    if (!g_setIDHandlePair(
        persistentIDSubsystem,
        &persistentId,
        handle))
    {
        LOG_ERROR(
            "PersistentIdFix: SetIDHandlePair failed for new ID %u "
            "handle=(%u,%u)",
            persistentId.ID,
            handle.Index,
            handle.SerialNumber);

        const uint8_t* map =
            reinterpret_cast<const uint8_t*>(
                &persistentIDSubsystem->HandleIDMap);

        const uint8_t* elementsData =
            *reinterpret_cast<const uint8_t* const*>(
                map + HandleIDMapElementsDataOffset);

        const int32_t elementsNum =
            *reinterpret_cast<const int32_t*>(
                map + HandleIDMapElementsNumOffset);

        bool foundByLinearScan = false;

        if (elementsData != nullptr && elementsNum > 0)
        {
            for (int32_t i = 0; i < elementsNum; ++i)
            {
                const uint8_t* element =
                    elementsData +
                    static_cast<size_t>(i) *
                    SetElementStride;

                const auto* elementHandle =
                    reinterpret_cast<const SDK::FMassEntityHandle*>(
                        element + SetElementHandleOffset);

                if (elementHandle->Index == handle.Index &&
                    elementHandle->SerialNumber == handle.SerialNumber)
                {
                    const uint32_t existingPersistentId =
                        *reinterpret_cast<const uint32_t*>(
                            element + SetElementPersistentIdOffset);

                    LOG_ERROR(
                        "PersistentIdFix: FAILED allocation handle "
                        "FOUND by linear scan: "
                        "element=%d "
                        "handle=(%u,%u) "
                        "existingPersistentId=%u "
                        "elementsNum=%d",
                        i,
                        elementHandle->Index,
                        elementHandle->SerialNumber,
                        existingPersistentId,
                        elementsNum);

                    foundByLinearScan = true;
                    break;
                }
            }
        }

        if (!foundByLinearScan)
        {
            LOG_ERROR(
                "PersistentIdFix: FAILED allocation handle "
                "NOT FOUND by linear scan: "
                "handle=(%u,%u) "
                "elementsData=%p "
                "elementsNum=%d",
                handle.Index,
                handle.SerialNumber,
                elementsData,
                elementsNum);
        }

        return returnValue;
    }

    *returnValue = persistentId;

    PersistentIdFixStats::RecordAssignment();

    return returnValue;
}

// ---------------------------------------------------------------------------
// Persistent-ID subsystem initialisation.
//
// Discovers the live UCrMassPersistentIDSubsystem and builds the reusable-ID
// pool from the IDs that were already consumed before this session started.
//
// This function is shared by:
//   - loaded-save initialisation via OnSaveLoaded()
//   - new-game initialisation via OnNewGame()
//
// The caller is responsible for ensuring that the persistent-ID subsystem
// belongs to the current world.
// ---------------------------------------------------------------------------

static bool PersistentIdSystemInit()
{
    g_persistentIdReuse.Clear();
    g_reusePoolReady = false;
    g_persistentIdSubsystem = nullptr;

    g_statisticsTimerActive = false;
    g_nextStatisticsLogTime = {};
    g_nextLiveStatisticsRefreshTime = {};

    if (g_self == nullptr || g_self->hooks == nullptr)
    {
        LOG_ERROR(
            "PersistentIdFix: hooks are unavailable during persistent ID system initialization");
        return false;
    }

    auto* walker = g_self->hooks->ObjectWalker;

    if (walker == nullptr || !walker->IsReady())
    {
        LOG_ERROR(
            "PersistentIdFix: ObjectWalker is unavailable during persistent ID system initialization");
        return false;
    }

    PluginObjectInfo objects[8] = {};

    const int count = walker->FindObjectsByClassNameInto(
        "CrMassPersistentIDSubsystem",
        PluginObjectLookup_InstanceOnly,
        objects,
        8);

    LOG_INFO(
        "PersistentIdFix: found %d CrMassPersistentIDSubsystem instance(s)",
        count);

    if (count <= 0)
    {
        LOG_ERROR(
            "PersistentIdFix: persistent ID subsystem was not found");
        return false;
    }

    if (count > 1)
    {
        LOG_ERROR(
            "PersistentIdFix: expected exactly one CrMassPersistentIDSubsystem, found %d",
            count);
        return false;
    }

    void* object = objects[0].object;

    LOG_INFO(
        "PersistentIdFix: subsystem object=%p name=%s",
        object,
        objects[0].objectName);

    auto* subsystem =
        static_cast<SDK::UCrMassPersistentIDSubsystem*>(object);

    // The highest existing ID is diagnostic information here.
    // PersistentIdFix does not infer corruption from MaxID alone.
    uint32_t minId = UINT32_MAX;
    uint32_t maxCurrentId = 0;

    for (const auto& pair : subsystem->IDHandleMap)
    {
        const uint32_t id = pair.Key().ID;

        if (id < minId)
            minId = id;

        if (id > maxCurrentId)
            maxCurrentId = id;
    }

    uint64_t maxId = 0;
    bool propertyRead = false;

    auto* properties = g_self->hooks->ObjectProperties;

    if (properties != nullptr && properties->IsReady())
    {
        PluginPropertyHandle property =
            properties->FindPropertyOnObject(object, "MaxID");

        if (property != nullptr)
        {
            int64_t value = 0;

            if (properties->GetIntProperty(
                object,
                property,
                &value))
            {
                if (value < 0 ||
                    static_cast<uint64_t>(value) > UINT32_MAX)
                {
                    LOG_ERROR(
                        "PersistentIdFix: MaxID FProperty value is outside uint32 range: %lld",
                        static_cast<long long>(value));

                    return false;
                }

                maxId = static_cast<uint64_t>(value);
                propertyRead = true;
            }
            else
            {
                LOG_WARN(
                    "PersistentIdFix: MaxID property was found but GetIntProperty failed");
            }
        }
        else
        {
            LOG_WARN(
                "PersistentIdFix: MaxID was not found as an FProperty");
        }
    }
    else
    {
        LOG_WARN(
            "PersistentIdFix: ObjectProperties is unavailable");
    }

    if (!propertyRead)
    {
        // UCrMassPersistentIDSubsystem::MaxID is documented by the
        // generated SDK at offset 0xD0. Read only; never write here.
        const auto* bytes =
            static_cast<const std::uint8_t*>(object);

        maxId =
            static_cast<uint64_t>(
                *reinterpret_cast<const std::uint32_t*>(
                    bytes + 0xD0));
    }

    const uint32_t loadedMaxID =
        static_cast<uint32_t>(maxId);

    // Build the reusable pool from holes below the loaded high-water mark.
    g_persistentIdReuse.Initialize(
        subsystem,
        g_setIDHandlePair);

    g_persistentIdReuse.BuildPool();
    g_reusePoolReady = true;

    if (g_self->hooks->NetMode != nullptr &&
        g_self->hooks->NetMode->GetNetMode != nullptr)
    {
        PersistentIdFixStats::UpdateSnapshot(
            g_self->hooks->NetMode->GetNetMode(),
            static_cast<uint32_t>(
                subsystem->IDHandleMap.Num()),
            loadedMaxID,
            g_persistentIdReuse.GetReusableIDCount(),
            static_cast<uint64_t>(
                g_persistentIdReuse.GetRangeCount()));
    }

    // Keep the live subsystem pointer at plugin level. PersistentIdReuse
    // owns the reusable-ID pool; it does not own the world subsystem.
    g_persistentIdSubsystem = subsystem;
    g_gameSessionActive = true;

    if (g_self->hooks->NetMode != nullptr &&
        g_self->hooks->NetMode->GetNetMode != nullptr)
    {
        const EPluginNetMode netMode =
            g_self->hooks->NetMode->GetNetMode();

        if (netMode == EPluginNetMode::Standalone ||
            netMode == EPluginNetMode::ListenServer ||
            netMode == EPluginNetMode::DedicatedServer)
        {
            g_nextStatisticsLogTime =
                std::chrono::steady_clock::now() +
                std::chrono::minutes(
                    PersistentIdFixConfig::GetLogIntervalMinutes());

            g_statisticsTimerActive = true;
        }
    }

    return true;
}

static bool RefreshPersistentIdStatistics()
{
    if (!g_reusePoolReady)
        return false;

    if (g_persistentIdSubsystem == nullptr)
        return false;

    if (g_self == nullptr ||
        g_self->hooks == nullptr ||
        g_self->hooks->NetMode == nullptr ||
        g_self->hooks->NetMode->GetNetMode == nullptr)
    {
        return false;
    }

    PersistentIdFixStats::UpdateSnapshot(
        g_self->hooks->NetMode->GetNetMode(),
        static_cast<std::uint32_t>(
            g_persistentIdSubsystem->IDHandleMap.Num()),
        g_persistentIdSubsystem->MaxID,
        g_persistentIdReuse.GetReusableIDCount(),
        static_cast<std::uint64_t>(
            g_persistentIdReuse.GetRangeCount()));

    return true;
}

// ---------------------------------------------------------------------------
// Save-loaded initialisation.
// ---------------------------------------------------------------------------

static void OnSaveLoaded()
{
    if (!PersistentIdSystemInit())
    {
        LOG_ERROR(
            "PersistentIdFix: persistent ID system initialization failed after save load");
        return;
    }

    if (!RefreshPersistentIdStatistics())
    {
        LOG_WARN(
            "PersistentIdFix: statistics refresh failed after save load");
    }
    else
    {
        PersistentIdFixStats::LogStats();
    }
}

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

    /*
     * -----------------------------------------------------------------------
     * Local statistics lifecycle.
     *
     * Nothing below this point should run merely because the process is a
     * network client. A remote client has no local persistent-ID subsystem.
     */
    if (!g_statisticsTimerActive)
        return;

    const auto now =
        std::chrono::steady_clock::now();

    /*
     * The statistics snapshot is refreshed once per second when either:
     *
     *   1. the local statistics UI is visible, or
     *   2. a remote client currently has the statistics UI open.
     *
     * The second case is important for dedicated servers, which have no
     * local UI.
     */
    bool statisticsRefreshRequired = false;

#ifdef MODLOADER_CLIENT_BUILD
    if (PersistentIdFixUI::IsVisible())
    {
        statisticsRefreshRequired = true;
    }
#endif

    if (PersistentIdFixNetwork::HasSubscribedClients())
    {
        statisticsRefreshRequired = true;
    }

    if (statisticsRefreshRequired &&
        now >= g_nextLiveStatisticsRefreshTime)
    {
        if (!RefreshPersistentIdStatistics())
        {
            LOG_WARN(
                "PersistentIdFix: live statistics refresh failed");
        }
        else
        {
            const PersistentIdFixStats::Snapshot snapshot =
                PersistentIdFixStats::GetSnapshot();

            /*
             * On a server/listen host this sends the same complete snapshot
             * that the local UI sees to all subscribed remote clients.
             */
            if (PersistentIdFixNetwork::HasSubscribedClients())
            {
                PersistentIdFixNetwork::SendSnapshotToSubscribedClients(
                    snapshot);
            }
        }

        g_nextLiveStatisticsRefreshTime =
            now +
            std::chrono::seconds(1);
    }

    /*
     * Periodic logging is server/standalone only.
     *
     * A pure remote client does not have a local persistent-ID subsystem and
     * therefore never produces local statistics logs.
     */
    if (netMode == EPluginNetMode::Client)
        return;

    if (now < g_nextStatisticsLogTime)
        return;

    if (!RefreshPersistentIdStatistics())
    {
        LOG_WARN(
            "PersistentIdFix: periodic statistics refresh failed");
    }
    else
    {
        PersistentIdFixStats::LogStats();
    }

    g_nextStatisticsLogTime =
        now +
        std::chrono::minutes(
            PersistentIdFixConfig::GetLogIntervalMinutes());
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

    /*
     * Only perform final local statistics processing when a local persistent-ID
     * game session was actually active.
     *
     * Main-menu/transition worlds such as "Untitled" must not generate a
     * final statistics log.
     */
    if (g_gameSessionActive)
    {
        if (g_statisticsTimerActive)
        {
            if (RefreshPersistentIdStatistics())
            {
                PersistentIdFixStats::LogStats();
            }
            else
            {
                LOG_WARN(
                    "PersistentIdFix: final statistics refresh failed at world end");
            }
        }
    }

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

    g_gameSessionActive = false;

    g_statisticsTimerActive = false;
    g_nextStatisticsLogTime = {};
    g_nextLiveStatisticsRefreshTime = {};

    g_persistentIdSubsystem = nullptr;

    g_persistentIdReuse.Clear();
    g_reusePoolReady = false;

    PersistentIdFixStats::Reset();
}

bool IsGameSessionActive()
{
    return g_gameSessionActive;
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

            g_setIDHandlePair =
                reinterpret_cast<SetIDHandlePairFn>(
                    g_setIDHandlePairAddress);

            //
            // Install GetOrAddIDForHandle hook
            //
            g_getOrAddIDForHandleHook =
                g_self->hooks->Hooks->Install(
                    g_getOrAddIDForHandleAddress,
                    reinterpret_cast<void*>(&GetOrAddIDForHandleDetour),
                    reinterpret_cast<void**>(
                        &g_originalGetOrAddIDForHandle));

            if (g_getOrAddIDForHandleHook == nullptr ||
                g_originalGetOrAddIDForHandle == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: failed to install GetOrAddIDForHandle hook");

                g_getOrAddIDForHandleHook = nullptr;
                g_originalGetOrAddIDForHandle = nullptr;
                g_setIDHandlePair = nullptr;

                break;
            }

            bInitNativeHook = true;

            LOG_INFO(
                "PersistentIdFix: GetOrAddIDForHandle hook installed");

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
            g_setIDHandlePair = nullptr;
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

        g_persistentIdSubsystem = nullptr;
        g_persistentIdReuse.Clear();
        g_reusePoolReady = false;

        g_gameSessionActive = false;

        g_statisticsTimerActive = false;
        g_nextStatisticsLogTime = {};
        g_nextLiveStatisticsRefreshTime = {};

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
            }

            g_statisticsTimerActive = false;
            g_nextStatisticsLogTime = {};
            g_nextLiveStatisticsRefreshTime = {};

            g_persistentIdSubsystem = nullptr;

            g_persistentIdReuse.Clear();
            g_reusePoolReady = false;
            g_gameSessionActive = false;
            g_gameWorldActive = false;

            g_getOrAddIDForHandleHook = nullptr;
            g_originalGetOrAddIDForHandle = nullptr;
            g_setIDHandlePair = nullptr;

            g_getOrAddIDForHandleAddress = 0;
            g_setIDHandlePairAddress = 0;

            g_self = nullptr;
        }

    }


}



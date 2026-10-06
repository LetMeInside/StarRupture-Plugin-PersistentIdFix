#include "PersistentId/PersistentIdSystem.h"

#include "plugin_helpers.h"
#include "PersistentIdReuse.h"
#include "Native/NativeIdLookup.h"
#include "Stats/Stats.h"
#include "Config/Config.h"
#include "Network/Network.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "UI/UI.h"
#endif

#include <cstdint>
#include <chrono>
#include <mutex>
#include <vector>

namespace
{
    static IPluginSelf* g_systemSelf = nullptr;

    static PersistentIdReuse g_persistentIdReuse;
    static SDK::UCrMassPersistentIDSubsystem* g_persistentIdSubsystem = nullptr;

    static SetIDHandlePairFn g_setIDHandlePair = nullptr;
    static GetOrAddIDForHandleFn g_originalGetOrAddIDForHandle = nullptr;

    static bool g_reusePoolReady = false;
    static bool g_sessionActive = false;

    static EPluginNetMode g_sessionNetMode = EPluginNetMode::Unknown;

    enum class LedgerEntryKind : std::uint8_t
    {
        Assigned = 0,
        Burned
    };

    struct LedgerEntry
    {
        SDK::UCrMassPersistentIDSubsystem* subsystem = nullptr;
        std::uint32_t id = 0;
        LedgerEntryKind kind = LedgerEntryKind::Assigned;
    };

    static std::mutex g_assignmentLedgerMutex;
    static std::vector<LedgerEntry> g_assignmentLedger;
    static std::uint64_t g_assignmentLedgerGeneration = 0;
    static bool g_assignmentLedgerHealthy = true;

    static std::chrono::steady_clock::time_point g_nextStatisticsLogTime{};
    static std::chrono::steady_clock::time_point g_nextLiveStatisticsRefreshTime{};
    static bool g_statisticsTimerActive = false;

    static void ResetAssignmentLedger(
        std::uint64_t loadGeneration)
    {
        std::lock_guard<std::mutex> lock(g_assignmentLedgerMutex);
        g_assignmentLedger.clear();
        g_assignmentLedgerGeneration = loadGeneration;
        g_assignmentLedgerHealthy = true;
    }

    static bool RecordLedgerEntry(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        std::uint32_t id,
        LedgerEntryKind kind)
    {
        try
        {
            std::lock_guard<std::mutex> lock(g_assignmentLedgerMutex);
            g_assignmentLedger.push_back({ subsystem, id, kind });
            return true;
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(g_assignmentLedgerMutex);
            g_assignmentLedgerHealthy = false;
            return false;
        }
    }

    static void RecordAssignedId(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        std::uint32_t id)
    {
        if (!RecordLedgerEntry(
                subsystem,
                id,
                LedgerEntryKind::Assigned))
        {
            LOG_ERROR(
                "PersistentIdFix: assignment ledger failed to record assigned ID %u; future certified activation must remain fail-closed",
                id);
        }
    }

    static void RecordBurnedId(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        std::uint32_t id)
    {
        if (!RecordLedgerEntry(
                subsystem,
                id,
                LedgerEntryKind::Burned))
        {
            LOG_ERROR(
                "PersistentIdFix: assignment ledger failed to record burned ID %u; future certified activation must remain fail-closed",
                id);
        }
    }

    static EPluginNetMode GetEffectiveSessionNetMode()
    {
        if (g_sessionNetMode != EPluginNetMode::Unknown)
            return g_sessionNetMode;

        if (g_systemSelf == nullptr ||
            g_systemSelf->hooks == nullptr ||
            g_systemSelf->hooks->NetMode == nullptr ||
            g_systemSelf->hooks->NetMode->GetNetMode == nullptr)
        {
            return EPluginNetMode::Unknown;
        }

        return g_systemSelf->hooks->NetMode->GetNetMode();
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

    static bool InitializeSession()
    {
        g_persistentIdReuse.Clear();
        g_reusePoolReady = false;
        g_persistentIdSubsystem = nullptr;

        g_statisticsTimerActive = false;
        g_nextStatisticsLogTime = {};
        g_nextLiveStatisticsRefreshTime = {};

        if (g_systemSelf == nullptr || g_systemSelf->hooks == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: hooks are unavailable during persistent ID system initialization");
            return false;
        }

        auto* walker = g_systemSelf->hooks->ObjectWalker;

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

        auto* properties = g_systemSelf->hooks->ObjectProperties;

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

        const EPluginNetMode netMode =
            GetEffectiveSessionNetMode();

        PersistentIdFixStats::UpdateSnapshot(
            netMode,
            static_cast<uint32_t>(
                subsystem->IDHandleMap.Num()),
            loadedMaxID,
            g_persistentIdReuse.GetReusableIDCount(),
            static_cast<uint64_t>(
                g_persistentIdReuse.GetRangeCount()));

        g_persistentIdSubsystem = subsystem;
        g_sessionActive = true;

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

        return true;
    }

    static bool RefreshPersistentIdStatistics()
    {
        if (!g_reusePoolReady)
            return false;

        if (g_persistentIdSubsystem == nullptr)
            return false;

        PersistentIdFixStats::UpdateSnapshot(
            GetEffectiveSessionNetMode(),
            static_cast<std::uint32_t>(
                g_persistentIdSubsystem->IDHandleMap.Num()),
            g_persistentIdSubsystem->MaxID,
            g_persistentIdReuse.GetReusableIDCount(),
            static_cast<std::uint64_t>(
                g_persistentIdReuse.GetRangeCount()));

        return true;
    }

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

        if (!InitializeSession())
        {
            LOG_ERROR(
                "PersistentIdFix: persistent ID system initialization failed for new game");
            return;
        }

        LOG_INFO(
            "PersistentIdFix: new-game persistent ID system initialized");
    }
}

namespace PersistentIdFixSystem
{
    void Configure(
        IPluginSelf* self,
        SetIDHandlePairFn setIDHandlePair,
        GetOrAddIDForHandleFn originalGetOrAddIDForHandle)
    {
        g_systemSelf = self;
        g_setIDHandlePair = setIDHandlePair;
        g_originalGetOrAddIDForHandle = originalGetOrAddIDForHandle;
    }

    void Reset()
    {
        g_persistentIdReuse.Clear();
        g_persistentIdSubsystem = nullptr;
        g_reusePoolReady = false;
        g_sessionActive = false;
        g_sessionNetMode = EPluginNetMode::Unknown;

        ResetAssignmentLedger(0);

        g_statisticsTimerActive = false;
        g_nextStatisticsLogTime = {};
        g_nextLiveStatisticsRefreshTime = {};

        PersistentIdFixStats::Reset();

        g_setIDHandlePair = nullptr;
        g_originalGetOrAddIDForHandle = nullptr;
        g_systemSelf = nullptr;
    }

    void BeginLoadGeneration(
        std::uint64_t loadGeneration)
    {
        ResetAssignmentLedger(loadGeneration);

        LOG_INFO(
            "PersistentIdFix: assignment ledger generation started: %llu",
            static_cast<unsigned long long>(loadGeneration));
    }

    AssignmentLedgerSnapshot GetAssignmentLedgerSnapshot()
    {
        AssignmentLedgerSnapshot snapshot;

        std::lock_guard<std::mutex> lock(g_assignmentLedgerMutex);

        snapshot.loadGeneration = g_assignmentLedgerGeneration;
        snapshot.healthy = g_assignmentLedgerHealthy;

        for (const LedgerEntry& entry : g_assignmentLedger)
        {
            const bool currentSubsystem =
                g_persistentIdSubsystem != nullptr &&
                entry.subsystem == g_persistentIdSubsystem;

            if (entry.kind == LedgerEntryKind::Assigned)
            {
                ++snapshot.assignedEntries;

                if (currentSubsystem)
                    ++snapshot.currentSubsystemAssigned;
                else
                    ++snapshot.foreignSubsystemAssigned;
            }
            else
            {
                ++snapshot.burnedEntries;

                if (currentSubsystem)
                    ++snapshot.currentSubsystemBurned;
                else
                    ++snapshot.foreignSubsystemBurned;
            }
        }

        return snapshot;
    }

    void LogAssignmentLedger(
        const char* phase)
    {
        const AssignmentLedgerSnapshot snapshot =
            GetAssignmentLedgerSnapshot();

        LOG_INFO(
            "PersistentIdFix: assignment ledger [%s]: generation=%llu healthy=%u assigned=%llu burned=%llu currentAssigned=%llu currentBurned=%llu foreignAssigned=%llu foreignBurned=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.loadGeneration),
            snapshot.healthy ? 1u : 0u,
            static_cast<unsigned long long>(snapshot.assignedEntries),
            static_cast<unsigned long long>(snapshot.burnedEntries),
            static_cast<unsigned long long>(snapshot.currentSubsystemAssigned),
            static_cast<unsigned long long>(snapshot.currentSubsystemBurned),
            static_cast<unsigned long long>(snapshot.foreignSubsystemAssigned),
            static_cast<unsigned long long>(snapshot.foreignSubsystemBurned));
    }

    void SetSessionNetMode(EPluginNetMode netMode)
    {
        if (g_sessionNetMode != EPluginNetMode::Unknown)
            return;

        if (netMode == EPluginNetMode::Standalone ||
            netMode == EPluginNetMode::ListenServer ||
            netMode == EPluginNetMode::DedicatedServer ||
            netMode == EPluginNetMode::Client)
        {
            g_sessionNetMode = netMode;
        }
    }

    EPluginNetMode GetSessionNetMode()
    {
        return GetEffectiveSessionNetMode();
    }

    void OnSaveLoaded()
    {
        if (!InitializeSession())
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

    void Tick()
    {
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

        if (g_systemSelf == nullptr ||
            g_systemSelf->hooks == nullptr ||
            g_systemSelf->hooks->NetMode == nullptr ||
            g_systemSelf->hooks->NetMode->GetNetMode == nullptr)
        {
            return;
        }

        const EPluginNetMode netMode =
            g_systemSelf->hooks->NetMode->GetNetMode();

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

    void EndSession()
    {
        /*
         * Only perform final local statistics processing when a local persistent-ID
         * game session was actually active.
         *
         * Main-menu/transition worlds such as "Untitled" must not generate a
         * final statistics log.
         */
        if (g_sessionActive)
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

        g_sessionActive = false;

        g_statisticsTimerActive = false;
        g_nextStatisticsLogTime = {};
        g_nextLiveStatisticsRefreshTime = {};

        g_persistentIdSubsystem = nullptr;

        g_persistentIdReuse.Clear();
        g_reusePoolReady = false;

        PersistentIdFixStats::Reset();

        g_sessionNetMode = EPluginNetMode::Unknown;
    }

    bool IsSessionActive()
    {
        return g_sessionActive;
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

    SDK::FCrMassPersistentEntityID* GetOrAddIDForHandleDetour(
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
        returnValue->ID = UINT32_MAX;

        if (subsystem == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: GetOrAddIDForHandle subsystem is null");

            return returnValue;
        }

        /*
         * Determine whether this process is a remote client.
         *
         * Once a concrete game-world role has been established, retain that role for
         * the lifetime of the world. Runtime NetMode can change during teardown and
         * must not cause a remote client to enter the local persistent-ID path.
         *
         * Before the role has been established, retain the startup fallback because
         * GetNetMode() can remain Unknown for some time on a remote client.
         */
        bool isRemoteClient =
            (g_sessionNetMode == EPluginNetMode::Client);

        if (g_sessionNetMode == EPluginNetMode::Unknown &&
            g_systemSelf != nullptr &&
            g_systemSelf->hooks != nullptr &&
            g_systemSelf->hooks->NetMode != nullptr)
        {
            if (g_systemSelf->hooks->NetMode->GetNetMode != nullptr)
            {
                const EPluginNetMode netMode =
                    g_systemSelf->hooks->NetMode->GetNetMode();

                if (netMode == EPluginNetMode::Client)
                {
                    isRemoteClient = true;
                }
                else if (netMode == EPluginNetMode::Unknown &&
                    g_systemSelf->hooks->NetMode->IsServer != nullptr &&
                    !g_systemSelf->hooks->NetMode->IsServer())
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
         * On a local authority session, the game can call
         * GetOrAddIDForHandle before the reusable-ID pool is ready.
         *
         * Do not delegate new allocations to the native allocator during this
         * window: the native implementation increments MaxID without guarding
         * the invalid 0xFFFFFFFF persistent-ID sentinel. With no reusable pool
         * available yet, fall through to the bounded monotonic path below.
         */
        if (!g_reusePoolReady)
        {
            LOG_INFO(
                "PersistentIdFix: GetOrAddIDForHandle before reuse pool was built; "
                "using bounded monotonic allocation: handle=(%u,%u) MaxID=%u",
                handle.Index,
                handle.SerialNumber,
                persistentIDSubsystem->MaxID);
        }

        /*
         * No existing mapping.
         *
         * First consume an ID hole that existed when the save was loaded.
         */
        switch (g_persistentIdReuse.TryAllocate(handle, *returnValue))
        {
        case PersistentIdAllocationResult::Allocated:
            RecordAssignedId(
                persistentIDSubsystem,
                returnValue->ID);
            PersistentIdFixStats::RecordAssignment();
            return returnValue;

        case PersistentIdAllocationResult::NoReusableId:
            break;

        case PersistentIdAllocationResult::Failed:
            // This is logged inside TryAllocate.
            return returnValue;
        }

        /*
         * No reusable IDs remain, or the reusable-ID pool is not ready yet.
         *
         * Use bounded monotonic allocation while preserving the native mapping
         * update path. Unlike the native allocator, never advance MaxID into the
         * invalid 0xFFFFFFFF persistent-ID sentinel.
         */
        if (persistentIDSubsystem->MaxID >= UINT32_MAX - 1u)
        {
            LOG_ERROR(
                "PersistentIdFix: persistent ID space exhausted; "
                "no reusable IDs remain and MaxID=%u cannot advance without "
                "reaching the invalid 0xFFFFFFFF persistent ID",
                persistentIDSubsystem->MaxID);

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
            RecordBurnedId(
                persistentIDSubsystem,
                persistentId.ID);

            LOG_ERROR(
                "PersistentIdFix: SetIDHandlePair is unavailable");

            return returnValue;
        }

        if (!g_setIDHandlePair(
            persistentIDSubsystem,
            &persistentId,
            handle))
        {
            RecordBurnedId(
                persistentIDSubsystem,
                persistentId.ID);

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

        RecordAssignedId(
            persistentIDSubsystem,
            persistentId.ID);

        PersistentIdFixStats::RecordAssignment();

        return returnValue;
    }
}

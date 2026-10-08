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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <intrin.h>
#include <mutex>
#include <vector>

namespace
{
    static IPluginSelf* g_systemSelf = nullptr;

    static PersistentIdReuse g_persistentIdReuse;
    static SDK::UCrMassPersistentIDSubsystem* g_persistentIdSubsystem = nullptr;
    static SDK::UWorld* g_activeGameWorld = nullptr;
    static bool g_subsystemWorldVerified = false;
    static bool g_unboundAllocatorWarningLogged = false;
    static std::atomic_bool g_lateAttachmentDetected{ false };
    static std::atomic_bool g_lateAttachmentNotificationPending{ false };

    static void* ReadUObjectOuter(const void* object)
    {
        if (object == nullptr)
            return nullptr;

        const auto* bytes =
            static_cast<const std::uint8_t*>(object);

        return *reinterpret_cast<void* const*>(bytes + 0x20);
    }

    // Snapshot of IDs present in IDHandleMap when the authoritative
    // session was initialized. This intentionally remains stable for
    // the session so IDs freed later are never treated as reusable.
    static std::vector<std::uint32_t> g_loadedHandleIds;
    static std::uint32_t g_loadedHighWater = 0;

    static SetIDHandlePairFn g_setIDHandlePair = nullptr;
    static GetOrAddIDForHandleFn g_originalGetOrAddIDForHandle = nullptr;

    static bool g_reusePoolReady = false;

    // Loaded-save reuse remains disabled until the complete source
    // certificate has produced and promoted the staged pool.
    static bool g_certifiedPoolActive = false;

    static bool g_sessionActive = false;

    static EPluginNetMode g_sessionNetMode = EPluginNetMode::Unknown;

    enum class AllocationFailureReason : std::uint32_t
    {
        NullReturnBuffer = 1,
        NullSubsystem,
        NativeAllocatorUnavailable,
        InvalidExistingMapping,
        RecycledAllocationFailed,
        PersistentIdSpaceExhausted,
        NativeSetterUnavailable,
        InvalidSetterInput,
        SetterRegistrationFailed,
        SetterPostconditionFailed
    };

    static const char* AllocationFailureReasonName(
        AllocationFailureReason reason)
    {
        switch (reason)
        {
        case AllocationFailureReason::NullReturnBuffer:
            return "null return buffer";
        case AllocationFailureReason::NullSubsystem:
            return "null persistent-ID subsystem";
        case AllocationFailureReason::NativeAllocatorUnavailable:
            return "native allocator unavailable";
        case AllocationFailureReason::InvalidExistingMapping:
            return "invalid existing persistent-ID mapping";
        case AllocationFailureReason::RecycledAllocationFailed:
            return "recycled-ID allocation failed";
        case AllocationFailureReason::PersistentIdSpaceExhausted:
            return "persistent-ID space exhausted";
        case AllocationFailureReason::NativeSetterUnavailable:
            return "native SetIDHandlePair unavailable";
        case AllocationFailureReason::InvalidSetterInput:
            return "invalid SetIDHandlePair input";
        case AllocationFailureReason::SetterRegistrationFailed:
            return "SetIDHandlePair registration failed";
        case AllocationFailureReason::SetterPostconditionFailed:
            return "SetIDHandlePair postcondition failed";
        default:
            return "unknown terminal allocation failure";
        }
    }

    [[noreturn]] static void FatalAllocationFailure(
        AllocationFailureReason reason,
        void* subsystem,
        SDK::FMassEntityHandle handle,
        std::uint32_t attemptedId,
        std::uint32_t maxId)
    {
        LOG_ERROR(
            "PersistentIdFix: terminal persistent-ID allocation failure: "
            "reason=%s reasonCode=%u subsystem=%p handle=(%u,%u) "
            "attemptedId=%u MaxID=%u",
            AllocationFailureReasonName(reason),
            static_cast<unsigned int>(reason),
            subsystem,
            handle.Index,
            handle.SerialNumber,
            attemptedId,
            maxId);

        __fastfail(7u);
    }

    static bool IsExactPersistentIdMapping(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        std::uint32_t id,
        const SDK::FMassEntityHandle& handle)
    {
        if (subsystem == nullptr ||
            id == 0 ||
            id == UINT32_MAX)
        {
            return false;
        }

        const SDK::FMassEntityHandle* forward =
            FindHandleByPersistentId(
                subsystem,
                id);

        if (forward == nullptr ||
            forward->Index != handle.Index ||
            forward->SerialNumber != handle.SerialNumber)
        {
            return false;
        }

        const SDK::FCrMassPersistentEntityID* reverse =
            FindPersistentIdByHandle(
                subsystem,
                handle);

        return reverse != nullptr &&
            reverse->ID == id;
    }

    static bool CheckedSetIDHandlePair(
        void* subsystem,
        SDK::FCrMassPersistentEntityID* persistentId,
        SDK::FMassEntityHandle handle)
    {
        auto* typedSubsystem =
            static_cast<SDK::UCrMassPersistentIDSubsystem*>(subsystem);

        if (typedSubsystem == nullptr ||
            persistentId == nullptr ||
            persistentId->ID == 0 ||
            persistentId->ID == UINT32_MAX)
        {
            FatalAllocationFailure(
                AllocationFailureReason::InvalidSetterInput,
                subsystem,
                handle,
                persistentId != nullptr
                    ? persistentId->ID
                    : UINT32_MAX,
                typedSubsystem != nullptr
                    ? typedSubsystem->MaxID
                    : 0);
        }

        if (g_setIDHandlePair == nullptr)
        {
            FatalAllocationFailure(
                AllocationFailureReason::NativeSetterUnavailable,
                subsystem,
                handle,
                persistentId->ID,
                typedSubsystem->MaxID);
        }

        const bool nativeResult =
            g_setIDHandlePair(
                subsystem,
                persistentId,
                handle);

        if (!IsExactPersistentIdMapping(
                typedSubsystem,
                persistentId->ID,
                handle))
        {
            FatalAllocationFailure(
                nativeResult
                    ? AllocationFailureReason::SetterPostconditionFailed
                    : AllocationFailureReason::SetterRegistrationFailed,
                subsystem,
                handle,
                persistentId->ID,
                typedSubsystem->MaxID);
        }

        return true;
    }

    static void RevokeCertifiedPool(const char* reason)
    {
        const bool wasActive = g_certifiedPoolActive;

        g_certifiedPoolActive = false;
        g_persistentIdReuse.ClearStagedPool();

        if (wasActive)
        {
            LOG_INFO(
                "PersistentIdFix: certified reusable pool revoked: %s",
                reason != nullptr ? reason : "<unspecified>");
        }
    }

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
    static bool g_assignmentLedgerRetainsEntries = true;
    static SDK::UCrMassPersistentIDSubsystem*
        g_assignmentLedgerCompactedSubsystem = nullptr;
    static std::uint64_t g_assignmentLedgerCompactedAssigned = 0;
    static std::uint64_t g_assignmentLedgerCompactedBurned = 0;
    static std::uint64_t g_assignmentLedgerCompactedForeignAssigned = 0;
    static std::uint64_t g_assignmentLedgerCompactedForeignBurned = 0;

#ifdef PERSISTENTIDFIX_DIAGNOSTICS
    static std::mutex g_setPairObserverMutex;
    static PersistentIdFixSystem::SetPairObserverSnapshot g_setPairObserver;
#endif

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
        g_assignmentLedgerRetainsEntries = true;
        g_assignmentLedgerCompactedSubsystem = nullptr;
        g_assignmentLedgerCompactedAssigned = 0;
        g_assignmentLedgerCompactedBurned = 0;
        g_assignmentLedgerCompactedForeignAssigned = 0;
        g_assignmentLedgerCompactedForeignBurned = 0;
    }

    static void CompactAssignmentLedgerLocked()
    {
        g_assignmentLedgerCompactedSubsystem =
            g_persistentIdSubsystem;

        g_assignmentLedgerCompactedAssigned = 0;
        g_assignmentLedgerCompactedBurned = 0;
        g_assignmentLedgerCompactedForeignAssigned = 0;
        g_assignmentLedgerCompactedForeignBurned = 0;

        for (const LedgerEntry& entry : g_assignmentLedger)
        {
            const bool compactedSubsystem =
                g_assignmentLedgerCompactedSubsystem != nullptr &&
                entry.subsystem ==
                    g_assignmentLedgerCompactedSubsystem;

            if (entry.kind == LedgerEntryKind::Assigned)
            {
                if (compactedSubsystem)
                    ++g_assignmentLedgerCompactedAssigned;
                else
                    ++g_assignmentLedgerCompactedForeignAssigned;
            }
            else
            {
                if (compactedSubsystem)
                    ++g_assignmentLedgerCompactedBurned;
                else
                    ++g_assignmentLedgerCompactedForeignBurned;
            }
        }

        std::vector<LedgerEntry> empty;
        g_assignmentLedger.swap(empty);
        g_assignmentLedgerRetainsEntries = false;
    }


    static void ResetSetPairObserver(
        std::uint64_t loadGeneration)
    {
#ifdef PERSISTENTIDFIX_DIAGNOSTICS
        std::lock_guard<std::mutex> lock(g_setPairObserverMutex);
        g_setPairObserver = {};
        g_setPairObserver.loadGeneration = loadGeneration;
#else
        (void)loadGeneration;
#endif
    }

#ifdef PERSISTENTIDFIX_DIAGNOSTICS
    struct SetPairState
    {
        bool pidPresent = false;
        bool handlePresent = false;
        bool exactPair = false;
    };

    static SetPairState InspectSetPairState(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        std::uint32_t id,
        const SDK::FMassEntityHandle& handle)
    {
        SetPairState state;

        if (subsystem == nullptr)
            return state;

        const auto* mappedHandle =
            FindHandleByPersistentId(
                subsystem,
                id);

        state.pidPresent = mappedHandle != nullptr;

        const bool exactForward =
            mappedHandle != nullptr &&
            mappedHandle->Index == handle.Index &&
            mappedHandle->SerialNumber == handle.SerialNumber;

        const auto* mappedId =
            FindPersistentIdByHandle(
                subsystem,
                handle);

        state.handlePresent = mappedId != nullptr;

        const bool exactReverse =
            mappedId != nullptr &&
            mappedId->ID == id;

        state.exactPair =
            exactForward && exactReverse;

        return state;
    }
#endif
    static bool RecordLedgerEntry(
        SDK::UCrMassPersistentIDSubsystem* subsystem,
        std::uint32_t id,
        LedgerEntryKind kind)
    {
        try
        {
            std::lock_guard<std::mutex> lock(g_assignmentLedgerMutex);

            if (g_assignmentLedgerRetainsEntries)
            {
                g_assignmentLedger.push_back({ subsystem, id, kind });
                return true;
            }

            const bool compactedSubsystem =
                g_assignmentLedgerCompactedSubsystem != nullptr &&
                subsystem ==
                    g_assignmentLedgerCompactedSubsystem;

            if (kind == LedgerEntryKind::Assigned)
            {
                if (compactedSubsystem)
                    ++g_assignmentLedgerCompactedAssigned;
                else
                    ++g_assignmentLedgerCompactedForeignAssigned;
            }
            else
            {
                if (compactedSubsystem)
                    ++g_assignmentLedgerCompactedBurned;
                else
                    ++g_assignmentLedgerCompactedForeignBurned;
            }

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
    // Binds the allocator to the current gameplay world, snapshots the loaded
    // ID map/high-water state, and prepares the baseline hole map. Loaded-save
    // reuse remains disabled until serialized source coverage is certified and
    // a protected candidate pool is staged and promoted.
    //
    // New games enter through the same initialization path when the allocator
    // is first observed empty. The caller is responsible for ensuring that the
    // persistent-ID subsystem belongs to the current world.
    // ---------------------------------------------------------------------------

    static bool InitializeSession(SDK::UCrMassPersistentIDSubsystem* requestedSubsystem)
    {
        g_persistentIdReuse.Clear();
        g_reusePoolReady = false;
        g_certifiedPoolActive = false;
        g_persistentIdSubsystem = nullptr;
        g_subsystemWorldVerified = false;
        g_loadedHandleIds.clear();
        g_loadedHighWater = 0;

        g_statisticsTimerActive = false;
        g_nextStatisticsLogTime = {};
        g_nextLiveStatisticsRefreshTime = {};

        if (g_systemSelf == nullptr || g_systemSelf->hooks == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: hooks are unavailable during persistent ID system initialization");
            return false;
        }

        SDK::UCrMassPersistentIDSubsystem* subsystem = requestedSubsystem;
        void* object = requestedSubsystem;

        if (subsystem != nullptr)
        {
            LOG_INFO(
                "PersistentIdFix: using allocator caller subsystem directly: object=%p outer=%p activeWorld=%p",
                static_cast<void*>(subsystem),
                ReadUObjectOuter(subsystem),
                static_cast<void*>(g_activeGameWorld));
        }
        else
        {
            if (g_activeGameWorld == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: loaded-session subsystem discovery rejected: no observed ChimeraMain world");
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
                "PersistentIdFix: found %d CrMassPersistentIDSubsystem instance(s); selecting by Outer world=%p",
                count,
                static_cast<void*>(g_activeGameWorld));

            if (count <= 0)
            {
                LOG_ERROR(
                    "PersistentIdFix: persistent ID subsystem was not found");
                return false;
            }

            if (count > 8)
            {
                LOG_ERROR(
                    "PersistentIdFix: subsystem discovery truncated: found %d instances, capacity=8",
                    count);
                return false;
            }

            int worldMatches = 0;
            PluginObjectInfo* selected = nullptr;

            for (int i = 0; i < count; ++i)
            {
                void* candidate = objects[i].object;
                void* outer = ReadUObjectOuter(candidate);

                LOG_INFO(
                    "PersistentIdFix: subsystem candidate object=%p name=%s outer=%p worldMatch=%u",
                    candidate,
                    objects[i].objectName,
                    outer,
                    outer == g_activeGameWorld ? 1u : 0u);

                if (outer == g_activeGameWorld)
                {
                    ++worldMatches;
                    selected = &objects[i];
                }
            }

            if (worldMatches != 1 || selected == nullptr)
            {
                LOG_ERROR(
                    "PersistentIdFix: expected exactly one persistent ID subsystem owned by active ChimeraMain world, found %d",
                    worldMatches);
                return false;
            }

            object = selected->object;
            subsystem =
                static_cast<SDK::UCrMassPersistentIDSubsystem*>(object);

            LOG_INFO(
                "PersistentIdFix: selected world-owned subsystem object=%p name=%s",
                object,
                selected->objectName);
        }

        if (g_activeGameWorld != nullptr)
        {
            g_subsystemWorldVerified =
                ReadUObjectOuter(subsystem) == g_activeGameWorld;

            if (!g_subsystemWorldVerified)
            {
                LOG_ERROR(
                    "PersistentIdFix: allocator subsystem ownership mismatch: subsystem=%p outer=%p activeWorld=%p",
                    static_cast<void*>(subsystem),
                    ReadUObjectOuter(subsystem),
                    static_cast<void*>(g_activeGameWorld));
                return false;
            }
        }

        // The highest existing ID is diagnostic information here.
        // PersistentIdFix does not infer corruption from MaxID alone.
        uint32_t minId = UINT32_MAX;
        uint32_t maxCurrentId = 0;

        try
        {
            g_loadedHandleIds.reserve(
                static_cast<std::size_t>(subsystem->IDHandleMap.Num()));

            for (const auto& pair : subsystem->IDHandleMap)
            {
                const uint32_t id = pair.Key().ID;

                g_loadedHandleIds.push_back(id);

                if (id < minId)
                    minId = id;

                if (id > maxCurrentId)
                    maxCurrentId = id;
            }

            std::sort(
                g_loadedHandleIds.begin(),
                g_loadedHandleIds.end());

            g_loadedHandleIds.erase(
                std::unique(
                    g_loadedHandleIds.begin(),
                    g_loadedHandleIds.end()),
                g_loadedHandleIds.end());
        }
        catch (...)
        {
            LOG_ERROR(
                "PersistentIdFix: failed to capture loaded IDHandleMap snapshot");
            g_loadedHandleIds.clear();
            return false;
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

        g_loadedHighWater = loadedMaxID;

        // Build the reusable pool from holes below the loaded high-water mark.
        g_persistentIdReuse.Initialize(
            subsystem,
            CheckedSetIDHandlePair);

        g_persistentIdReuse.BuildPool();
        g_reusePoolReady = true;

        const EPluginNetMode netMode =
            GetEffectiveSessionNetMode();

        PersistentIdFixStats::UpdateSnapshot(
            netMode,
            static_cast<uint32_t>(
                subsystem->IDHandleMap.Num()),
            loadedMaxID,
            g_certifiedPoolActive
                ? g_persistentIdReuse.GetReusableIDCount()
                : 0u,
            g_certifiedPoolActive
                ? static_cast<uint64_t>(
                    g_persistentIdReuse.GetRangeCount())
                : 0u);

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
            g_certifiedPoolActive
                ? g_persistentIdReuse.GetReusableIDCount()
                : 0u,
            g_certifiedPoolActive
                ? static_cast<std::uint64_t>(
                    g_persistentIdReuse.GetRangeCount())
                : 0u);

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

    static void OnNewGame(SDK::UCrMassPersistentIDSubsystem* subsystem)
    {
        ResetSetPairObserver(0);

        LOG_INFO(
            "PersistentIdFix: new game detected");

        if (!InitializeSession(subsystem))
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
        g_loadedHandleIds.clear();
        g_loadedHighWater = 0;
        g_reusePoolReady = false;
        g_certifiedPoolActive = false;
        g_sessionActive = false;
        g_sessionNetMode = EPluginNetMode::Unknown;
        g_activeGameWorld = nullptr;
        g_subsystemWorldVerified = false;
        g_unboundAllocatorWarningLogged = false;
        g_lateAttachmentDetected.store(false);
        g_lateAttachmentNotificationPending.store(false);

        ResetAssignmentLedger(0);
        ResetSetPairObserver(0);

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
        // A new source generation invalidates every prior reuse certificate.
        RevokeCertifiedPool("new load generation");

        // A newly observed OnPreLoadMap boundary restores provenance.
        g_unboundAllocatorWarningLogged = false;
        g_lateAttachmentDetected.store(false);
        g_lateAttachmentNotificationPending.store(false);

        ResetAssignmentLedger(loadGeneration);
        ResetSetPairObserver(loadGeneration);

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

        if (g_assignmentLedgerRetainsEntries)
        {
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

        snapshot.assignedEntries =
            g_assignmentLedgerCompactedAssigned +
            g_assignmentLedgerCompactedForeignAssigned;

        snapshot.burnedEntries =
            g_assignmentLedgerCompactedBurned +
            g_assignmentLedgerCompactedForeignBurned;

        if (g_persistentIdSubsystem != nullptr &&
            g_persistentIdSubsystem ==
                g_assignmentLedgerCompactedSubsystem)
        {
            snapshot.currentSubsystemAssigned =
                g_assignmentLedgerCompactedAssigned;
            snapshot.currentSubsystemBurned =
                g_assignmentLedgerCompactedBurned;
            snapshot.foreignSubsystemAssigned =
                g_assignmentLedgerCompactedForeignAssigned;
            snapshot.foreignSubsystemBurned =
                g_assignmentLedgerCompactedForeignBurned;
        }
        else
        {
            // Exact foreign identities are intentionally discarded
            // after activation. If ownership later differs, treat
            // every compacted entry as foreign and remain fail-closed.
            snapshot.foreignSubsystemAssigned =
                snapshot.assignedEntries;
            snapshot.foreignSubsystemBurned =
                snapshot.burnedEntries;
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

    SetPairObserverSnapshot GetSetPairObserverSnapshot()
    {
#ifdef PERSISTENTIDFIX_DIAGNOSTICS
        std::lock_guard<std::mutex> lock(g_setPairObserverMutex);
        return g_setPairObserver;
#else
        return {};
#endif
    }

    void LogSetPairObserver(
        const char* phase)
    {
#ifdef PERSISTENTIDFIX_DIAGNOSTICS
        const SetPairObserverSnapshot snapshot =
            GetSetPairObserverSnapshot();

        LOG_INFO(
            "PersistentIdFix: SetIDHandlePair observer [%s]: generation=%llu calls=%llu true=%llu false=%llu exactBefore=%llu exactAfter=%llu newInsertions=%llu consistentDuplicates=%llu conflicts=%llu maxAdvances=%llu maxAdvanceWithoutExactPair=%llu postStateMismatches=%llu nullArgs=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.loadGeneration),
            static_cast<unsigned long long>(snapshot.totalCalls),
            static_cast<unsigned long long>(snapshot.trueReturns),
            static_cast<unsigned long long>(snapshot.falseReturns),
            static_cast<unsigned long long>(snapshot.exactPairBefore),
            static_cast<unsigned long long>(snapshot.exactPairAfter),
            static_cast<unsigned long long>(snapshot.newInsertions),
            static_cast<unsigned long long>(snapshot.consistentDuplicates),
            static_cast<unsigned long long>(snapshot.conflictingAttempts),
            static_cast<unsigned long long>(snapshot.maxAdvances),
            static_cast<unsigned long long>(snapshot.maxAdvanceWithoutExactPair),
            static_cast<unsigned long long>(snapshot.postStateMismatches),
            static_cast<unsigned long long>(snapshot.nullArgumentCalls));
#else
        (void)phase;
#endif
    }

    HighWaterCrossCheckSnapshot LogHighWaterCrossCheck(
        const char* phase,
        std::uint64_t sourceGeneration,
        bool savedHighWaterCaptured,
        bool savedHighWaterConflict,
        std::uint32_t savedHighWater,
        bool requireExactAccounting)
    {
        HighWaterCrossCheckSnapshot result;
        result.sourceGeneration = sourceGeneration;
        result.savedHighWater = savedHighWater;
        result.exactAccountingRequested = requireExactAccounting;

        const AssignmentLedgerSnapshot ledger =
            GetAssignmentLedgerSnapshot();

        result.ledgerGeneration = ledger.loadGeneration;
        result.currentAssigned = ledger.currentSubsystemAssigned;
        result.currentBurned = ledger.currentSubsystemBurned;
        result.foreignAssigned = ledger.foreignSubsystemAssigned;
        result.foreignBurned = ledger.foreignSubsystemBurned;

        if (!g_sessionActive ||
            g_persistentIdSubsystem == nullptr ||
            !savedHighWaterCaptured ||
            savedHighWaterConflict ||
            sourceGeneration == 0 ||
            ledger.loadGeneration != sourceGeneration)
        {
            LOG_WARN(
                "PersistentIdFix: high-water cross-check [%s]: available=0 sourceGeneration=%llu ledgerGeneration=%llu captured=%u conflict=%u sessionActive=%u subsystem=%p",
                phase != nullptr ? phase : "<null>",
                static_cast<unsigned long long>(sourceGeneration),
                static_cast<unsigned long long>(ledger.loadGeneration),
                savedHighWaterCaptured ? 1u : 0u,
                savedHighWaterConflict ? 1u : 0u,
                g_sessionActive ? 1u : 0u,
                static_cast<void*>(g_persistentIdSubsystem));
            return result;
        }

        result.available = true;
        result.liveHighWater = g_persistentIdSubsystem->MaxID;
        result.restoredAtLeastSaved =
            result.liveHighWater >= result.savedHighWater;

        const std::uint64_t expected =
            static_cast<std::uint64_t>(savedHighWater) +
            ledger.currentSubsystemAssigned +
            ledger.currentSubsystemBurned;

        result.expectedLiveHighWater = expected;
        result.expectedOverflow = expected > UINT32_MAX;

        if (requireExactAccounting)
        {
            result.exactAccountingValid =
                ledger.healthy &&
                ledger.foreignSubsystemAssigned == 0 &&
                ledger.foreignSubsystemBurned == 0 &&
                !result.expectedOverflow;

            if (result.exactAccountingValid)
            {
                result.exactAccountingMatch =
                    result.liveHighWater ==
                    static_cast<std::uint32_t>(expected);
            }
        }

        LOG_INFO(
            "PersistentIdFix: high-water cross-check [%s]: available=%u sourceGeneration=%llu ledgerGeneration=%llu saved=%u live=%u restoredAtLeastSaved=%u exactRequested=%u exactValid=%u exactMatch=%u expected=%llu overflow=%u currentAssigned=%llu currentBurned=%llu foreignAssigned=%llu foreignBurned=%llu ledgerHealthy=%u",
            phase != nullptr ? phase : "<null>",
            result.available ? 1u : 0u,
            static_cast<unsigned long long>(result.sourceGeneration),
            static_cast<unsigned long long>(result.ledgerGeneration),
            result.savedHighWater,
            result.liveHighWater,
            result.restoredAtLeastSaved ? 1u : 0u,
            result.exactAccountingRequested ? 1u : 0u,
            result.exactAccountingValid ? 1u : 0u,
            result.exactAccountingMatch ? 1u : 0u,
            static_cast<unsigned long long>(result.expectedLiveHighWater),
            result.expectedOverflow ? 1u : 0u,
            static_cast<unsigned long long>(result.currentAssigned),
            static_cast<unsigned long long>(result.currentBurned),
            static_cast<unsigned long long>(result.foreignAssigned),
            static_cast<unsigned long long>(result.foreignBurned),
            ledger.healthy ? 1u : 0u);

        if (!result.restoredAtLeastSaved)
        {
            LOG_ERROR(
                "PersistentIdFix: live MaxID is below serialized saved high-water during [%s]: saved=%u live=%u",
                phase != nullptr ? phase : "<null>",
                result.savedHighWater,
                result.liveHighWater);
        }

        if (requireExactAccounting &&
            result.exactAccountingValid &&
            !result.exactAccountingMatch)
        {
            LOG_ERROR(
                "PersistentIdFix: pre-certification MaxID accounting mismatch during [%s]: saved=%u assigned=%llu burned=%llu expected=%llu live=%u",
                phase != nullptr ? phase : "<null>",
                result.savedHighWater,
                static_cast<unsigned long long>(result.currentAssigned),
                static_cast<unsigned long long>(result.currentBurned),
                static_cast<unsigned long long>(result.expectedLiveHighWater),
                result.liveHighWater);
        }

        return result;
    }

    CandidatePoolDiagnostic BuildCandidatePoolDiagnostic(
        std::uint64_t loadGeneration,
        const std::vector<std::uint32_t>& sourceProtectedIds)
    {
        CandidatePoolDiagnostic diagnostic;
        diagnostic.loadGeneration = loadGeneration;

        if (!g_sessionActive ||
            g_persistentIdSubsystem == nullptr ||
            !g_reusePoolReady ||
            !g_subsystemWorldVerified ||
            g_activeGameWorld == nullptr ||
            g_loadedHighWater == 0)
        {
            LOG_WARN(
                "PersistentIdFix: candidate-pool diagnostic unavailable: authoritative session state is incomplete");
            return diagnostic;
        }

        std::vector<std::uint32_t> ledgerProtected;

        {
            std::lock_guard<std::mutex> lock(g_assignmentLedgerMutex);

            if (!g_assignmentLedgerHealthy ||
                g_assignmentLedgerGeneration != loadGeneration ||
                !g_assignmentLedgerRetainsEntries)
            {
                LOG_WARN(
                    "PersistentIdFix: candidate-pool diagnostic unavailable: assignment ledger generation/health/detail mismatch");
                return diagnostic;
            }

            try
            {
                ledgerProtected.reserve(g_assignmentLedger.size());

                for (const LedgerEntry& entry : g_assignmentLedger)
                {
                    if (entry.subsystem == g_persistentIdSubsystem)
                    {
                        ledgerProtected.push_back(entry.id);
                    }
                }
            }
            catch (...)
            {
                LOG_ERROR(
                    "PersistentIdFix: candidate-pool diagnostic failed while copying assignment ledger");
                return diagnostic;
            }
        }

        std::vector<std::uint32_t> blocked;

        try
        {
            blocked.reserve(
                g_loadedHandleIds.size() +
                sourceProtectedIds.size() +
                ledgerProtected.size());

            blocked.insert(
                blocked.end(),
                g_loadedHandleIds.begin(),
                g_loadedHandleIds.end());

            blocked.insert(
                blocked.end(),
                sourceProtectedIds.begin(),
                sourceProtectedIds.end());

            blocked.insert(
                blocked.end(),
                ledgerProtected.begin(),
                ledgerProtected.end());

            blocked.erase(
                std::remove_if(
                    blocked.begin(),
                    blocked.end(),
                    [](std::uint32_t id)
                    {
                        return id == 0 ||
                            id == UINT32_MAX ||
                            id >= g_loadedHighWater;
                    }),
                blocked.end());

            std::sort(blocked.begin(), blocked.end());
            blocked.erase(
                std::unique(blocked.begin(), blocked.end()),
                blocked.end());
        }
        catch (...)
        {
            LOG_ERROR(
                "PersistentIdFix: candidate-pool diagnostic failed while staging blocked IDs");
            return diagnostic;
        }

        std::uint64_t sourceOnlyBlocked = 0;

        for (const std::uint32_t id : sourceProtectedIds)
        {
            if (id == 0 ||
                id == UINT32_MAX ||
                id >= g_loadedHighWater)
            {
                continue;
            }

            if (!std::binary_search(
                    g_loadedHandleIds.begin(),
                    g_loadedHandleIds.end(),
                    id))
            {
                ++sourceOnlyBlocked;
            }
        }

        // sourceProtectedIds is already deduplicated by G1, so the
        // source-only count does not require another uniqueness pass.

        std::uint64_t candidateReusable = 0;
        std::uint64_t candidateRanges = 0;
        std::uint32_t nextId = 1;

        for (const std::uint32_t blockedId : blocked)
        {
            if (nextId < blockedId)
            {
                candidateReusable +=
                    static_cast<std::uint64_t>(blockedId) -
                    static_cast<std::uint64_t>(nextId);
                ++candidateRanges;
            }

            if (blockedId >= nextId)
                nextId = blockedId + 1u;
        }

        if (nextId < g_loadedHighWater)
        {
            candidateReusable +=
                static_cast<std::uint64_t>(g_loadedHighWater) -
                static_cast<std::uint64_t>(nextId);
            ++candidateRanges;
        }

        diagnostic.valid = true;
        diagnostic.loadedHighWater = g_loadedHighWater;
        diagnostic.loadedHandleIds =
            static_cast<std::uint64_t>(g_loadedHandleIds.size());
        diagnostic.sourceProtectedIds =
            static_cast<std::uint64_t>(sourceProtectedIds.size());
        diagnostic.ledgerProtectedIds =
            static_cast<std::uint64_t>(ledgerProtected.size());
        diagnostic.blockedUniqueIds =
            static_cast<std::uint64_t>(blocked.size());
        diagnostic.sourceOnlyBlockedIds = sourceOnlyBlocked;
        diagnostic.candidateReusableIds = candidateReusable;
        diagnostic.candidateRanges = candidateRanges;
        diagnostic.legacyReusableIds =
            g_persistentIdReuse.GetReusableIDCount();
        diagnostic.legacyRanges =
            static_cast<std::uint64_t>(
                g_persistentIdReuse.GetRangeCount());

        if (!g_persistentIdReuse.StagePoolFromBlockedIds(
                blocked,
                g_loadedHighWater))
        {
            LOG_ERROR(
                "PersistentIdFix: staged certified pool construction failed");
            g_persistentIdReuse.ClearStagedPool();
            diagnostic.valid = false;
            return diagnostic;
        }

        diagnostic.stagedReusableIds =
            g_persistentIdReuse.GetStagedReusableIDCount();
        diagnostic.stagedRanges =
            static_cast<std::uint64_t>(
                g_persistentIdReuse.GetStagedRangeCount());

        diagnostic.stagedHasFirstRange =
            g_persistentIdReuse.GetStagedFirstRange(
                diagnostic.stagedFirst,
                diagnostic.stagedFirstLast);

        diagnostic.stagedHasLastRange =
            g_persistentIdReuse.GetStagedLastRange(
                diagnostic.stagedLastFirst,
                diagnostic.stagedLast);

        if (diagnostic.stagedReusableIds !=
                diagnostic.candidateReusableIds ||
            diagnostic.stagedRanges !=
                diagnostic.candidateRanges)
        {
            LOG_ERROR(
                "PersistentIdFix: staged certified pool disagrees with candidate arithmetic: stagedReusable=%llu candidateReusable=%llu stagedRanges=%llu candidateRanges=%llu",
                static_cast<unsigned long long>(diagnostic.stagedReusableIds),
                static_cast<unsigned long long>(diagnostic.candidateReusableIds),
                static_cast<unsigned long long>(diagnostic.stagedRanges),
                static_cast<unsigned long long>(diagnostic.candidateRanges));

            g_persistentIdReuse.ClearStagedPool();
            diagnostic.valid = false;
            return diagnostic;
        }

        LOG_INFO(
            "PersistentIdFix: candidate pool diagnostic: generation=%llu highWater=%u loadedH=%llu sourceProtected=%llu ledgerProtected=%llu blockedUnique=%llu sourceOnlyBlocked=%llu candidateReusable=%llu candidateRanges=%llu legacyReusable=%llu legacyRanges=%llu deltaReusable=%lld",
            static_cast<unsigned long long>(diagnostic.loadGeneration),
            diagnostic.loadedHighWater,
            static_cast<unsigned long long>(diagnostic.loadedHandleIds),
            static_cast<unsigned long long>(diagnostic.sourceProtectedIds),
            static_cast<unsigned long long>(diagnostic.ledgerProtectedIds),
            static_cast<unsigned long long>(diagnostic.blockedUniqueIds),
            static_cast<unsigned long long>(diagnostic.sourceOnlyBlockedIds),
            static_cast<unsigned long long>(diagnostic.candidateReusableIds),
            static_cast<unsigned long long>(diagnostic.candidateRanges),
            static_cast<unsigned long long>(diagnostic.legacyReusableIds),
            static_cast<unsigned long long>(diagnostic.legacyRanges),
            static_cast<long long>(diagnostic.legacyReusableIds) -
                static_cast<long long>(diagnostic.candidateReusableIds));

        LOG_INFO(
            "PersistentIdFix: staged certified pool: generation=%llu reusable=%llu ranges=%llu first=%u-%u hasFirst=%u last=%u-%u hasLast=%u activeLegacyReusable=%llu activeLegacyRanges=%llu",
            static_cast<unsigned long long>(diagnostic.loadGeneration),
            static_cast<unsigned long long>(diagnostic.stagedReusableIds),
            static_cast<unsigned long long>(diagnostic.stagedRanges),
            diagnostic.stagedFirst,
            diagnostic.stagedFirstLast,
            diagnostic.stagedHasFirstRange ? 1u : 0u,
            diagnostic.stagedLastFirst,
            diagnostic.stagedLast,
            diagnostic.stagedHasLastRange ? 1u : 0u,
            static_cast<unsigned long long>(diagnostic.legacyReusableIds),
            static_cast<unsigned long long>(diagnostic.legacyRanges));

        return diagnostic;
    }

    bool ActivateCertifiedPool(
        std::uint64_t loadGeneration)
    {
        if (!g_sessionActive ||
            !g_reusePoolReady ||
            g_persistentIdSubsystem == nullptr ||
            !g_subsystemWorldVerified ||
            g_activeGameWorld == nullptr ||
            g_loadedHighWater == 0 ||
            loadGeneration == 0)
        {
            LOG_WARN(
                "PersistentIdFix: certified pool activation rejected: session state is incomplete");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_assignmentLedgerMutex);

            if (!g_assignmentLedgerHealthy ||
                g_assignmentLedgerGeneration != loadGeneration ||
                !g_assignmentLedgerRetainsEntries)
            {
                LOG_WARN(
                    "PersistentIdFix: certified pool activation rejected: assignment ledger generation/health/detail mismatch");
                return false;
            }

            for (const LedgerEntry& entry : g_assignmentLedger)
            {
                if (entry.subsystem != g_persistentIdSubsystem)
                {
                    LOG_WARN(
                        "PersistentIdFix: certified pool activation rejected: foreign subsystem ledger entry is present");
                    return false;
                }
            }

            if (!g_persistentIdReuse.PromoteStagedPool())
            {
                LOG_ERROR(
                    "PersistentIdFix: certified pool activation failed: no valid staged pool was available");
                return false;
            }

            CompactAssignmentLedgerLocked();
            g_certifiedPoolActive = true;
        }

        LOG_INFO(
            "PersistentIdFix: certified reusable pool activated: generation=%llu reusable=%llu ranges=%llu",
            static_cast<unsigned long long>(loadGeneration),
            static_cast<unsigned long long>(
                g_persistentIdReuse.GetReusableIDCount()),
            static_cast<unsigned long long>(
                g_persistentIdReuse.GetRangeCount()));

        return true;
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

    void BeginGameWorld(SDK::UWorld* world)
    {
        RevokeCertifiedPool("gameplay world binding changed");

        g_activeGameWorld = world;
        g_subsystemWorldVerified = false;

        if (world == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: BeginGameWorld received null world; reuse remains fail-closed");
            return;
        }

        if (g_persistentIdSubsystem == nullptr)
        {
            LOG_INFO(
                "PersistentIdFix: gameplay world observed: world=%p allocator not yet bound",
                static_cast<void*>(world));
            return;
        }

        void* outer = ReadUObjectOuter(g_persistentIdSubsystem);
        g_subsystemWorldVerified = outer == world;

        if (g_subsystemWorldVerified)
        {
            LOG_INFO(
                "PersistentIdFix: allocator/world ownership verified: subsystem=%p outer=%p world=%p",
                static_cast<void*>(g_persistentIdSubsystem),
                outer,
                static_cast<void*>(world));
        }
        else
        {
            LOG_ERROR(
                "PersistentIdFix: allocator/world ownership mismatch: subsystem=%p outer=%p world=%p; reuse remains fail-closed",
                static_cast<void*>(g_persistentIdSubsystem),
                outer,
                static_cast<void*>(world));
        }
    }

    void EndGameWorld(SDK::UWorld* world)
    {
        if (world != nullptr && g_activeGameWorld == world)
        {
            LOG_INFO(
                "PersistentIdFix: gameplay world binding ended: world=%p",
                static_cast<void*>(world));
            g_activeGameWorld = nullptr;
            g_subsystemWorldVerified = false;
        }
    }

    void OnSaveLoaded()
    {
        if (!InitializeSession(nullptr))
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
        g_loadedHandleIds.clear();
        g_loadedHighWater = 0;

        g_persistentIdReuse.Clear();
        g_reusePoolReady = false;
        g_certifiedPoolActive = false;

        PersistentIdFixStats::Reset();

        g_sessionNetMode = EPluginNetMode::Unknown;
    }

    bool IsSessionActive()
    {
        return g_sessionActive;
    }

    void MarkLateAttachmentDetected()
    {
        g_lateAttachmentDetected.store(true);
        g_lateAttachmentNotificationPending.store(true);
    }

    bool ConsumeLateAttachmentNotification()
    {
        return g_lateAttachmentNotificationPending.exchange(false);
    }

    bool IsLateAttachmentDetected()
    {
        return g_lateAttachmentDetected.load();
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

#ifdef PERSISTENTIDFIX_DIAGNOSTICS
    bool SetIDHandlePairDetour(
        void* subsystem,
        SDK::FCrMassPersistentEntityID* persistentId,
        SDK::FMassEntityHandle handle)
    {
        if (g_setIDHandlePair == nullptr)
        {
            LOG_ERROR(
                "PersistentIdFix: native SetIDHandlePair trampoline is unavailable");
            return false;
        }

        auto* persistentIDSubsystem =
            static_cast<SDK::UCrMassPersistentIDSubsystem*>(subsystem);

        if (persistentIDSubsystem == nullptr || persistentId == nullptr)
        {
            {
                std::lock_guard<std::mutex> lock(g_setPairObserverMutex);
                ++g_setPairObserver.totalCalls;
                ++g_setPairObserver.nullArgumentCalls;
            }

            return g_setIDHandlePair(
                subsystem,
                persistentId,
                handle);
        }

        const std::uint32_t id = persistentId->ID;
        const std::uint32_t maxBefore = persistentIDSubsystem->MaxID;
        const SetPairState before =
            InspectSetPairState(
                persistentIDSubsystem,
                id,
                handle);

        const bool result =
            g_setIDHandlePair(
                subsystem,
                persistentId,
                handle);

        const std::uint32_t maxAfter = persistentIDSubsystem->MaxID;
        const SetPairState after =
            InspectSetPairState(
                persistentIDSubsystem,
                id,
                handle);

        const bool maxAdvanced = maxAfter > maxBefore;
        const bool newInsertion =
            result && !before.exactPair && after.exactPair;
        const bool consistentDuplicate =
            !result && before.exactPair && after.exactPair;
        const bool conflictingAttempt =
            !result && !before.exactPair && !after.exactPair &&
            (before.pidPresent || before.handlePresent);
        const bool postStateMismatch =
            result && !after.exactPair;

        {
            std::lock_guard<std::mutex> lock(g_setPairObserverMutex);
            ++g_setPairObserver.totalCalls;

            if (result) ++g_setPairObserver.trueReturns;
            else ++g_setPairObserver.falseReturns;
            if (before.exactPair) ++g_setPairObserver.exactPairBefore;
            if (after.exactPair) ++g_setPairObserver.exactPairAfter;
            if (newInsertion) ++g_setPairObserver.newInsertions;
            if (consistentDuplicate) ++g_setPairObserver.consistentDuplicates;
            if (conflictingAttempt) ++g_setPairObserver.conflictingAttempts;
            if (maxAdvanced) ++g_setPairObserver.maxAdvances;
            if (maxAdvanced && !after.exactPair)
                ++g_setPairObserver.maxAdvanceWithoutExactPair;
            if (postStateMismatch)
                ++g_setPairObserver.postStateMismatches;
        }

        if (maxAdvanced && !after.exactPair)
        {
            LOG_WARN(
                "PersistentIdFix: SetIDHandlePair advanced MaxID without establishing exact pair: id=%u handle=(%u,%u) MaxID=%u->%u result=%u pidPresentBefore=%u handlePresentBefore=%u",
                id,
                handle.Index,
                handle.SerialNumber,
                maxBefore,
                maxAfter,
                result ? 1u : 0u,
                before.pidPresent ? 1u : 0u,
                before.handlePresent ? 1u : 0u);
        }

        if (postStateMismatch)
        {
            LOG_ERROR(
                "PersistentIdFix: SetIDHandlePair returned true without exact bidirectional pair: id=%u handle=(%u,%u)",
                id,
                handle.Index,
                handle.SerialNumber);
        }

        return result;
    }

#endif

    SDK::FCrMassPersistentEntityID* GetOrAddIDForHandleDetour(
        void* subsystem,
        SDK::FCrMassPersistentEntityID* returnValue,
        SDK::FMassEntityHandle handle)
    {
        if (returnValue == nullptr)
        {
            FatalAllocationFailure(
                AllocationFailureReason::NullReturnBuffer,
                subsystem,
                handle,
                UINT32_MAX,
                0);
        }

        *returnValue = {};
        returnValue->ID = UINT32_MAX;

        if (subsystem == nullptr)
        {
            FatalAllocationFailure(
                AllocationFailureReason::NullSubsystem,
                nullptr,
                handle,
                UINT32_MAX,
                0);
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
                FatalAllocationFailure(
                    AllocationFailureReason::NativeAllocatorUnavailable,
                    subsystem,
                    handle,
                    UINT32_MAX,
                    0);
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
            if (!IsExactPersistentIdMapping(
                    persistentIDSubsystem,
                    existingId->ID,
                    handle))
            {
                FatalAllocationFailure(
                    AllocationFailureReason::InvalidExistingMapping,
                    subsystem,
                    handle,
                    existingId->ID,
                    persistentIDSubsystem->MaxID);
            }

            *returnValue = *existingId;
            return returnValue;
        }

        if (!g_sessionActive &&
            g_activeGameWorld == nullptr &&
            !g_unboundAllocatorWarningLogged &&
            (persistentIDSubsystem->MaxID != 0 ||
                persistentIDSubsystem->IDHandleMap.Num() != 0))
        {
            g_unboundAllocatorWarningLogged = true;
            g_lateAttachmentDetected.store(true);
            g_lateAttachmentNotificationPending.store(true);
            LOG_WARN(
                "PersistentIdFix: allocator activity observed without an OnWorldBeginPlay ownership boundary; treating this as late/hot attachment and keeping reuse fail-closed: subsystem=%p outer=%p MaxID=%u entities=%d",
                static_cast<void*>(persistentIDSubsystem),
                ReadUObjectOuter(persistentIDSubsystem),
                persistentIDSubsystem->MaxID,
                persistentIDSubsystem->IDHandleMap.Num());
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
            OnNewGame(persistentIDSubsystem);
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
#ifdef PERSISTENTIDFIX_DIAGNOSTICS
        if (!g_reusePoolReady)
        {
            LOG_INFO(
                "PersistentIdFix: GetOrAddIDForHandle before reuse pool was built; "
                "using bounded monotonic allocation: handle=(%u,%u) MaxID=%u",
                handle.Index,
                handle.SerialNumber,
                persistentIDSubsystem->MaxID);
        }
#endif

        /*
         * No existing mapping.
         *
         * Loaded-save holes are consumed only after the comprehensive
         * source certificate has promoted the staged pool. Before that
         * point, allocation deliberately falls through to bounded
         * monotonic IDs. This removes the legacy unsafe-reuse window.
         *
         * A brand-new game also remains monotonic-only for its first
         * session: it begins with no pre-existing holes, and same-session
         * recycling is intentionally prohibited.
         */
        if (g_certifiedPoolActive)
        {
            const bool certifiedOwnershipValid =
                g_sessionActive &&
                g_persistentIdSubsystem != nullptr &&
                persistentIDSubsystem == g_persistentIdSubsystem &&
                g_persistentIdReuse.IsInitializedFor(
                    persistentIDSubsystem) &&
                g_activeGameWorld != nullptr &&
                g_subsystemWorldVerified &&
                ReadUObjectOuter(persistentIDSubsystem) ==
                    g_activeGameWorld;

            if (!certifiedOwnershipValid)
            {
                LOG_ERROR(
                    "PersistentIdFix: certified reuse ownership check failed at allocation time; revoking reuse: caller=%p certifiedSubsystem=%p callerOuter=%p activeWorld=%p worldVerified=%u sessionActive=%u",
                    static_cast<void*>(persistentIDSubsystem),
                    static_cast<void*>(g_persistentIdSubsystem),
                    ReadUObjectOuter(persistentIDSubsystem),
                    static_cast<void*>(g_activeGameWorld),
                    g_subsystemWorldVerified ? 1u : 0u,
                    g_sessionActive ? 1u : 0u);

                RevokeCertifiedPool("allocation-time ownership mismatch");
            }
        }

        if (g_certifiedPoolActive)
        {
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
                FatalAllocationFailure(
                    AllocationFailureReason::RecycledAllocationFailed,
                    subsystem,
                    handle,
                    UINT32_MAX,
                    persistentIDSubsystem->MaxID);
            }
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
            FatalAllocationFailure(
                AllocationFailureReason::PersistentIdSpaceExhausted,
                subsystem,
                handle,
                UINT32_MAX,
                persistentIDSubsystem->MaxID);
        }

        if (g_setIDHandlePair == nullptr)
        {
            FatalAllocationFailure(
                AllocationFailureReason::NativeSetterUnavailable,
                subsystem,
                handle,
                UINT32_MAX,
                persistentIDSubsystem->MaxID);
        }

        ++persistentIDSubsystem->MaxID;

        SDK::FCrMassPersistentEntityID persistentId{};

        persistentId.ID =
            persistentIDSubsystem->MaxID;

        persistentId.CachedHandle =
            handle;

        CheckedSetIDHandlePair(
            persistentIDSubsystem,
            &persistentId,
            handle);

        *returnValue = persistentId;

        RecordAssignedId(
            persistentIDSubsystem,
            persistentId.ID);

        PersistentIdFixStats::RecordAssignment();

        return returnValue;
    }
}

#include "SourceReadObserver.h"

#include "MassFragmentClassifier.h"
#include "IndependentSourceCollector.h"
#include "SDK/Chimera_structs.hpp"

#include "plugin_helpers.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <shared_mutex>

namespace
{
    // Audited Windows x64 FString/TArray layout used only to snapshot the
    // caller-owned section name before UCrSaveSubsystem::GetSaveData runs.
    struct FStringView
    {
        const wchar_t* Data;
        std::int32_t Num;
        std::int32_t Max;
    };

    static_assert(sizeof(FStringView) == 0x10);

    constexpr std::size_t kDiagnosticSectionNameCapacity = 96;

    // Holds a bounded stack copy of an FString section name for classification
    // and failure diagnostics after the native callee destroys its argument.
    struct CapturedSectionName
    {
        wchar_t Text[kDiagnosticSectionNameCapacity]{};
        std::int32_t Length = 0;
        bool Valid = false;
    };

    std::atomic<std::uint64_t> g_loadGeneration{ 0 };
    std::atomic<std::uint64_t> g_inventoryGeneration{ 0 };
    std::atomic<bool> g_gameWorldAttached{ false };
    std::atomic<std::uint64_t> g_attachedLoadGeneration{ 0 };

    struct SavedHighWaterState
    {
        bool captured = false;
        bool conflict = false;
        std::uint64_t generation = 0;
        std::uint64_t captures = 0;
        std::uint32_t value = 0;
    };

    std::mutex g_savedHighWaterMutex;
    SavedHighWaterState g_savedHighWater;

    std::atomic<bool> g_inventoryCaptured{ false };
    std::atomic<bool> g_inventoryValid{ false };
    std::atomic<std::uint64_t> g_inventoryEntries{ 0 };
    std::atomic<std::uint64_t> g_inventoryRecognized{ 0 };
    std::atomic<bool> g_inventoryMass{ false };
    std::atomic<bool> g_inventoryBuildingCustomNames{ false };
    std::atomic<bool> g_inventoryGameStateData{ false };
    std::atomic<bool> g_inventoryAntennasData{ false };
    std::atomic<bool> g_inventoryZiplineReplicator{ false };
    std::atomic<bool> g_inventoryZiplineSubsystem{ false };
    std::atomic<bool> g_inventoryBaseCoreReplicationHelper{ false };
#if defined(MODLOADER_CLIENT_BUILD)
    std::atomic<bool> g_inventoryCrPackageTransportReplicator{ false };
    std::atomic<bool> g_inventoryStorageShareableSubsystemData{ false };
    std::atomic<bool> g_inventoryBaseSiteAttackData{ false };
#endif

    std::atomic<std::uint64_t> g_successfulReads{ 0 };
    std::atomic<std::uint64_t> g_recognizedSuccessfulReads{ 0 };
    std::atomic<std::uint64_t> g_failedReads{ 0 };

    std::atomic<std::uint64_t> g_massReads{ 0 };
    std::atomic<std::uint64_t> g_massReadFailures{ 0 };
    std::atomic<std::uint64_t> g_buildingCustomNameReads{ 0 };
    std::atomic<std::uint64_t> g_gameStateDataReads{ 0 };
    std::atomic<std::uint64_t> g_antennasDataReads{ 0 };
    std::atomic<std::uint64_t> g_ziplineReplicatorReads{ 0 };
    std::atomic<std::uint64_t> g_ziplineSubsystemReads{ 0 };
    std::atomic<std::uint64_t> g_baseCoreReplicationHelperReads{ 0 };
#if defined(MODLOADER_CLIENT_BUILD)
    std::atomic<std::uint64_t> g_crPackageTransportReplicatorReads{ 0 };
    std::atomic<std::uint64_t> g_crPackageTransportReplicatorFailedReads{ 0 };
    std::atomic<std::uint64_t> g_storageShareableSubsystemDataReads{ 0 };
    std::atomic<std::uint64_t> g_storageShareableSubsystemDataFailedReads{ 0 };
    std::atomic<std::uint64_t> g_baseSiteAttackDataReads{ 0 };
    std::atomic<std::uint64_t> g_baseSiteAttackDataFailedReads{ 0 };
#endif

    // Monotonic activity counters allow publication to prove that no
    // GetSaveData observer frame was active or entered/exited while
    // plugin-owned source vectors were being copied.
    std::atomic<std::uint64_t> g_activeObserverFrames{ 0 };
    std::atomic<std::uint64_t> g_observerFrameTransitions{ 0 };

    // Formal source/publication exclusion gate.
    //
    // Observer frames take a shared lock for their entire native-read
    // and collector-update lifetime. Publication and generation reset
    // take the exclusive lock. Therefore collector publication cannot
    // overlap a source read or collector reset.
    std::shared_mutex g_sourcePublicationGate;

    // Publication is initiated from the game thread. Keep explicit
    // state so accidental nested/unpaired freeze requests fail closed.
    std::mutex g_publicationFreezeStateMutex;
    bool g_publicationFreezeHeld = false;

    struct ObserverFrameGuard
    {
        ObserverFrameGuard()
        {
            g_observerFrameTransitions.fetch_add(
                1,
                std::memory_order_seq_cst);
            g_activeObserverFrames.fetch_add(
                1,
                std::memory_order_seq_cst);
        }

        ~ObserverFrameGuard()
        {
            g_activeObserverFrames.fetch_sub(
                1,
                std::memory_order_seq_cst);
            g_observerFrameTransitions.fetch_add(
                1,
                std::memory_order_seq_cst);
        }
    };

    void ResetObservationCounters()
    {
        {
            std::lock_guard<std::mutex> lock(g_savedHighWaterMutex);
            g_savedHighWater = {};
        }

        g_inventoryCaptured.store(false, std::memory_order_relaxed);
        g_inventoryValid.store(false, std::memory_order_relaxed);
        g_inventoryGeneration.store(0, std::memory_order_relaxed);
        g_inventoryEntries.store(0, std::memory_order_relaxed);
        g_inventoryRecognized.store(0, std::memory_order_relaxed);
        g_inventoryMass.store(false, std::memory_order_relaxed);
        g_inventoryBuildingCustomNames.store(false, std::memory_order_relaxed);
        g_inventoryGameStateData.store(false, std::memory_order_relaxed);
        g_inventoryAntennasData.store(false, std::memory_order_relaxed);
        g_inventoryZiplineReplicator.store(false, std::memory_order_relaxed);
        g_inventoryZiplineSubsystem.store(false, std::memory_order_relaxed);
        g_inventoryBaseCoreReplicationHelper.store(false, std::memory_order_relaxed);
#if defined(MODLOADER_CLIENT_BUILD)
        g_inventoryCrPackageTransportReplicator.store(false, std::memory_order_relaxed);
        g_inventoryStorageShareableSubsystemData.store(false, std::memory_order_relaxed);
        g_inventoryBaseSiteAttackData.store(false, std::memory_order_relaxed);
#endif

        g_successfulReads.store(0, std::memory_order_relaxed);
        g_recognizedSuccessfulReads.store(0, std::memory_order_relaxed);
        g_failedReads.store(0, std::memory_order_relaxed);

        g_massReads.store(0, std::memory_order_relaxed);
        g_massReadFailures.store(0, std::memory_order_relaxed);
        g_buildingCustomNameReads.store(0, std::memory_order_relaxed);
        g_gameStateDataReads.store(0, std::memory_order_relaxed);
        g_antennasDataReads.store(0, std::memory_order_relaxed);
        g_ziplineReplicatorReads.store(0, std::memory_order_relaxed);
        g_ziplineSubsystemReads.store(0, std::memory_order_relaxed);
        g_baseCoreReplicationHelperReads.store(0, std::memory_order_relaxed);
#if defined(MODLOADER_CLIENT_BUILD)
        g_crPackageTransportReplicatorReads.store(0, std::memory_order_relaxed);
        g_crPackageTransportReplicatorFailedReads.store(0, std::memory_order_relaxed);
        g_storageShareableSubsystemDataReads.store(0, std::memory_order_relaxed);
        g_storageShareableSubsystemDataFailedReads.store(0, std::memory_order_relaxed);
        g_baseSiteAttackDataReads.store(0, std::memory_order_relaxed);
        g_baseSiteAttackDataFailedReads.store(0, std::memory_order_relaxed);
#endif

        PersistentIdFixMassFragmentClassifier::Reset();
        PersistentIdFixIndependentSourceCollector::Reset();
    }

    bool CaptureSavedHighWater(
        const SDK::FCrMassSaveData* massSaveData)
    {
        const std::uint64_t generation =
            g_loadGeneration.load(std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(g_savedHighWaterMutex);

        ++g_savedHighWater.captures;

        if (massSaveData == nullptr || generation == 0)
        {
            g_savedHighWater.conflict = true;
            return false;
        }

        const std::uint32_t value =
            massSaveData->MaxPersistentID;

        if (!g_savedHighWater.captured)
        {
            g_savedHighWater.captured = true;
            g_savedHighWater.generation = generation;
            g_savedHighWater.value = value;
            return true;
        }

        if (g_savedHighWater.generation != generation ||
            g_savedHighWater.value != value)
        {
            g_savedHighWater.conflict = true;
            return false;
        }

        return true;
    }

    bool EqualsSection(
        const FStringView& value,
        const wchar_t* expected)
    {
        if (value.Data == nullptr ||
            value.Num <= 0 ||
            value.Max < value.Num ||
            expected == nullptr)
        {
            return false;
        }

        const std::size_t expectedLength =
            std::wcslen(expected);

        // FString Num includes the trailing null terminator for these section
        // names. Require that exact shape rather than scanning arbitrary memory.
        if (static_cast<std::size_t>(value.Num) !=
            expectedLength + 1u)
        {
            return false;
        }

        if (value.Data[expectedLength] != L'\0')
            return false;

        return std::wmemcmp(
            value.Data,
            expected,
            expectedLength) == 0;
    }

    bool CaptureInventory(
        const void* saveSubsystem)
    {
        if (saveSubsystem == nullptr)
            return false;

        // Audited Hotfix 0.3.5 layout:
        //   UCrSaveSubsystem::SaveData = +0x30
        //   FCrSaveGameData::ItemData = +0x00
        //
        // PostLoadGameFile assigns the incoming FCrSaveGameData into
        // this retained field before OnPreLoadMap begins the map
        // transition. Copy only section-name presence into plugin-owned
        // atomics; never retain FString or JSON-wrapper pointers.
        const auto* saveData =
            reinterpret_cast<const SDK::FCrSaveGameData*>(
                static_cast<const std::byte*>(saveSubsystem) + 0x30);

        const auto& itemData = saveData->ItemData;

        const std::int32_t live = itemData.Num();
        const std::int32_t allocated = itemData.NumAllocated();
        const std::int32_t capacity = itemData.Max();

        if (live < 0 ||
            allocated < 0 ||
            capacity < 0 ||
            live > allocated ||
            allocated > capacity)
        {
            return false;
        }

        const auto& flags = itemData.GetAllocationFlags();

        if (flags.Num() != allocated ||
            flags.Num() < 0 ||
            flags.Max() < flags.Num())
        {
            return false;
        }

        if (allocated > 0 && flags.GetData() == nullptr)
            return false;

        bool mass = false;
        bool customNames = false;
        bool gameState = false;
        bool antennas = false;
        bool ziplineReplicator = false;
        bool ziplineSubsystem = false;
        bool baseCore = false;
#if defined(MODLOADER_CLIENT_BUILD)
        bool crPackageTransportReplicator = false;
        bool storageShareableSubsystemData = false;
        bool baseSiteAttackData = false;
#endif

        std::uint64_t recognized = 0;
        std::int32_t visited = 0;

        for (auto it = begin(itemData); it != end(itemData); ++it)
        {
            ++visited;

            if (visited > live)
                return false;

            const auto& key = it->Key();
            const auto& view =
                *reinterpret_cast<const FStringView*>(&key);

            auto mark =
                [&](bool& value) -> bool
                {
                    if (value)
                        return false;

                    value = true;
                    ++recognized;
                    return true;
                };

            if (EqualsSection(view, L"Mass"))
            {
                if (!mark(mass))
                    return false;
            }
            else if (EqualsSection(
                view,
                L"CrBuildingCustomNameSubsystem"))
            {
                if (!mark(customNames))
                    return false;
            }
            else if (EqualsSection(view, L"GameStateData"))
            {
                if (!mark(gameState))
                    return false;
            }
            else if (EqualsSection(view, L"AntennasData"))
            {
                if (!mark(antennas))
                    return false;
            }
            else if (EqualsSection(view, L"CrZiplineReplicator"))
            {
                if (!mark(ziplineReplicator))
                    return false;
            }
            else if (EqualsSection(view, L"CrZiplineSubsystem"))
            {
                if (!mark(ziplineSubsystem))
                    return false;
            }
            else if (EqualsSection(
                view,
                L"BaseCoreReplicationHelperSaveData"))
            {
                if (!mark(baseCore))
                    return false;
            }
#if defined(MODLOADER_CLIENT_BUILD)
            else if (EqualsSection(view, L"CrPackageTransportReplicator"))
            {
                if (!mark(crPackageTransportReplicator))
                    return false;
            }
            else if (EqualsSection(view, L"StorageShareableSubsystemData"))
            {
                if (!mark(storageShareableSubsystemData))
                    return false;
            }
            else if (EqualsSection(view, L"BaseSiteAttackData"))
            {
                if (!mark(baseSiteAttackData))
                    return false;
            }
#endif
        }

        if (visited != live)
            return false;

        g_inventoryEntries.store(
            static_cast<std::uint64_t>(live),
            std::memory_order_relaxed);
        g_inventoryRecognized.store(recognized, std::memory_order_relaxed);
        g_inventoryMass.store(mass, std::memory_order_relaxed);
        g_inventoryBuildingCustomNames.store(customNames, std::memory_order_relaxed);
        g_inventoryGameStateData.store(gameState, std::memory_order_relaxed);
        g_inventoryAntennasData.store(antennas, std::memory_order_relaxed);
        g_inventoryZiplineReplicator.store(ziplineReplicator, std::memory_order_relaxed);
        g_inventoryZiplineSubsystem.store(ziplineSubsystem, std::memory_order_relaxed);
        g_inventoryBaseCoreReplicationHelper.store(baseCore, std::memory_order_relaxed);
#if defined(MODLOADER_CLIENT_BUILD)
        g_inventoryCrPackageTransportReplicator.store(
            crPackageTransportReplicator,
            std::memory_order_relaxed);
        g_inventoryStorageShareableSubsystemData.store(
            storageShareableSubsystemData,
            std::memory_order_relaxed);
        g_inventoryBaseSiteAttackData.store(
            baseSiteAttackData,
            std::memory_order_relaxed);
#endif

        return true;
    }

    // Copies the bounded by-value FString from sectionName into caller-owned
    // storage and returns its source classification, or Unknown if invalid.
    PersistentIdFixSourceReadObserver::SourceSection CaptureSection(
        const void* sectionName,
        CapturedSectionName* capturedName)
    {
        using PersistentIdFixSourceReadObserver::SourceSection;

        if (sectionName == nullptr || capturedName == nullptr)
            return SourceSection::Unknown;

        capturedName->Text[0] = L'\0';
        capturedName->Length = 0;
        capturedName->Valid = false;

        // The native ABI passes FString by value, so validate and copy its
        // bounded contents before the callee can destroy its storage.
        __try
        {
            const auto& sourceName =
                *static_cast<const FStringView*>(sectionName);

            if (sourceName.Data == nullptr ||
                sourceName.Num <= 0 ||
                sourceName.Max < sourceName.Num ||
                static_cast<std::size_t>(sourceName.Num) >
                    kDiagnosticSectionNameCapacity)
            {
                return SourceSection::Unknown;
            }

            const std::int32_t length = sourceName.Num - 1;

            if (sourceName.Data[length] != L'\0')
                return SourceSection::Unknown;

            for (std::int32_t index = 0; index < length; ++index)
            {
                const wchar_t character = sourceName.Data[index];

                if (character == L'\0')
                    return SourceSection::Unknown;

                capturedName->Text[index] = character;
            }

            capturedName->Text[length] = L'\0';
            capturedName->Length = length;
            capturedName->Valid = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            capturedName->Valid = false;
            capturedName->Length = 0;
            capturedName->Text[0] = L'\0';
            return SourceSection::Unknown;
        }

        if (!capturedName->Valid)
            return SourceSection::Unknown;

        const FStringView value{
            capturedName->Text,
            capturedName->Length + 1,
            capturedName->Length + 1 };

        if (EqualsSection(value, L"Mass"))
            return SourceSection::Mass;

        if (EqualsSection(
            value,
            L"CrBuildingCustomNameSubsystem"))
        {
            return SourceSection::BuildingCustomNames;
        }

        if (EqualsSection(value, L"GameStateData"))
            return SourceSection::GameStateData;

        if (EqualsSection(value, L"AntennasData"))
            return SourceSection::AntennasData;

        if (EqualsSection(value, L"CrZiplineReplicator"))
            return SourceSection::ZiplineReplicator;

        if (EqualsSection(value, L"CrZiplineSubsystem"))
            return SourceSection::ZiplineSubsystem;

        if (EqualsSection(
            value,
            L"BaseCoreReplicationHelperSaveData"))
        {
            return SourceSection::BaseCoreReplicationHelper;
        }

#if defined(MODLOADER_CLIENT_BUILD)
        if (EqualsSection(value, L"CrPackageTransportReplicator"))
            return SourceSection::CrPackageTransportReplicator;

        if (EqualsSection(value, L"StorageShareableSubsystemData"))
            return SourceSection::StorageShareableSubsystemData;

        if (EqualsSection(value, L"BaseSiteAttackData"))
            return SourceSection::BaseSiteAttackData;
#endif

        return SourceSection::Unknown;
    }

    bool IndependentSectionComplete(
        const PersistentIdFixIndependentSourceCollector::SectionSnapshot& section)
    {
        return
            section.attempts > 0 &&
            section.successes == section.attempts &&
            section.readFailures == 0 &&
            section.failures == 0 &&
            section.schemaMismatches == 0 &&
            section.containerFailures == 0 &&
            section.incompleteCoverage == 0;
    }

    bool MassSectionComplete(
        const PersistentIdFixMassFragmentClassifier::ClassificationSnapshot& mass)
    {
        return
            mass.classifiedPayloads == mass.massPayloads &&
            mass.malformedPayloads == 0 &&
            mass.unsupportedPayloads == 0 &&
            mass.schemaMismatches == 0 &&
            mass.massDescriptorMismatches == 0 &&
            mass.entityCountMismatches == 0 &&
            mass.semanticFailures == 0 &&
            mass.semanticContainerFailures == 0 &&
            mass.tagMalformed == 0 &&
            mass.tagUnsupported == 0 &&
            mass.tagSchemaMismatches == 0 &&
            mass.tagContainerFailures == 0 &&
            mass.identityAttempts > 0 &&
            mass.identitySuccesses == mass.identityAttempts &&
            mass.identityFailures == 0 &&
            mass.identityContainerFailures == 0 &&
            mass.massRemainderAttempts > 0 &&
            mass.massRemainderSuccesses == mass.massRemainderAttempts &&
            mass.massRemainderFailures == 0 &&
            mass.massRemainderContainerFailures == 0;
    }

    PersistentIdFixSourceReadObserver::CertificationState CertifyIndependent(
        bool inventoryCaptured,
        bool inventoryValid,
        bool inventoryPresent,
        std::uint64_t observedReads,
        const PersistentIdFixIndependentSourceCollector::SectionSnapshot& section)
    {
        using PersistentIdFixSourceReadObserver::CertificationState;

        if (!inventoryCaptured || !inventoryValid)
            return CertificationState::InventoryUnavailable;

        // Absence is deliberately not certified safe yet. Until a
        // version/mode-specific omission policy is established, a
        // missing protected-source section remains fail-closed.
        if (!inventoryPresent)
            return CertificationState::AbsentUnsupported;

        if (section.readFailures != 0 ||
            section.failures != 0 ||
            section.schemaMismatches != 0 ||
            section.containerFailures != 0 ||
            section.incompleteCoverage != 0)
        {
            return CertificationState::Failed;
        }

        if (observedReads == 0 || section.attempts == 0)
            return CertificationState::Pending;

        return IndependentSectionComplete(section)
            ? CertificationState::Certified
            : CertificationState::Failed;
    }

#if defined(MODLOADER_CLIENT_BUILD)
    PersistentIdFixSourceReadObserver::CertificationState CertifyExperimentalIndependent(
        bool inventoryCaptured,
        bool inventoryValid,
        std::uint64_t inventoryGeneration,
        std::uint64_t loadGeneration,
        bool inventoryPresent,
        std::uint64_t successfulReads,
        std::uint64_t failedReads,
        bool descriptorRegistryReady,
        const PersistentIdFixIndependentSourceCollector::SectionSnapshot& section)
    {
        using PersistentIdFixSourceReadObserver::CertificationState;

        if (!inventoryCaptured || !inventoryValid || inventoryGeneration == 0 ||
            inventoryGeneration != loadGeneration)
        {
            return CertificationState::InventoryUnavailable;
        }

        if (!inventoryPresent)
        {
            if (successfulReads != 0 || section.attempts != 0 ||
                section.readFailures != 0 || section.failures != 0 ||
                section.schemaMismatches != 0 ||
                section.containerFailures != 0 || section.incompleteCoverage != 0)
            {
                return CertificationState::Failed;
            }

            return descriptorRegistryReady
                ? CertificationState::SafelyAbsent
                : CertificationState::Failed;
        }

        if (failedReads != 0 ||
            section.readFailures != 0 ||
            section.failures != 0 ||
            section.schemaMismatches != 0 ||
            section.containerFailures != 0 ||
            section.incompleteCoverage != 0)
        {
            return CertificationState::Failed;
        }

        if (successfulReads == 0 || section.attempts == 0)
            return CertificationState::Pending;

        if (successfulReads != section.attempts)
            return CertificationState::Failed;

        return IndependentSectionComplete(section)
            ? CertificationState::Certified
            : CertificationState::Failed;
    }
#endif

    const char* CertificationStateName(
        PersistentIdFixSourceReadObserver::CertificationState state)
    {
        using PersistentIdFixSourceReadObserver::CertificationState;

        switch (state)
        {
        case CertificationState::InventoryUnavailable:
            return "InventoryUnavailable";
        case CertificationState::AbsentUnsupported:
            return "AbsentUnsupported";
        case CertificationState::Pending:
            return "Pending";
        case CertificationState::Certified:
            return "Certified";
        case CertificationState::Failed:
            return "Failed";
        case CertificationState::SafelyAbsent:
            return "SafelyAbsent";
        default:
            return "<invalid>";
        }
    }

    void CountCertificationState(
        PersistentIdFixSourceReadObserver::CertificationState state,
        PersistentIdFixSourceReadObserver::CoverageSnapshot& snapshot)
    {
        using PersistentIdFixSourceReadObserver::CertificationState;

        switch (state)
        {
        case CertificationState::Certified:
            ++snapshot.certifiedSections;
            break;
        case CertificationState::SafelyAbsent:
            ++snapshot.safelyAbsentSections;
            break;
        case CertificationState::Pending:
            ++snapshot.pendingSections;
            break;
        case CertificationState::Failed:
            ++snapshot.failedSections;
            break;
        case CertificationState::AbsentUnsupported:
            ++snapshot.absentUnsupportedSections;
            break;
        case CertificationState::InventoryUnavailable:
            ++snapshot.inventoryUnavailableSections;
            break;
        default:
            ++snapshot.failedSections;
            break;
        }
    }
}

namespace PersistentIdFixSourceReadObserver
{
    bool ObserveGetSaveData(
        GetSaveDataFn original,
        void* saveSubsystem,
        void* sectionName,
        const void* structType,
        void* destination)
    {
        if (original == nullptr)
            return false;

        std::shared_lock<std::shared_mutex> sourceReadLock(
            g_sourcePublicationGate);

        ObserverFrameGuard observerFrame;

        CapturedSectionName capturedName;
        const SourceSection section =
            CaptureSection(sectionName, &capturedName);
        const std::uint64_t loadGeneration =
            g_loadGeneration.load(std::memory_order_relaxed);

        const bool result =
            original(
                saveSubsystem,
                sectionName,
                structType,
                destination);

        if (!result)
        {
            const auto classificationName =
                [](SourceSection value) -> const char*
                {
                    switch (value)
                    {
                    case SourceSection::Mass:
                        return "Mass";
                    case SourceSection::BuildingCustomNames:
                        return "BuildingCustomNames";
                    case SourceSection::GameStateData:
                        return "GameStateData";
                    case SourceSection::AntennasData:
                        return "AntennasData";
                    case SourceSection::ZiplineReplicator:
                        return "ZiplineReplicator";
                    case SourceSection::ZiplineSubsystem:
                        return "ZiplineSubsystem";
                    case SourceSection::BaseCoreReplicationHelper:
                        return "BaseCoreReplicationHelper";
#if defined(MODLOADER_CLIENT_BUILD)
                    case SourceSection::CrPackageTransportReplicator:
                        return "CrPackageTransportReplicator";
                    case SourceSection::StorageShareableSubsystemData:
                        return "StorageShareableSubsystemData";
                    case SourceSection::BaseSiteAttackData:
                        return "BaseSiteAttackData";
#endif
                    case SourceSection::Unknown:
                    default:
                        return "Unknown";
                    }
                };

            const auto formatSectionName =
                [&capturedName](char* output, std::size_t outputCapacity)
                {
                    if (!capturedName.Valid)
                    {
                        std::snprintf(
                            output,
                            outputCapacity,
                            "<unavailable:invalid-name>");
                        return;
                    }

                    std::size_t outputIndex = 0;

                    for (std::int32_t index = 0;
                        index < capturedName.Length;
                        ++index)
                    {
                        const std::uint32_t character =
                            static_cast<std::uint32_t>(capturedName.Text[index]);
                        const bool escaped =
                            character < 0x20 ||
                            character > 0x7e ||
                            character == '"' ||
                            character == '\\';
                        const std::size_t required = escaped ? 6 : 1;

                        if (outputIndex + required >= outputCapacity)
                            break;

                        if (escaped)
                        {
                            std::snprintf(
                                output + outputIndex,
                                outputCapacity - outputIndex,
                                "\\u%04X",
                                static_cast<unsigned int>(character & 0xffff));
                            outputIndex += 6;
                        }
                        else
                        {
                            output[outputIndex] = static_cast<char>(character);
                            ++outputIndex;
                            output[outputIndex] = '\0';
                        }
                    }

                    output[outputIndex] = '\0';
                };

            char diagnosticSectionName[
                kDiagnosticSectionNameCapacity * 6 + 1]{};
            formatSectionName(
                diagnosticSectionName,
                sizeof(diagnosticSectionName));

            const FStringView capturedNameView{
                capturedName.Text,
                capturedName.Length + 1,
                capturedName.Length + 1 };
            const bool isFlowSaveData =
                capturedName.Valid &&
                EqualsSection(capturedNameView, L"FlowSaveData");

            bool safelyAbsent = false;

#if defined(MODLOADER_CLIENT_BUILD)
            if (section == SourceSection::CrPackageTransportReplicator ||
                section == SourceSection::StorageShareableSubsystemData ||
                section == SourceSection::BaseSiteAttackData)
            {
                const CoverageSnapshot coverage =
                    GetCoverageSnapshot();
                const bool inventoryCurrent =
                    coverage.inventoryCaptured &&
                    coverage.inventoryValid &&
                    coverage.loadGeneration == loadGeneration &&
                    coverage.inventoryGeneration == loadGeneration;

                if (inventoryCurrent)
                {
                    switch (section)
                    {
                    case SourceSection::CrPackageTransportReplicator:
                        safelyAbsent =
                            coverage.crPackageTransportReplicatorCertification ==
                            CertificationState::SafelyAbsent;
                        break;
                    case SourceSection::StorageShareableSubsystemData:
                        safelyAbsent =
                            coverage.storageShareableSubsystemDataCertification ==
                            CertificationState::SafelyAbsent;
                        break;
                    case SourceSection::BaseSiteAttackData:
                        safelyAbsent =
                            coverage.baseSiteAttackDataCertification ==
                            CertificationState::SafelyAbsent;
                        break;
                    default:
                        break;
                    }
                }
            }
#endif

            if (isFlowSaveData)
            {
                LOG_INFO(
                    "PersistentIdFix: optional save section unavailable: generation=%llu section=\"%s\" classification=%s protected=%u",
                    static_cast<unsigned long long>(loadGeneration),
                    diagnosticSectionName,
                    classificationName(section),
                    section != SourceSection::Unknown ? 1u : 0u);
            }
            else if (safelyAbsent)
            {
                LOG_INFO(
                    "PersistentIdFix: optional save section absent (SafelyAbsent): generation=%llu section=\"%s\" classification=%s protected=%u",
                    static_cast<unsigned long long>(loadGeneration),
                    diagnosticSectionName,
                    classificationName(section),
                    section != SourceSection::Unknown ? 1u : 0u);
            }
            else if (section == SourceSection::Unknown)
            {
                LOG_WARN(
                    "PersistentIdFix: native GetSaveData returned false: generation=%llu section=\"%s\" classification=%s protected=%u",
                    static_cast<unsigned long long>(loadGeneration),
                    diagnosticSectionName,
                    classificationName(section),
                    section != SourceSection::Unknown ? 1u : 0u);
            }
            else
            {
                LOG_ERROR(
                    "PersistentIdFix: native GetSaveData failed: generation=%llu section=\"%s\" classification=%s protected=%u",
                    static_cast<unsigned long long>(loadGeneration),
                    diagnosticSectionName,
                    classificationName(section),
                    section != SourceSection::Unknown ? 1u : 0u);
            }

            g_failedReads.fetch_add(
                1,
                std::memory_order_relaxed);

            switch (section)
            {
            case SourceSection::Mass:
                g_massReadFailures.fetch_add(
                    1,
                    std::memory_order_relaxed);
                break;

            case SourceSection::GameStateData:
                PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                    PersistentIdFixIndependentSourceCollector::Section::GameStateData);
                break;
            case SourceSection::BuildingCustomNames:
                PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                    PersistentIdFixIndependentSourceCollector::Section::BuildingCustomNames);
                break;
            case SourceSection::AntennasData:
                PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                    PersistentIdFixIndependentSourceCollector::Section::AntennasData);
                break;
            case SourceSection::ZiplineReplicator:
                PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                    PersistentIdFixIndependentSourceCollector::Section::ZiplineReplicator);
                break;
            case SourceSection::ZiplineSubsystem:
                PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                    PersistentIdFixIndependentSourceCollector::Section::ZiplineSubsystem);
                break;
            case SourceSection::BaseCoreReplicationHelper:
                PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                    PersistentIdFixIndependentSourceCollector::Section::BaseCoreReplication);
                break;
#if defined(MODLOADER_CLIENT_BUILD)
            case SourceSection::CrPackageTransportReplicator:
                g_crPackageTransportReplicatorFailedReads.fetch_add(
                    1,
                    std::memory_order_relaxed);
                if (!g_inventoryCaptured.load(std::memory_order_relaxed) ||
                    !g_inventoryValid.load(std::memory_order_relaxed) ||
                    g_inventoryCrPackageTransportReplicator.load(std::memory_order_relaxed))
                {
                    PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                        PersistentIdFixIndependentSourceCollector::Section::CrPackageTransportReplicator);
                }
                break;

            case SourceSection::StorageShareableSubsystemData:
                g_storageShareableSubsystemDataFailedReads.fetch_add(
                    1,
                    std::memory_order_relaxed);
                if (!g_inventoryCaptured.load(std::memory_order_relaxed) ||
                    !g_inventoryValid.load(std::memory_order_relaxed) ||
                    g_inventoryStorageShareableSubsystemData.load(std::memory_order_relaxed))
                {
                    PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                        PersistentIdFixIndependentSourceCollector::Section::StorageShareableSubsystemData);
                }
                break;

            case SourceSection::BaseSiteAttackData:
                g_baseSiteAttackDataFailedReads.fetch_add(
                    1,
                    std::memory_order_relaxed);
                if (!g_inventoryCaptured.load(std::memory_order_relaxed) ||
                    !g_inventoryValid.load(std::memory_order_relaxed) ||
                    g_inventoryBaseSiteAttackData.load(std::memory_order_relaxed))
                {
                    PersistentIdFixIndependentSourceCollector::RecordReadFailure(
                        PersistentIdFixIndependentSourceCollector::Section::BaseSiteAttackData);
                }
                break;
#endif
            default:
                break;
            }

            return result;
        }

        g_successfulReads.fetch_add(
            1,
            std::memory_order_relaxed);

        if (section == SourceSection::Mass)
        {
            // Native GetSaveData has successfully reconstructed the typed Mass
            // destination. Capture the original serialized high-water before
            // PostInitialize resumes and applies it to the live PID subsystem.
            const auto* descriptor =
                static_cast<const SDK::UScriptStruct*>(structType);
            const auto* massSaveData =
                static_cast<const SDK::FCrMassSaveData*>(destination);

            if (PersistentIdFixMassFragmentClassifier::ValidateMassSaveDataEnvelope(
                    descriptor,
                    massSaveData))
            {
                if (!CaptureSavedHighWater(massSaveData))
                {
                    LOG_ERROR(
                        "PersistentIdFix: failed to capture coherent saved Mass MaxPersistentID");
                }
            }
            else
            {
                LOG_ERROR(
                    "PersistentIdFix: saved Mass high-water capture skipped because destination validation failed");
            }

            // Inspect the same typed destination synchronously before the
            // caller resumes.
            PersistentIdFixMassFragmentClassifier::ObserveMassSaveData(
                descriptor,
                massSaveData);
        }
        else
        {
            const SDK::UScriptStruct* descriptor =
                static_cast<const SDK::UScriptStruct*>(structType);

            switch (section)
            {
            case SourceSection::GameStateData:
                PersistentIdFixIndependentSourceCollector::ObserveGameState(
                    descriptor,
                    static_cast<const SDK::FGameStateSaveData*>(destination));
                break;

            case SourceSection::BuildingCustomNames:
                PersistentIdFixIndependentSourceCollector::ObserveBuildingCustomNames(
                    descriptor,
                    static_cast<const SDK::FBuildingCustomNameSaveData*>(destination));
                break;

            case SourceSection::AntennasData:
                PersistentIdFixIndependentSourceCollector::ObserveAntennas(
                    descriptor,
                    static_cast<const SDK::FCrAntennaSaveData*>(destination));
                break;

            case SourceSection::ZiplineReplicator:
                PersistentIdFixIndependentSourceCollector::ObserveZiplineReplicator(
                    descriptor,
                    static_cast<const SDK::FCrZiplineReplicatorSaveData*>(destination));
                break;

            case SourceSection::ZiplineSubsystem:
                PersistentIdFixIndependentSourceCollector::ObserveZiplineSubsystem(
                    descriptor,
                    static_cast<const SDK::FCrZiplineSaveData*>(destination));
                break;

            case SourceSection::BaseCoreReplicationHelper:
                PersistentIdFixIndependentSourceCollector::ObserveBaseCoreReplication(
                    descriptor,
                    static_cast<const SDK::FBaseCoreReplicationSaveData*>(destination));
                break;

#if defined(MODLOADER_CLIENT_BUILD)
            case SourceSection::CrPackageTransportReplicator:
                PersistentIdFixIndependentSourceCollector::ObserveCrPackageTransportReplicator(
                    descriptor,
                    static_cast<const SDK::FCrSenderConnectionsSaveData*>(destination));
                if (g_inventoryCaptured.load(std::memory_order_relaxed) &&
                    g_inventoryValid.load(std::memory_order_relaxed) &&
                    !g_inventoryCrPackageTransportReplicator.load(std::memory_order_relaxed))
                {
                    PersistentIdFixIndependentSourceCollector::RecordConsistencyFailure(
                        PersistentIdFixIndependentSourceCollector::Section::CrPackageTransportReplicator);
                }
                break;

            case SourceSection::StorageShareableSubsystemData:
                PersistentIdFixIndependentSourceCollector::ObserveStorageShareableSubsystemData(
                    descriptor,
                    static_cast<const SDK::FCrStorageShareableSubsystemSaveData*>(destination));
                if (g_inventoryCaptured.load(std::memory_order_relaxed) &&
                    g_inventoryValid.load(std::memory_order_relaxed) &&
                    !g_inventoryStorageShareableSubsystemData.load(std::memory_order_relaxed))
                {
                    PersistentIdFixIndependentSourceCollector::RecordConsistencyFailure(
                        PersistentIdFixIndependentSourceCollector::Section::StorageShareableSubsystemData);
                }
                break;

            case SourceSection::BaseSiteAttackData:
                PersistentIdFixIndependentSourceCollector::ObserveBaseSiteAttackData(
                    descriptor,
                    static_cast<const SDK::FBaseSiteAttackSubsystemSaveData*>(destination));
                if (g_inventoryCaptured.load(std::memory_order_relaxed) &&
                    g_inventoryValid.load(std::memory_order_relaxed) &&
                    !g_inventoryBaseSiteAttackData.load(std::memory_order_relaxed))
                {
                    PersistentIdFixIndependentSourceCollector::RecordConsistencyFailure(
                        PersistentIdFixIndependentSourceCollector::Section::BaseSiteAttackData);
                }
                break;
#endif

            default:
                break;
            }
        }

        if (section != SourceSection::Unknown)
        {
            g_recognizedSuccessfulReads.fetch_add(
                1,
                std::memory_order_relaxed);

            std::atomic<std::uint64_t>* sectionCounter = nullptr;

            switch (section)
            {
            case SourceSection::Mass:
                sectionCounter = &g_massReads;
                break;

            case SourceSection::BuildingCustomNames:
                sectionCounter = &g_buildingCustomNameReads;
                break;

            case SourceSection::GameStateData:
                sectionCounter = &g_gameStateDataReads;
                break;

            case SourceSection::AntennasData:
                sectionCounter = &g_antennasDataReads;
                break;

            case SourceSection::ZiplineReplicator:
                sectionCounter = &g_ziplineReplicatorReads;
                break;

            case SourceSection::ZiplineSubsystem:
                sectionCounter = &g_ziplineSubsystemReads;
                break;

            case SourceSection::BaseCoreReplicationHelper:
                sectionCounter = &g_baseCoreReplicationHelperReads;
                break;

#if defined(MODLOADER_CLIENT_BUILD)
            case SourceSection::CrPackageTransportReplicator:
                sectionCounter = &g_crPackageTransportReplicatorReads;
                break;

            case SourceSection::StorageShareableSubsystemData:
                sectionCounter = &g_storageShareableSubsystemDataReads;
                break;

            case SourceSection::BaseSiteAttackData:
                sectionCounter = &g_baseSiteAttackDataReads;
                break;
#endif

            default:
                break;
            }

            if (sectionCounter != nullptr)
            {
                sectionCounter->fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        }

        return result;
    }

    CoverageSnapshot GetCoverageSnapshot()
    {
        CoverageSnapshot snapshot;

        snapshot.loadGeneration =
            g_loadGeneration.load(
                std::memory_order_relaxed);
        snapshot.inventoryGeneration =
            g_inventoryGeneration.load(
                std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(g_savedHighWaterMutex);
            snapshot.savedHighWaterCaptured = g_savedHighWater.captured;
            snapshot.savedHighWaterConflict = g_savedHighWater.conflict;
            snapshot.savedHighWaterGeneration = g_savedHighWater.generation;
            snapshot.savedHighWaterCaptures = g_savedHighWater.captures;
            snapshot.savedHighWater = g_savedHighWater.value;
        }

        snapshot.gameWorldAttached =
            g_gameWorldAttached.load(std::memory_order_relaxed);
        snapshot.attachedLoadGeneration =
            g_attachedLoadGeneration.load(std::memory_order_relaxed);
        snapshot.coverageAttachedToCurrentGeneration =
            snapshot.gameWorldAttached &&
            snapshot.loadGeneration != 0 &&
            snapshot.attachedLoadGeneration == snapshot.loadGeneration;

        snapshot.inventoryCaptured =
            g_inventoryCaptured.load(std::memory_order_relaxed);
        snapshot.inventoryValid =
            g_inventoryValid.load(std::memory_order_relaxed);
        snapshot.inventoryEntries =
            g_inventoryEntries.load(std::memory_order_relaxed);
        snapshot.inventoryRecognized =
            g_inventoryRecognized.load(std::memory_order_relaxed);
        snapshot.inventoryMass =
            g_inventoryMass.load(std::memory_order_relaxed);
        snapshot.inventoryBuildingCustomNames =
            g_inventoryBuildingCustomNames.load(std::memory_order_relaxed);
        snapshot.inventoryGameStateData =
            g_inventoryGameStateData.load(std::memory_order_relaxed);
        snapshot.inventoryAntennasData =
            g_inventoryAntennasData.load(std::memory_order_relaxed);
        snapshot.inventoryZiplineReplicator =
            g_inventoryZiplineReplicator.load(std::memory_order_relaxed);
        snapshot.inventoryZiplineSubsystem =
            g_inventoryZiplineSubsystem.load(std::memory_order_relaxed);
        snapshot.inventoryBaseCoreReplicationHelper =
            g_inventoryBaseCoreReplicationHelper.load(std::memory_order_relaxed);
#if defined(MODLOADER_CLIENT_BUILD)
        snapshot.inventoryCrPackageTransportReplicator =
            g_inventoryCrPackageTransportReplicator.load(std::memory_order_relaxed);
        snapshot.inventoryStorageShareableSubsystemData =
            g_inventoryStorageShareableSubsystemData.load(std::memory_order_relaxed);
        snapshot.inventoryBaseSiteAttackData =
            g_inventoryBaseSiteAttackData.load(std::memory_order_relaxed);
#endif

        snapshot.successfulReads =
            g_successfulReads.load(
                std::memory_order_relaxed);

        snapshot.recognizedSuccessfulReads =
            g_recognizedSuccessfulReads.load(
                std::memory_order_relaxed);

        snapshot.failedReads =
            g_failedReads.load(
                std::memory_order_relaxed);

        snapshot.massReads =
            g_massReads.load(
                std::memory_order_relaxed);

        snapshot.buildingCustomNameReads =
            g_buildingCustomNameReads.load(
                std::memory_order_relaxed);

        snapshot.gameStateDataReads =
            g_gameStateDataReads.load(
                std::memory_order_relaxed);

        snapshot.antennasDataReads =
            g_antennasDataReads.load(
                std::memory_order_relaxed);

        snapshot.ziplineReplicatorReads =
            g_ziplineReplicatorReads.load(
                std::memory_order_relaxed);

        snapshot.ziplineSubsystemReads =
            g_ziplineSubsystemReads.load(
                std::memory_order_relaxed);

        snapshot.baseCoreReplicationHelperReads =
            g_baseCoreReplicationHelperReads.load(
                std::memory_order_relaxed);
#if defined(MODLOADER_CLIENT_BUILD)
        snapshot.crPackageTransportReplicatorReads =
            g_crPackageTransportReplicatorReads.load(std::memory_order_relaxed);
        snapshot.crPackageTransportReplicatorFailedReads =
            g_crPackageTransportReplicatorFailedReads.load(std::memory_order_relaxed);
        snapshot.storageShareableSubsystemDataReads =
            g_storageShareableSubsystemDataReads.load(std::memory_order_relaxed);
        snapshot.storageShareableSubsystemDataFailedReads =
            g_storageShareableSubsystemDataFailedReads.load(std::memory_order_relaxed);
        snapshot.baseSiteAttackDataReads =
            g_baseSiteAttackDataReads.load(std::memory_order_relaxed);
        snapshot.baseSiteAttackDataFailedReads =
            g_baseSiteAttackDataFailedReads.load(std::memory_order_relaxed);
#endif

        const auto independent =
            PersistentIdFixIndependentSourceCollector::GetSnapshot();

        const auto mass =
            PersistentIdFixMassFragmentClassifier::GetSnapshot();

        if (!snapshot.inventoryCaptured || !snapshot.inventoryValid)
        {
            snapshot.massCertification =
                CertificationState::InventoryUnavailable;
        }
        else if (!snapshot.inventoryMass)
        {
            snapshot.massCertification =
                CertificationState::AbsentUnsupported;
        }
        else if (g_massReadFailures.load(std::memory_order_relaxed) != 0)
        {
            snapshot.massCertification =
                CertificationState::Failed;
        }
        else if (snapshot.massReads == 0)
        {
            snapshot.massCertification =
                CertificationState::Pending;
        }
        else if (!snapshot.savedHighWaterCaptured ||
            snapshot.savedHighWaterConflict ||
            snapshot.savedHighWaterGeneration != snapshot.loadGeneration)
        {
            snapshot.massCertification =
                CertificationState::Failed;
        }
        else
        {
            snapshot.massCertification =
                MassSectionComplete(mass)
                ? CertificationState::Certified
                : CertificationState::Failed;
        }

        snapshot.buildingCustomNamesCertification =
            CertifyIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryBuildingCustomNames,
                snapshot.buildingCustomNameReads,
                independent.buildingCustomNames);

        snapshot.gameStateDataCertification =
            CertifyIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryGameStateData,
                snapshot.gameStateDataReads,
                independent.gameStateData);

        snapshot.antennasDataCertification =
            CertifyIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryAntennasData,
                snapshot.antennasDataReads,
                independent.antennasData);

        snapshot.ziplineReplicatorCertification =
            CertifyIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryZiplineReplicator,
                snapshot.ziplineReplicatorReads,
                independent.ziplineReplicator);

        snapshot.ziplineSubsystemCertification =
            CertifyIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryZiplineSubsystem,
                snapshot.ziplineSubsystemReads,
                independent.ziplineSubsystem);

        snapshot.baseCoreReplicationCertification =
            CertifyIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryBaseCoreReplicationHelper,
                snapshot.baseCoreReplicationHelperReads,
                independent.baseCoreReplication);

#if defined(MODLOADER_CLIENT_BUILD)
        const bool descriptorRegistryReady =
            PersistentIdFixIndependentSourceCollector::IsDescriptorRegistryReady();

        snapshot.crPackageTransportReplicatorCertification =
            CertifyExperimentalIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryGeneration,
                snapshot.loadGeneration,
                snapshot.inventoryCrPackageTransportReplicator,
                snapshot.crPackageTransportReplicatorReads,
                snapshot.crPackageTransportReplicatorFailedReads,
                descriptorRegistryReady,
                independent.crPackageTransportReplicator);

        snapshot.storageShareableSubsystemDataCertification =
            CertifyExperimentalIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryGeneration,
                snapshot.loadGeneration,
                snapshot.inventoryStorageShareableSubsystemData,
                snapshot.storageShareableSubsystemDataReads,
                snapshot.storageShareableSubsystemDataFailedReads,
                descriptorRegistryReady,
                independent.storageShareableSubsystemData);

        snapshot.baseSiteAttackDataCertification =
            CertifyExperimentalIndependent(
                snapshot.inventoryCaptured,
                snapshot.inventoryValid,
                snapshot.inventoryGeneration,
                snapshot.loadGeneration,
                snapshot.inventoryBaseSiteAttackData,
                snapshot.baseSiteAttackDataReads,
                snapshot.baseSiteAttackDataFailedReads,
                descriptorRegistryReady,
                independent.baseSiteAttackData);
#endif

        CountCertificationState(
            snapshot.massCertification,
            snapshot);
        CountCertificationState(
            snapshot.buildingCustomNamesCertification,
            snapshot);
        CountCertificationState(
            snapshot.gameStateDataCertification,
            snapshot);
        CountCertificationState(
            snapshot.antennasDataCertification,
            snapshot);
        CountCertificationState(
            snapshot.ziplineReplicatorCertification,
            snapshot);
        CountCertificationState(
            snapshot.ziplineSubsystemCertification,
            snapshot);
        CountCertificationState(
            snapshot.baseCoreReplicationCertification,
            snapshot);
#if defined(MODLOADER_CLIENT_BUILD)
        CountCertificationState(
            snapshot.crPackageTransportReplicatorCertification,
            snapshot);
        CountCertificationState(
            snapshot.storageShareableSubsystemDataCertification,
            snapshot);
        CountCertificationState(
            snapshot.baseSiteAttackDataCertification,
            snapshot);
#endif

#if defined(MODLOADER_CLIENT_BUILD)
        const bool originalSevenCertified =
            snapshot.massCertification == CertificationState::Certified &&
            snapshot.buildingCustomNamesCertification == CertificationState::Certified &&
            snapshot.gameStateDataCertification == CertificationState::Certified &&
            snapshot.antennasDataCertification == CertificationState::Certified &&
            snapshot.ziplineReplicatorCertification == CertificationState::Certified &&
            snapshot.ziplineSubsystemCertification == CertificationState::Certified &&
            snapshot.baseCoreReplicationCertification == CertificationState::Certified;

        const bool newDomainsResolved =
            (snapshot.crPackageTransportReplicatorCertification == CertificationState::Certified ||
                snapshot.crPackageTransportReplicatorCertification == CertificationState::SafelyAbsent) &&
            (snapshot.storageShareableSubsystemDataCertification == CertificationState::Certified ||
                snapshot.storageShareableSubsystemDataCertification == CertificationState::SafelyAbsent) &&
            (snapshot.baseSiteAttackDataCertification == CertificationState::Certified ||
                snapshot.baseSiteAttackDataCertification == CertificationState::SafelyAbsent);

        snapshot.sourceCoverageComplete =
            snapshot.inventoryCaptured &&
            snapshot.inventoryValid &&
            snapshot.inventoryGeneration == snapshot.loadGeneration &&
            originalSevenCertified &&
            newDomainsResolved &&
            snapshot.pendingSections == 0 &&
            snapshot.failedSections == 0 &&
            snapshot.absentUnsupportedSections == 0 &&
            snapshot.inventoryUnavailableSections == 0;
#else
        snapshot.sourceCoverageComplete =
            snapshot.inventoryCaptured &&
            snapshot.inventoryValid &&
            snapshot.certifiedSections == 7 &&
            snapshot.pendingSections == 0 &&
            snapshot.failedSections == 0 &&
            snapshot.absentUnsupportedSections == 0 &&
            snapshot.inventoryUnavailableSections == 0;
#endif

        return snapshot;
    }

    ObserverActivitySnapshot GetObserverActivitySnapshot()
    {
        ObserverActivitySnapshot snapshot;
        snapshot.activeFrames =
            g_activeObserverFrames.load(std::memory_order_seq_cst);
        snapshot.frameTransitions =
            g_observerFrameTransitions.load(std::memory_order_seq_cst);
        return snapshot;
    }

    bool BeginPublicationFreeze()
    {
        {
            std::lock_guard<std::mutex> stateLock(
                g_publicationFreezeStateMutex);

            if (g_publicationFreezeHeld)
            {
                LOG_ERROR(
                    "PersistentIdFix: publication freeze rejected: freeze already held");
                return false;
            }

            g_publicationFreezeHeld = true;
        }

        try
        {
            g_sourcePublicationGate.lock();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> stateLock(
                g_publicationFreezeStateMutex);
            g_publicationFreezeHeld = false;

            LOG_ERROR(
                "PersistentIdFix: publication freeze failed while acquiring source gate");
            return false;
        }

        return true;
    }

    void EndPublicationFreeze()
    {
        bool release = false;

        {
            std::lock_guard<std::mutex> stateLock(
                g_publicationFreezeStateMutex);

            if (!g_publicationFreezeHeld)
            {
                LOG_ERROR(
                    "PersistentIdFix: publication freeze release requested with no held freeze");
                return;
            }

            g_publicationFreezeHeld = false;
            release = true;
        }

        if (release)
            g_sourcePublicationGate.unlock();
    }

    void LogCoverage(const char* phase)
    {
        const CoverageSnapshot snapshot =
            GetCoverageSnapshot();
#if defined(MODLOADER_CLIENT_BUILD)
        const auto collectorSnapshot =
            PersistentIdFixIndependentSourceCollector::GetSnapshot();
#endif

        LOG_INFO(
            "PersistentIdFix: source reads [%s]: generation=%llu successful=%llu recognized=%llu failed=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.loadGeneration),
            static_cast<unsigned long long>(snapshot.successfulReads),
            static_cast<unsigned long long>(snapshot.recognizedSuccessfulReads),
            static_cast<unsigned long long>(snapshot.failedReads));

        LOG_INFO(
            "PersistentIdFix: saved high-water [%s]: captured=%u conflict=%u generation=%llu currentGeneration=%llu captures=%llu value=%u",
            phase != nullptr ? phase : "<null>",
            snapshot.savedHighWaterCaptured ? 1u : 0u,
            snapshot.savedHighWaterConflict ? 1u : 0u,
            static_cast<unsigned long long>(snapshot.savedHighWaterGeneration),
            static_cast<unsigned long long>(snapshot.loadGeneration),
            static_cast<unsigned long long>(snapshot.savedHighWaterCaptures),
            snapshot.savedHighWater);

        LOG_INFO(
            "PersistentIdFix: source inventory [%s]: captured=%u valid=%u entries=%llu recognized=%llu Mass=%u CustomNames=%u GameState=%u Antennas=%u ZiplineReplicator=%u ZiplineSubsystem=%u BaseCoreReplication=%u",
            phase != nullptr ? phase : "<null>",
            snapshot.inventoryCaptured ? 1u : 0u,
            snapshot.inventoryValid ? 1u : 0u,
            static_cast<unsigned long long>(snapshot.inventoryEntries),
            static_cast<unsigned long long>(snapshot.inventoryRecognized),
            snapshot.inventoryMass ? 1u : 0u,
            snapshot.inventoryBuildingCustomNames ? 1u : 0u,
            snapshot.inventoryGameStateData ? 1u : 0u,
            snapshot.inventoryAntennasData ? 1u : 0u,
            snapshot.inventoryZiplineReplicator ? 1u : 0u,
            snapshot.inventoryZiplineSubsystem ? 1u : 0u,
            snapshot.inventoryBaseCoreReplicationHelper ? 1u : 0u);

#if defined(MODLOADER_CLIENT_BUILD)
        LOG_INFO(
            "PersistentIdFix: experimental source inventory [%s]: CrPackageTransportReplicator=%u StorageShareableSubsystemData=%u BaseSiteAttackData=%u generation=%llu inventoryGeneration=%llu",
            phase != nullptr ? phase : "<null>",
            snapshot.inventoryCrPackageTransportReplicator ? 1u : 0u,
            snapshot.inventoryStorageShareableSubsystemData ? 1u : 0u,
            snapshot.inventoryBaseSiteAttackData ? 1u : 0u,
            static_cast<unsigned long long>(snapshot.loadGeneration),
            static_cast<unsigned long long>(snapshot.inventoryGeneration));
#endif

        LOG_INFO(
            "PersistentIdFix: source certification [%s]: Mass=%s CustomNames=%s GameState=%s Antennas=%s ZiplineReplicator=%s ZiplineSubsystem=%s BaseCoreReplication=%s",
            phase != nullptr ? phase : "<null>",
            CertificationStateName(snapshot.massCertification),
            CertificationStateName(snapshot.buildingCustomNamesCertification),
            CertificationStateName(snapshot.gameStateDataCertification),
            CertificationStateName(snapshot.antennasDataCertification),
            CertificationStateName(snapshot.ziplineReplicatorCertification),
            CertificationStateName(snapshot.ziplineSubsystemCertification),
            CertificationStateName(snapshot.baseCoreReplicationCertification));

#if defined(MODLOADER_CLIENT_BUILD)
        LOG_INFO(
            "PersistentIdFix: experimental source certification [%s]: CrPackageTransportReplicator=%s StorageShareableSubsystemData=%s BaseSiteAttackData=%s",
            phase != nullptr ? phase : "<null>",
            CertificationStateName(snapshot.crPackageTransportReplicatorCertification),
            CertificationStateName(snapshot.storageShareableSubsystemDataCertification),
            CertificationStateName(snapshot.baseSiteAttackDataCertification));
#endif

        LOG_INFO(
            "PersistentIdFix: source coverage [%s]: complete=%u certified=%llu pending=%llu failed=%llu absentUnsupported=%llu inventoryUnavailable=%llu",
            phase != nullptr ? phase : "<null>",
            snapshot.sourceCoverageComplete ? 1u : 0u,
            static_cast<unsigned long long>(snapshot.certifiedSections),
            static_cast<unsigned long long>(snapshot.pendingSections),
            static_cast<unsigned long long>(snapshot.failedSections),
            static_cast<unsigned long long>(snapshot.absentUnsupportedSections),
            static_cast<unsigned long long>(snapshot.inventoryUnavailableSections));

#if defined(MODLOADER_CLIENT_BUILD)
        LOG_INFO(
            "PersistentIdFix: experimental source outcomes [%s]: CrPackageTransportReplicator=read:%llu readFailed:%llu collect:%llu/%llu protectedIds:%llu StorageShareableSubsystemData=read:%llu readFailed:%llu collect:%llu/%llu protectedIds:%llu BaseSiteAttackData=read:%llu readFailed:%llu collect:%llu/%llu protectedIds:%llu certified=%llu safelyAbsent=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.crPackageTransportReplicatorReads),
            static_cast<unsigned long long>(snapshot.crPackageTransportReplicatorFailedReads),
            static_cast<unsigned long long>(collectorSnapshot.crPackageTransportReplicator.successes),
            static_cast<unsigned long long>(collectorSnapshot.crPackageTransportReplicator.attempts),
            static_cast<unsigned long long>(collectorSnapshot.crPackageTransportReplicator.values),
            static_cast<unsigned long long>(snapshot.storageShareableSubsystemDataReads),
            static_cast<unsigned long long>(snapshot.storageShareableSubsystemDataFailedReads),
            static_cast<unsigned long long>(collectorSnapshot.storageShareableSubsystemData.successes),
            static_cast<unsigned long long>(collectorSnapshot.storageShareableSubsystemData.attempts),
            static_cast<unsigned long long>(collectorSnapshot.storageShareableSubsystemData.values),
            static_cast<unsigned long long>(snapshot.baseSiteAttackDataReads),
            static_cast<unsigned long long>(snapshot.baseSiteAttackDataFailedReads),
            static_cast<unsigned long long>(collectorSnapshot.baseSiteAttackData.successes),
            static_cast<unsigned long long>(collectorSnapshot.baseSiteAttackData.attempts),
            static_cast<unsigned long long>(collectorSnapshot.baseSiteAttackData.values),
            static_cast<unsigned long long>(snapshot.certifiedSections),
            static_cast<unsigned long long>(snapshot.safelyAbsentSections));
#endif

        LOG_INFO(
            "PersistentIdFix: source attachment [%s]: gameWorld=%u generation=%llu attachedGeneration=%llu current=%u coverageComplete=%u",
            phase != nullptr ? phase : "<null>",
            snapshot.gameWorldAttached ? 1u : 0u,
            static_cast<unsigned long long>(snapshot.loadGeneration),
            static_cast<unsigned long long>(snapshot.attachedLoadGeneration),
            snapshot.coverageAttachedToCurrentGeneration ? 1u : 0u,
            snapshot.sourceCoverageComplete ? 1u : 0u);

        LOG_INFO(
            "PersistentIdFix: source sections [%s]: Mass=%llu CustomNames=%llu GameState=%llu Antennas=%llu ZiplineReplicator=%llu ZiplineSubsystem=%llu BaseCoreReplication=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.massReads),
            static_cast<unsigned long long>(snapshot.buildingCustomNameReads),
            static_cast<unsigned long long>(snapshot.gameStateDataReads),
            static_cast<unsigned long long>(snapshot.antennasDataReads),
            static_cast<unsigned long long>(snapshot.ziplineReplicatorReads),
            static_cast<unsigned long long>(snapshot.ziplineSubsystemReads),
            static_cast<unsigned long long>(snapshot.baseCoreReplicationHelperReads));

        PersistentIdFixMassFragmentClassifier::LogSnapshot(phase);
        PersistentIdFixIndependentSourceCollector::LogSnapshot(phase);
    }

    void BeginLoadGeneration(
        void* saveSubsystem)
    {
        std::unique_lock<std::shared_mutex> generationLock(
            g_sourcePublicationGate);

        ResetObservationCounters();

        g_gameWorldAttached.store(false, std::memory_order_relaxed);
        g_attachedLoadGeneration.store(0, std::memory_order_relaxed);

        const std::uint64_t generation =
            g_loadGeneration.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

        g_inventoryGeneration.store(
            generation,
            std::memory_order_relaxed);

        g_inventoryCaptured.store(true, std::memory_order_relaxed);

        const bool inventoryValid =
            CaptureInventory(saveSubsystem);

        g_inventoryValid.store(
            inventoryValid,
            std::memory_order_relaxed);

        LOG_INFO(
            "PersistentIdFix: source observation generation started: %llu inventoryValid=%u",
            static_cast<unsigned long long>(generation),
            inventoryValid ? 1u : 0u);
    }

    void AttachCurrentGenerationToGameWorld()
    {
        const std::uint64_t generation =
            g_loadGeneration.load(std::memory_order_relaxed);

        if (generation == 0)
        {
            g_gameWorldAttached.store(false, std::memory_order_relaxed);
            g_attachedLoadGeneration.store(0, std::memory_order_relaxed);
            return;
        }

        g_attachedLoadGeneration.store(
            generation,
            std::memory_order_relaxed);
        g_gameWorldAttached.store(true, std::memory_order_relaxed);

        LOG_INFO(
            "PersistentIdFix: source generation attached to game world: %llu",
            static_cast<unsigned long long>(generation));
    }

    void DetachGameWorld()
    {
        const std::uint64_t generation =
            g_attachedLoadGeneration.load(std::memory_order_relaxed);

        g_gameWorldAttached.store(false, std::memory_order_relaxed);
        g_attachedLoadGeneration.store(0, std::memory_order_relaxed);

        if (generation != 0)
        {
            LOG_INFO(
                "PersistentIdFix: source generation detached from game world: %llu",
                static_cast<unsigned long long>(generation));
        }
    }

    void Reset()
    {
        std::unique_lock<std::shared_mutex> resetLock(
            g_sourcePublicationGate);

        ResetObservationCounters();
        g_gameWorldAttached.store(false, std::memory_order_relaxed);
        g_attachedLoadGeneration.store(0, std::memory_order_relaxed);
        g_loadGeneration.store(0, std::memory_order_relaxed);
    }
}

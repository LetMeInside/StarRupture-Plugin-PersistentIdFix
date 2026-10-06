#include "SourceReadObserver.h"

#include "MassFragmentClassifier.h"
#include "IndependentSourceCollector.h"
#include "SDK/Chimera_structs.hpp"

#include "plugin_helpers.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>

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

    std::atomic<std::uint64_t> g_loadGeneration{ 0 };
    std::atomic<bool> g_gameWorldAttached{ false };
    std::atomic<std::uint64_t> g_attachedLoadGeneration{ 0 };

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

    void ResetObservationCounters()
    {
        g_inventoryCaptured.store(false, std::memory_order_relaxed);
        g_inventoryValid.store(false, std::memory_order_relaxed);
        g_inventoryEntries.store(0, std::memory_order_relaxed);
        g_inventoryRecognized.store(0, std::memory_order_relaxed);
        g_inventoryMass.store(false, std::memory_order_relaxed);
        g_inventoryBuildingCustomNames.store(false, std::memory_order_relaxed);
        g_inventoryGameStateData.store(false, std::memory_order_relaxed);
        g_inventoryAntennasData.store(false, std::memory_order_relaxed);
        g_inventoryZiplineReplicator.store(false, std::memory_order_relaxed);
        g_inventoryZiplineSubsystem.store(false, std::memory_order_relaxed);
        g_inventoryBaseCoreReplicationHelper.store(false, std::memory_order_relaxed);

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

        PersistentIdFixMassFragmentClassifier::Reset();
        PersistentIdFixIndependentSourceCollector::Reset();
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

        return true;
    }

    PersistentIdFixSourceReadObserver::SourceSection CaptureSection(
        const void* sectionName)
    {
        using PersistentIdFixSourceReadObserver::SourceSection;

        if (sectionName == nullptr)
            return SourceSection::Unknown;

        const auto& value =
            *static_cast<const FStringView*>(sectionName);

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

        // This is the only information Step 3A reads before the native call.
        // Do not retain the FString pointer: native GetSaveData owns the
        // by-value argument lifetime and destroys it before returning.
        const SourceSection section =
            CaptureSection(sectionName);

        const bool result =
            original(
                saveSubsystem,
                sectionName,
                structType,
                destination);

        if (!result)
        {
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
            default:
                break;
            }

            return false;
        }

        g_successfulReads.fetch_add(
            1,
            std::memory_order_relaxed);

        if (section == SourceSection::Mass)
        {
            // Native GetSaveData has successfully reconstructed the typed Mass
            // destination. Inspect it synchronously before the caller resumes.
            PersistentIdFixMassFragmentClassifier::ObserveMassSaveData(
                static_cast<const SDK::UScriptStruct*>(structType),
                static_cast<const SDK::FCrMassSaveData*>(destination));
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

        return true;
    }

    CoverageSnapshot GetCoverageSnapshot()
    {
        CoverageSnapshot snapshot;

        snapshot.loadGeneration =
            g_loadGeneration.load(
                std::memory_order_relaxed);

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

        snapshot.sourceCoverageComplete =
            snapshot.inventoryCaptured &&
            snapshot.inventoryValid &&
            snapshot.certifiedSections == 7 &&
            snapshot.pendingSections == 0 &&
            snapshot.failedSections == 0 &&
            snapshot.absentUnsupportedSections == 0 &&
            snapshot.inventoryUnavailableSections == 0;

        return snapshot;
    }

    void LogCoverage(const char* phase)
    {
        const CoverageSnapshot snapshot =
            GetCoverageSnapshot();

        LOG_INFO(
            "PersistentIdFix: source reads [%s]: generation=%llu successful=%llu recognized=%llu failed=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.loadGeneration),
            static_cast<unsigned long long>(snapshot.successfulReads),
            static_cast<unsigned long long>(snapshot.recognizedSuccessfulReads),
            static_cast<unsigned long long>(snapshot.failedReads));

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

        LOG_INFO(
            "PersistentIdFix: source coverage [%s]: complete=%u certified=%llu pending=%llu failed=%llu absentUnsupported=%llu inventoryUnavailable=%llu",
            phase != nullptr ? phase : "<null>",
            snapshot.sourceCoverageComplete ? 1u : 0u,
            static_cast<unsigned long long>(snapshot.certifiedSections),
            static_cast<unsigned long long>(snapshot.pendingSections),
            static_cast<unsigned long long>(snapshot.failedSections),
            static_cast<unsigned long long>(snapshot.absentUnsupportedSections),
            static_cast<unsigned long long>(snapshot.inventoryUnavailableSections));

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
        ResetObservationCounters();

        g_gameWorldAttached.store(false, std::memory_order_relaxed);
        g_attachedLoadGeneration.store(0, std::memory_order_relaxed);

        const std::uint64_t generation =
            g_loadGeneration.fetch_add(
                1,
                std::memory_order_relaxed) + 1;

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
        ResetObservationCounters();
        g_gameWorldAttached.store(false, std::memory_order_relaxed);
        g_attachedLoadGeneration.store(0, std::memory_order_relaxed);
        g_loadGeneration.store(0, std::memory_order_relaxed);
    }
}

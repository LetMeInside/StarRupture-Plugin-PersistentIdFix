#include "SourceReadObserver.h"

#include "MassFragmentClassifier.h"
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

    std::atomic<std::uint64_t> g_successfulReads{ 0 };
    std::atomic<std::uint64_t> g_recognizedSuccessfulReads{ 0 };
    std::atomic<std::uint64_t> g_failedReads{ 0 };

    std::atomic<std::uint64_t> g_massReads{ 0 };
    std::atomic<std::uint64_t> g_buildingCustomNameReads{ 0 };
    std::atomic<std::uint64_t> g_gameStateDataReads{ 0 };
    std::atomic<std::uint64_t> g_antennasDataReads{ 0 };
    std::atomic<std::uint64_t> g_ziplineReplicatorReads{ 0 };
    std::atomic<std::uint64_t> g_ziplineSubsystemReads{ 0 };
    std::atomic<std::uint64_t> g_baseCoreReplicationHelperReads{ 0 };

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

        return snapshot;
    }

    void LogCoverage(const char* phase)
    {
        const CoverageSnapshot snapshot =
            GetCoverageSnapshot();

        LOG_INFO(
            "PersistentIdFix: source reads [%s]: successful=%llu recognized=%llu failed=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.successfulReads),
            static_cast<unsigned long long>(snapshot.recognizedSuccessfulReads),
            static_cast<unsigned long long>(snapshot.failedReads));

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
    }

    void Reset()
    {
        g_successfulReads.store(
            0,
            std::memory_order_relaxed);

        g_recognizedSuccessfulReads.store(
            0,
            std::memory_order_relaxed);

        g_failedReads.store(
            0,
            std::memory_order_relaxed);

        g_massReads.store(
            0,
            std::memory_order_relaxed);

        g_buildingCustomNameReads.store(
            0,
            std::memory_order_relaxed);

        g_gameStateDataReads.store(
            0,
            std::memory_order_relaxed);

        g_antennasDataReads.store(
            0,
            std::memory_order_relaxed);

        g_ziplineReplicatorReads.store(
            0,
            std::memory_order_relaxed);

        g_ziplineSubsystemReads.store(
            0,
            std::memory_order_relaxed);

        g_baseCoreReplicationHelperReads.store(
            0,
            std::memory_order_relaxed);

        PersistentIdFixMassFragmentClassifier::Reset();
    }
}

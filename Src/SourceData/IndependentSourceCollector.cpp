#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "IndependentSourceCollector.h"

#include "plugin.h"
#include "plugin_helpers.h"

#include "SDK/Chimera_structs.hpp"
#include "SDK/CoreUObject_classes.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

namespace
{
    using namespace SDK;
    using PersistentIdFixIndependentSourceCollector::Section;
    using PersistentIdFixIndependentSourceCollector::SectionSnapshot;
    using PersistentIdFixIndependentSourceCollector::Snapshot;

    static_assert(sizeof(FBuildingCustomNameSaveData) == 0x58);
    static_assert(sizeof(FCrAntennaSaveData) == 0x50);
    static_assert(sizeof(FCrZiplineReplicatorSaveData) == 0x50);
    static_assert(sizeof(FCrZiplineSaveData) == 0x50);
    static_assert(sizeof(FBaseCoreReplicationSaveData) == 0x38);
    static_assert(sizeof(FCrBaseCoreSaveData) == 0x28);

    struct SectionState
    {
        std::atomic<std::uint64_t> ReadFailures{0};
        std::atomic<std::uint64_t> Attempts{0};
        std::atomic<std::uint64_t> Successes{0};
        std::atomic<std::uint64_t> Failures{0};
        std::atomic<std::uint64_t> SchemaMismatches{0};
        std::atomic<std::uint64_t> Values{0};
        std::atomic<std::uint64_t> ZeroValues{0};
        std::atomic<std::uint64_t> InvalidSentinels{0};
        std::atomic<std::uint64_t> ContainerFailures{0};
    };

    std::array<SectionState, 5> g_states;

    std::mutex g_valuesMutex;
    std::array<std::vector<std::uint32_t>, 5> g_values;

    struct DescriptorProfile
    {
        Section SectionId;
        const wchar_t* Path;
        std::int32_t ExpectedSize;
    };

    constexpr std::array<DescriptorProfile, 5> kProfiles{{
        { Section::BuildingCustomNames, L"/Script/Chimera.BuildingCustomNameSaveData", 0x58 },
        { Section::AntennasData, L"/Script/Chimera.CrAntennaSaveData", 0x50 },
        { Section::ZiplineReplicator, L"/Script/Chimera.CrZiplineReplicatorSaveData", 0x50 },
        { Section::ZiplineSubsystem, L"/Script/Chimera.CrZiplineSaveData", 0x50 },
        { Section::BaseCoreReplication, L"/Script/Chimera.BaseCoreReplicationSaveData", 0x38 }
    }};

    using StaticFindObjectSafeByNameFn =
        UObject* (__fastcall*)(UClass*, UObject*, const wchar_t*, bool);

    std::mutex g_registryMutex;
    std::array<const UScriptStruct*, 5> g_descriptors{};
    bool g_registryReady = false;

    std::size_t Index(Section section)
    {
        return static_cast<std::size_t>(section);
    }

    SectionState& State(Section section)
    {
        return g_states[Index(section)];
    }

    bool IsReadableRange(const void* pointer, std::size_t byteCount)
    {
        if (byteCount == 0)
            return true;
        if (pointer == nullptr)
            return false;

        const auto start = reinterpret_cast<std::uintptr_t>(pointer);
        if (start > (std::numeric_limits<std::uintptr_t>::max)() - byteCount)
            return false;

        const auto end = start + byteCount;
        auto cursor = start;

        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(
                    reinterpret_cast<const void*>(cursor),
                    &info,
                    sizeof(info)) == 0)
            {
                return false;
            }

            if (info.State != MEM_COMMIT ||
                (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            {
                return false;
            }

            const DWORD readable =
                PAGE_READONLY |
                PAGE_READWRITE |
                PAGE_WRITECOPY |
                PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE |
                PAGE_EXECUTE_WRITECOPY;

            if ((info.Protect & readable) == 0)
                return false;

            const auto regionStart =
                reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            const auto regionSize =
                static_cast<std::uintptr_t>(info.RegionSize);

            if (regionStart >
                (std::numeric_limits<std::uintptr_t>::max)() - regionSize)
            {
                return false;
            }

            const auto regionEnd = regionStart + regionSize;
            if (regionEnd <= cursor)
                return false;

            cursor = regionEnd < end ? regionEnd : end;
        }

        return true;
    }

    template <typename T>
    bool ValidateArray(const TArray<T>& values)
    {
        const std::int32_t count = values.Num();
        const std::int32_t capacity = values.Max();

        if (count < 0 || capacity < 0 || count > capacity)
            return false;

        const T* data = values.GetDataPtr();
        if (capacity > 0 && data == nullptr)
            return false;
        if (count == 0)
            return true;

        if (data == nullptr ||
            (reinterpret_cast<std::uintptr_t>(data) % alignof(T)) != 0)
        {
            return false;
        }

        const auto countWide = static_cast<std::size_t>(count);
        if (countWide >
            (std::numeric_limits<std::size_t>::max)() / sizeof(T))
        {
            return false;
        }

        return IsReadableRange(data, countWide * sizeof(T));
    }

    void ContainerFailure(Section section)
    {
        State(section).ContainerFailures.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    template <typename T, typename Visitor>
    bool CheckedEach(
        Section section,
        const TArray<T>& values,
        Visitor&& visitor)
    {
        if (!ValidateArray(values))
        {
            ContainerFailure(section);
            return false;
        }

        for (std::int32_t index = 0; index < values.Num(); ++index)
        {
            if (!values.IsValidIndex(index))
            {
                ContainerFailure(section);
                return false;
            }
            if (!visitor(values[index]))
                return false;
        }

        return true;
    }

    template <typename K, typename V, typename Visitor>
    bool CheckedSparseMap(
        Section section,
        const TMap<K, V>& values,
        Visitor&& visitor)
    {
        const std::int32_t live = values.Num();
        const std::int32_t allocated = values.NumAllocated();
        const std::int32_t capacity = values.Max();

        if (live < 0 ||
            allocated < 0 ||
            capacity < 0 ||
            live > allocated ||
            allocated > capacity)
        {
            ContainerFailure(section);
            return false;
        }

        const auto& flags = values.GetAllocationFlags();
        if (flags.Num() < 0 ||
            flags.Max() < 0 ||
            flags.Num() != allocated ||
            flags.Max() < flags.Num())
        {
            ContainerFailure(section);
            return false;
        }

        if (allocated == 0)
        {
            if (live != 0)
            {
                ContainerFailure(section);
                return false;
            }
            return true;
        }

        if (!values.IsValid() || flags.GetData() == nullptr)
        {
            ContainerFailure(section);
            return false;
        }

        const auto wordCount =
            (static_cast<std::size_t>(allocated) + 31u) / 32u;

        if (wordCount >
            (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t) ||
            !IsReadableRange(
                flags.GetData(),
                wordCount * sizeof(std::uint32_t)))
        {
            ContainerFailure(section);
            return false;
        }

        struct SparseDataHeader
        {
            const void* Data;
            std::int32_t Num;
            std::int32_t Max;
        };
        static_assert(sizeof(SparseDataHeader) == 0x10);

        using PairType = typename TMap<K, V>::ElementType;
        using SlotType = UC::ContainerImpl::SetElement<PairType>;

        const auto& sparseData =
            *reinterpret_cast<const SparseDataHeader*>(&values);

        if (sparseData.Num != allocated ||
            sparseData.Max != capacity ||
            sparseData.Data == nullptr ||
            (reinterpret_cast<std::uintptr_t>(sparseData.Data) %
                alignof(SlotType)) != 0)
        {
            ContainerFailure(section);
            return false;
        }

        const auto allocatedWide =
            static_cast<std::size_t>(allocated);

        if (allocatedWide >
            (std::numeric_limits<std::size_t>::max)() / sizeof(SlotType) ||
            !IsReadableRange(
                sparseData.Data,
                allocatedWide * sizeof(SlotType)))
        {
            ContainerFailure(section);
            return false;
        }

        std::int32_t visited = 0;
        for (auto it = begin(values); it != end(values); ++it)
        {
            ++visited;
            if (!visitor(*it))
                return false;
        }

        if (visited != live)
        {
            ContainerFailure(section);
            return false;
        }

        return true;
    }

    bool Emit(
        std::vector<std::uint32_t>& staged,
        std::uint32_t value)
    {
        try
        {
            staged.push_back(value);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ValidateRoot(
        Section section,
        const UScriptStruct* actual,
        const void* destination)
    {
        const std::size_t index = Index(section);

        if (!g_registryReady ||
            actual == nullptr ||
            destination == nullptr ||
            actual != g_descriptors[index] ||
            actual->Size != kProfiles[index].ExpectedSize)
        {
            State(section).SchemaMismatches.fetch_add(
                1,
                std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    bool Commit(
        Section section,
        std::vector<std::uint32_t>& staged)
    {
        std::uint64_t zeros = 0;
        std::uint64_t sentinels = 0;

        for (const std::uint32_t value : staged)
        {
            if (value == 0)
                ++zeros;
            if (value == (std::numeric_limits<std::uint32_t>::max)())
                ++sentinels;
        }

        try
        {
            std::lock_guard<std::mutex> lock(g_valuesMutex);
            auto& destination = g_values[Index(section)];
            destination.insert(
                destination.end(),
                staged.begin(),
                staged.end());
        }
        catch (...)
        {
            return false;
        }

        SectionState& state = State(section);
        state.Values.fetch_add(
            static_cast<std::uint64_t>(staged.size()),
            std::memory_order_relaxed);
        state.ZeroValues.fetch_add(zeros, std::memory_order_relaxed);
        state.InvalidSentinels.fetch_add(
            sentinels,
            std::memory_order_relaxed);
        state.Successes.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    template <typename Collector>
    bool Observe(
        Section section,
        const UScriptStruct* actual,
        const void* destination,
        Collector&& collector)
    {
        SectionState& state = State(section);
        state.Attempts.fetch_add(1, std::memory_order_relaxed);

        if (!ValidateRoot(section, actual, destination))
        {
            state.Failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::vector<std::uint32_t> staged;

        try
        {
            if (!collector(staged) || !Commit(section, staged))
            {
                state.Failures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            return true;
        }
        catch (...)
        {
            state.Failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    SectionSnapshot SnapshotOf(Section section)
    {
        const SectionState& state = g_states[Index(section)];
        SectionSnapshot result;
        result.readFailures = state.ReadFailures.load(std::memory_order_relaxed);
        result.attempts = state.Attempts.load(std::memory_order_relaxed);
        result.successes = state.Successes.load(std::memory_order_relaxed);
        result.failures = state.Failures.load(std::memory_order_relaxed);
        result.schemaMismatches = state.SchemaMismatches.load(std::memory_order_relaxed);
        result.values = state.Values.load(std::memory_order_relaxed);
        result.zeroValues = state.ZeroValues.load(std::memory_order_relaxed);
        result.invalidSentinels = state.InvalidSentinels.load(std::memory_order_relaxed);
        result.containerFailures = state.ContainerFailures.load(std::memory_order_relaxed);
        return result;
    }

    void ResetSectionState(Section section)
    {
        SectionState& state = State(section);
        state.ReadFailures.store(0, std::memory_order_relaxed);
        state.Attempts.store(0, std::memory_order_relaxed);
        state.Successes.store(0, std::memory_order_relaxed);
        state.Failures.store(0, std::memory_order_relaxed);
        state.SchemaMismatches.store(0, std::memory_order_relaxed);
        state.Values.store(0, std::memory_order_relaxed);
        state.ZeroValues.store(0, std::memory_order_relaxed);
        state.InvalidSentinels.store(0, std::memory_order_relaxed);
        state.ContainerFailures.store(0, std::memory_order_relaxed);
    }

    void LogSection(
        const char* phase,
        const char* name,
        const SectionSnapshot& snapshot)
    {
        LOG_INFO(
            "PersistentIdFix: source collector %s [%s]: readFailed=%llu attempts=%llu success=%llu failed=%llu schemaMismatch=%llu values=%llu zero=%llu invalidSentinel=%llu containerFailures=%llu",
            name,
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.readFailures),
            static_cast<unsigned long long>(snapshot.attempts),
            static_cast<unsigned long long>(snapshot.successes),
            static_cast<unsigned long long>(snapshot.failures),
            static_cast<unsigned long long>(snapshot.schemaMismatches),
            static_cast<unsigned long long>(snapshot.values),
            static_cast<unsigned long long>(snapshot.zeroValues),
            static_cast<unsigned long long>(snapshot.invalidSentinels),
            static_cast<unsigned long long>(snapshot.containerFailures));
    }
}

namespace PersistentIdFixIndependentSourceCollector
{
    bool InitializeDescriptorRegistry(IPluginEngineEvents* engineEvents)
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);

        g_registryReady = false;
        g_descriptors.fill(nullptr);

        if (engineEvents == nullptr ||
            engineEvents->GetStaticFindObjectSafeByNameAddress == nullptr)
        {
            return false;
        }

        const uintptr_t resolverAddress =
            engineEvents->GetStaticFindObjectSafeByNameAddress();

        if (resolverAddress == 0)
            return false;

        const auto findSafe =
            reinterpret_cast<StaticFindObjectSafeByNameFn>(resolverAddress);

        UObject* scriptStructClassObject =
            findSafe(nullptr, nullptr, L"/Script/CoreUObject.ScriptStruct", true);

        if (scriptStructClassObject == nullptr)
            return false;

        UClass* scriptStructClass =
            reinterpret_cast<UClass*>(scriptStructClassObject);

        for (const DescriptorProfile& profile : kProfiles)
        {
            UObject* object =
                findSafe(
                    scriptStructClass,
                    nullptr,
                    profile.Path,
                    true);

            if (object == nullptr)
            {
                LOG_WARN(
                    "PersistentIdFix: independent descriptor lookup failed for section index=%llu",
                    static_cast<unsigned long long>(Index(profile.SectionId)));
                g_descriptors.fill(nullptr);
                return false;
            }

            const UScriptStruct* descriptor =
                reinterpret_cast<const UScriptStruct*>(object);

            if (descriptor->Size != profile.ExpectedSize)
            {
                LOG_WARN(
                    "PersistentIdFix: independent descriptor schema mismatch section=%llu size=%d expected=%d",
                    static_cast<unsigned long long>(Index(profile.SectionId)),
                    descriptor->Size,
                    profile.ExpectedSize);
                g_descriptors.fill(nullptr);
                return false;
            }

            g_descriptors[Index(profile.SectionId)] = descriptor;
        }

        g_registryReady = true;

        LOG_INFO(
            "PersistentIdFix: independent fixed-source descriptor registry ready: profiles=5");

        return true;
    }

    void ResetDescriptorRegistry()
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        g_registryReady = false;
        g_descriptors.fill(nullptr);
    }

    bool IsDescriptorRegistryReady()
    {
        return g_registryReady;
    }

    void RecordReadFailure(Section section)
    {
        State(section).ReadFailures.fetch_add(
            1,
            std::memory_order_relaxed);
    }

    bool ObserveBuildingCustomNames(
        const UScriptStruct* structType,
        const FBuildingCustomNameSaveData* data)
    {
        return Observe(
            Section::BuildingCustomNames,
            structType,
            data,
            [&](std::vector<std::uint32_t>& staged)
            {
                return CheckedSparseMap(
                    Section::BuildingCustomNames,
                    data->CustomNames,
                    [&](const auto& pair)
                    {
                        return Emit(staged, pair.Key().ID);
                    });
            });
    }

    bool ObserveAntennas(
        const UScriptStruct* structType,
        const FCrAntennaSaveData* data)
    {
        return Observe(
            Section::AntennasData,
            structType,
            data,
            [&](std::vector<std::uint32_t>& staged)
            {
                return CheckedSparseMap(
                    Section::AntennasData,
                    data->Antennas,
                    [&](const auto& pair)
                    {
                        return Emit(staged, pair.Key().ID);
                    });
            });
    }

    bool ObserveZiplineReplicator(
        const UScriptStruct* structType,
        const FCrZiplineReplicatorSaveData* data)
    {
        return Observe(
            Section::ZiplineReplicator,
            structType,
            data,
            [&](std::vector<std::uint32_t>& staged)
            {
                return CheckedSparseMap(
                    Section::ZiplineReplicator,
                    data->ZiplinesActivity,
                    [&](const auto& pair)
                    {
                        return Emit(staged, pair.Key().ID);
                    });
            });
    }

    bool ObserveZiplineSubsystem(
        const UScriptStruct* structType,
        const FCrZiplineSaveData* data)
    {
        return Observe(
            Section::ZiplineSubsystem,
            structType,
            data,
            [&](std::vector<std::uint32_t>& staged)
            {
                return CheckedSparseMap(
                    Section::ZiplineSubsystem,
                    data->ZiplineConnections,
                    [&](const auto& pair)
                    {
                        if (!Emit(staged, pair.Key().ID))
                            return false;

                        return CheckedEach(
                            Section::ZiplineSubsystem,
                            pair.Value().Values,
                            [&](const FCrMassPersistentEntityID& value)
                            {
                                return Emit(staged, value.ID);
                            });
                    });
            });
    }

    bool ObserveBaseCoreReplication(
        const UScriptStruct* structType,
        const FBaseCoreReplicationSaveData* data)
    {
        return Observe(
            Section::BaseCoreReplication,
            structType,
            data,
            [&](std::vector<std::uint32_t>& staged)
            {
                if (!Emit(
                        staged,
                        data->BaseCoreAwaitingForBaseAttackAfterUpgradeSaved.ID) ||
                    !Emit(
                        staged,
                        data->CurrentBaseCoreBaseAttackTargetSaved.ID))
                {
                    return false;
                }

                return CheckedEach(
                    Section::BaseCoreReplication,
                    data->BaseCoreSaveData,
                    [&](const FCrBaseCoreSaveData& value)
                    {
                        return Emit(staged, value.BaseCore.ID);
                    });
            });
    }

    Snapshot GetSnapshot()
    {
        Snapshot result;
        result.buildingCustomNames = SnapshotOf(Section::BuildingCustomNames);
        result.antennasData = SnapshotOf(Section::AntennasData);
        result.ziplineReplicator = SnapshotOf(Section::ZiplineReplicator);
        result.ziplineSubsystem = SnapshotOf(Section::ZiplineSubsystem);
        result.baseCoreReplication = SnapshotOf(Section::BaseCoreReplication);
        return result;
    }

    void LogSnapshot(const char* phase)
    {
        const Snapshot snapshot = GetSnapshot();

        LogSection(
            phase,
            "CustomNames",
            snapshot.buildingCustomNames);
        LogSection(
            phase,
            "Antennas",
            snapshot.antennasData);
        LogSection(
            phase,
            "ZiplineReplicator",
            snapshot.ziplineReplicator);
        LogSection(
            phase,
            "ZiplineSubsystem",
            snapshot.ziplineSubsystem);
        LogSection(
            phase,
            "BaseCoreReplication",
            snapshot.baseCoreReplication);
    }

    void Reset()
    {
        for (std::size_t i = 0; i < g_states.size(); ++i)
            ResetSectionState(static_cast<Section>(i));

        std::lock_guard<std::mutex> lock(g_valuesMutex);
        for (auto& values : g_values)
            values.clear();
    }
}

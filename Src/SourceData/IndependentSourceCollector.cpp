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
#include <string>
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
    static_assert(sizeof(FGameStateSaveData) == 0x490);
    static_assert(sizeof(FCrCharacterPlayersBaseSaveData) == 0x50);
    static_assert(sizeof(FCrCharacterPlayerBaseSaveDataPerPlayer) == 0x6F0);
    static_assert(sizeof(FCrPlayersMapMenuState) == 0x50);

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
        std::atomic<std::uint64_t> IncompleteCoverage{0};
    };

    std::array<SectionState, 6> g_states;

    std::mutex g_valuesMutex;
    std::array<std::vector<std::uint32_t>, 6> g_values;

    struct DescriptorProfile
    {
        Section SectionId;
        const wchar_t* Path;
        std::int32_t ExpectedSize;
    };

    constexpr std::array<DescriptorProfile, 6> kProfiles{{
        { Section::BuildingCustomNames, L"/Script/Chimera.BuildingCustomNameSaveData", 0x58 },
        { Section::AntennasData, L"/Script/Chimera.CrAntennaSaveData", 0x50 },
        { Section::ZiplineReplicator, L"/Script/Chimera.CrZiplineReplicatorSaveData", 0x50 },
        { Section::ZiplineSubsystem, L"/Script/Chimera.CrZiplineSaveData", 0x50 },
        { Section::BaseCoreReplication, L"/Script/Chimera.BaseCoreReplicationSaveData", 0x38 },
        { Section::GameStateData, L"/Script/Chimera.GameStateSaveData", 0x490 }
    }};

    using StaticFindObjectSafeByNameFn =
        UObject* (__fastcall*)(UClass*, UObject*, const wchar_t*, bool);

    std::mutex g_registryMutex;
    std::array<const UScriptStruct*, 6> g_descriptors{};
    bool g_registryReady = false;

    std::atomic<std::uint64_t> g_gameStatePlayers{0};
    std::atomic<std::uint64_t> g_gameStateFloorValues{0};
    std::atomic<std::uint64_t> g_gameStateAntennaFogValues{0};
    std::atomic<std::uint64_t> g_gameStateDevicePayloads{0};
    std::atomic<std::uint64_t> g_gameStateDeviceEmpty{0};
    std::atomic<std::uint64_t> g_gameStateDeviceMalformed{0};
    std::atomic<std::uint64_t> g_gameStateDeviceUnknownPresent{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowSupported{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowUnsupported{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowProperties{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowArrays{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowStructs{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowIntegralLeaves{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowEnumLeaves{0};
    std::atomic<std::uint64_t> g_gameStateDeviceShadowMaxDepth{0};
    std::atomic<std::uint64_t> g_gameStateOpaqueStoreEntries{0};
    std::atomic<std::uint64_t> g_gameStateDiscoveredBuildingValues{0};

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

    constexpr std::uint32_t kShadowMaxStructDepth = 16;
    constexpr std::uint64_t kShadowMaxProperties = 512;

    struct ShadowSchemaSummary
    {
        bool supported = true;
        std::uint64_t properties = 0;
        std::uint64_t arrays = 0;
        std::uint64_t structs = 0;
        std::uint64_t integralLeaves = 0;
        std::uint64_t enumLeaves = 0;
        std::uint64_t unsupported = 0;
        std::uint64_t maxDepth = 0;
    };

    bool HasCastFlag(
        const FField* field,
        EClassCastFlags flag)
    {
        if (field == nullptr ||
            !IsReadableRange(field, sizeof(FField)) ||
            field->ClassPrivate == nullptr ||
            !IsReadableRange(
                field->ClassPrivate,
                sizeof(FFieldClass)))
        {
            return false;
        }

        return
            (field->ClassPrivate->CastFlags &
                static_cast<std::uint64_t>(flag)) != 0;
    }

    bool IsSupportedIntegralProperty(
        const FProperty* property)
    {
        if (property == nullptr)
            return false;

        return
            HasCastFlag(property, EClassCastFlags::Int8Property) ||
            HasCastFlag(property, EClassCastFlags::ByteProperty) ||
            HasCastFlag(property, EClassCastFlags::Int16Property) ||
            HasCastFlag(property, EClassCastFlags::UInt16Property) ||
            HasCastFlag(property, EClassCastFlags::IntProperty) ||
            HasCastFlag(property, EClassCastFlags::UInt32Property) ||
            HasCastFlag(property, EClassCastFlags::Int64Property) ||
            HasCastFlag(property, EClassCastFlags::UInt64Property) ||
            HasCastFlag(property, EClassCastFlags::BoolProperty);
    }

    bool AnalyzeShadowPropertyShape(
        const FProperty* property,
        std::uint32_t depth,
        ShadowSchemaSummary& summary);

    bool AnalyzeShadowStructShape(
        const UStruct* descriptor,
        std::uint32_t depth,
        ShadowSchemaSummary& summary)
    {
        if (descriptor == nullptr ||
            depth > kShadowMaxStructDepth ||
            !IsReadableRange(descriptor, sizeof(UStruct)) ||
            descriptor->Size <= 0)
        {
            summary.supported = false;
            ++summary.unsupported;
            return false;
        }

        if (depth > summary.maxDepth)
            summary.maxDepth = depth;

        if (descriptor->SuperStruct != nullptr)
        {
            if (!AnalyzeShadowStructShape(
                    descriptor->SuperStruct,
                    depth + 1u,
                    summary))
            {
                return false;
            }
        }

        const FField* field = descriptor->ChildProperties;
        std::uint64_t chainGuard = 0;

        while (field != nullptr)
        {
            if (++chainGuard > kShadowMaxProperties ||
                summary.properties >= kShadowMaxProperties ||
                !IsReadableRange(field, sizeof(FField)) ||
                !HasCastFlag(field, EClassCastFlags::Property))
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            const auto* property =
                reinterpret_cast<const FProperty*>(field);

            if (!IsReadableRange(property, sizeof(FProperty)) ||
                property->ArrayDim <= 0 ||
                property->ElementSize <= 0 ||
                property->Offset < 0)
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            const std::uint64_t offset =
                static_cast<std::uint64_t>(property->Offset);
            const std::uint64_t elementSize =
                static_cast<std::uint64_t>(property->ElementSize);
            const std::uint64_t arrayDim =
                static_cast<std::uint64_t>(property->ArrayDim);

            if (arrayDim >
                    (std::numeric_limits<std::uint64_t>::max)() /
                        elementSize)
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            const std::uint64_t byteCount =
                elementSize * arrayDim;

            if (offset >
                    static_cast<std::uint64_t>(descriptor->Size) ||
                byteCount >
                    static_cast<std::uint64_t>(descriptor->Size) -
                        offset)
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            ++summary.properties;

            if (!AnalyzeShadowPropertyShape(
                    property,
                    depth,
                    summary))
            {
                return false;
            }

            field = field->Next;
        }

        return true;
    }

    bool AnalyzeShadowPropertyShape(
        const FProperty* property,
        std::uint32_t depth,
        ShadowSchemaSummary& summary)
    {
        if (property == nullptr ||
            depth > kShadowMaxStructDepth ||
            !IsReadableRange(property, sizeof(FProperty)))
        {
            summary.supported = false;
            ++summary.unsupported;
            return false;
        }

        if (HasCastFlag(property, EClassCastFlags::ArrayProperty))
        {
            if (!IsReadableRange(
                    property,
                    sizeof(FArrayProperty)))
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            const auto* arrayProperty =
                static_cast<const FArrayProperty*>(property);

            if (arrayProperty->InnerProperty == nullptr)
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            ++summary.arrays;

            return AnalyzeShadowPropertyShape(
                arrayProperty->InnerProperty,
                depth + 1u,
                summary);
        }

        if (HasCastFlag(property, EClassCastFlags::StructProperty))
        {
            if (!IsReadableRange(
                    property,
                    sizeof(FStructProperty)))
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            const auto* structProperty =
                static_cast<const FStructProperty*>(property);

            if (structProperty->Struct == nullptr)
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            ++summary.structs;

            return AnalyzeShadowStructShape(
                structProperty->Struct,
                depth + 1u,
                summary);
        }

        if (HasCastFlag(property, EClassCastFlags::EnumProperty))
        {
            if (!IsReadableRange(
                    property,
                    sizeof(FEnumProperty)))
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            const auto* enumProperty =
                static_cast<const FEnumProperty*>(property);

            if (enumProperty->UnderlayingProperty == nullptr ||
                !IsSupportedIntegralProperty(
                    enumProperty->UnderlayingProperty))
            {
                summary.supported = false;
                ++summary.unsupported;
                return false;
            }

            ++summary.enumLeaves;
            return true;
        }

        if (IsSupportedIntegralProperty(property))
        {
            ++summary.integralLeaves;
            return true;
        }

        summary.supported = false;
        ++summary.unsupported;
        return false;
    }

    bool AnalyzeShadowPayload(
        const FInstancedStruct& payload,
        ShadowSchemaSummary& summary,
        std::string& descriptorName)
    {
        if (payload.ScriptStruct == nullptr ||
            payload.StructMemory == nullptr ||
            !IsReadableRange(
                payload.ScriptStruct,
                sizeof(UScriptStruct)) ||
            payload.ScriptStruct->Size <= 0 ||
            !IsReadableRange(
                payload.StructMemory,
                static_cast<std::size_t>(
                    payload.ScriptStruct->Size)))
        {
            summary.supported = false;
            ++summary.unsupported;
            return false;
        }

        try
        {
            descriptorName =
                payload.ScriptStruct->Name.ToString();
        }
        catch (...)
        {
            descriptorName = "<name-unavailable>";
        }

        return AnalyzeShadowStructShape(
            payload.ScriptStruct,
            0,
            summary);
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
        result.incompleteCoverage = state.IncompleteCoverage.load(std::memory_order_relaxed);
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
        state.IncompleteCoverage.store(0, std::memory_order_relaxed);
    }

    void LogSection(
        const char* phase,
        const char* name,
        const SectionSnapshot& snapshot)
    {
        LOG_INFO(
            "PersistentIdFix: source collector %s [%s]: readFailed=%llu attempts=%llu success=%llu failed=%llu schemaMismatch=%llu values=%llu zero=%llu invalidSentinel=%llu containerFailures=%llu incomplete=%llu",
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
            static_cast<unsigned long long>(snapshot.containerFailures),
            static_cast<unsigned long long>(snapshot.incompleteCoverage));
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
            "PersistentIdFix: independent source descriptor registry ready: profiles=6");

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

    bool ObserveGameState(
        const UScriptStruct* structType,
        const FGameStateSaveData* data)
    {
        SectionState& state = State(Section::GameStateData);
        state.Attempts.fetch_add(1, std::memory_order_relaxed);

        if (!ValidateRoot(Section::GameStateData, structType, data))
        {
            state.Failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::vector<std::uint32_t> staged;
        std::uint64_t players = 0;
        std::uint64_t floorValues = 0;
        std::uint64_t antennaFogValues = 0;
        std::uint64_t devicePayloads = 0;
        std::uint64_t deviceEmpty = 0;
        std::uint64_t deviceMalformed = 0;
        std::uint64_t deviceUnknownPresent = 0;
        std::uint64_t opaqueStoreEntries = 0;
        std::uint64_t discoveredBuildingValues = 0;

        try
        {
            const auto& savedPlayers =
                data->AllCharactersBaseSaveData.AllPlayersSaveData;

            if (!CheckedSparseMap(
                    Section::GameStateData,
                    savedPlayers,
                    [&](const auto& pair)
                    {
                        const auto& player = pair.Value();
                        ++players;

                        if (!Emit(
                                staged,
                                player.FloorPersistentEntityID.ID))
                        {
                            return false;
                        }
                        ++floorValues;

                        if (!CheckedEach(
                                Section::GameStateData,
                                player.MapMenuState.AntennasUncoveredFogOfWar,
                                [&](const std::uint32_t value)
                                {
                                    if (!Emit(staged, value))
                                        return false;
                                    ++antennaFogValues;
                                    return true;
                                }))
                        {
                            return false;
                        }

                        // The item-instance JSON wrappers are diagnostic-only.
                        // For this audited binary/SDK pair, FAuItemInstance
                        // contributes no base save payload and the only concrete
                        // descendant (FAuWeaponItemInstance) saves only ammo
                        // state and bFirstShotExecuted; none is a persistent-ID
                        // source.
                        const auto countOpaqueMap =
                            [&](const auto& values) -> bool
                            {
                                return CheckedSparseMap(
                                    Section::GameStateData,
                                    values,
                                    [&](const auto&)
                                    {
                                        ++opaqueStoreEntries;
                                        return true;
                                    });
                            };

                        if (!countOpaqueMap(
                                player.GemsStoreState.ItemsInstancesSaveData) ||
                            !countOpaqueMap(
                                player.GemsStoreState.SlotedItemsInstancesSaveData) ||
                            !countOpaqueMap(
                                player.ItemsStoreState.ItemsInstancesSaveData) ||
                            !countOpaqueMap(
                                player.ItemsStoreState.SlotedItemsInstancesSaveData))
                        {
                            return false;
                        }

                        // DiscoveredBuildings contains building-data discovery
                        // identifiers, not entity persistent IDs. Native
                        // DiscoverBuilding reads the int32 identifier from the
                        // supplied building-data object and stores it in this
                        // array; save/load copy the array unchanged.
                        return CheckedEach(
                            Section::GameStateData,
                            player.DiscoveredBuildings,
                            [&](const std::int32_t)
                            {
                                ++discoveredBuildingValues;
                                return true;
                            });
                    }))
            {
                state.Failures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            if (!CheckedSparseMap(
                    Section::GameStateData,
                    data->DevicesCustomData,
                    [&](const auto& pair)
                    {
                        ++devicePayloads;

                        const FInstancedStruct& payload = pair.Value();
                        const bool hasType = payload.ScriptStruct != nullptr;
                        const bool hasMemory = payload.StructMemory != nullptr;

                        if (!hasType && !hasMemory)
                        {
                            ++deviceEmpty;
                            return true;
                        }

                        if (hasType != hasMemory)
                        {
                            ++deviceMalformed;
                            return false;
                        }

                        ShadowSchemaSummary shadow{};
                        std::string descriptorName;

                        const bool shadowSupported =
                            AnalyzeShadowPayload(
                                payload,
                                shadow,
                                descriptorName);

                        if (shadowSupported)
                        {
                            g_gameStateDeviceShadowSupported.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }
                        else
                        {
                            g_gameStateDeviceShadowUnsupported.fetch_add(
                                1,
                                std::memory_order_relaxed);
                        }

                        g_gameStateDeviceShadowProperties.fetch_add(
                            shadow.properties,
                            std::memory_order_relaxed);
                        g_gameStateDeviceShadowArrays.fetch_add(
                            shadow.arrays,
                            std::memory_order_relaxed);
                        g_gameStateDeviceShadowStructs.fetch_add(
                            shadow.structs,
                            std::memory_order_relaxed);
                        g_gameStateDeviceShadowIntegralLeaves.fetch_add(
                            shadow.integralLeaves,
                            std::memory_order_relaxed);
                        g_gameStateDeviceShadowEnumLeaves.fetch_add(
                            shadow.enumLeaves,
                            std::memory_order_relaxed);

                        std::uint64_t observedDepth =
                            g_gameStateDeviceShadowMaxDepth.load(
                                std::memory_order_relaxed);

                        while (observedDepth < shadow.maxDepth &&
                            !g_gameStateDeviceShadowMaxDepth.compare_exchange_weak(
                                observedDepth,
                                shadow.maxDepth,
                                std::memory_order_relaxed))
                        {
                        }

                        LOG_INFO(
                            "PersistentIdFix: device payload reflection shadow: "
                            "schema=%s size=%d supportedShape=%u "
                            "properties=%llu arrays=%llu structs=%llu "
                            "integralLeaves=%llu enumLeaves=%llu "
                            "unsupported=%llu maxDepth=%llu",
                            descriptorName.empty()
                                ? "<unnamed>"
                                : descriptorName.c_str(),
                            payload.ScriptStruct->Size,
                            shadowSupported ? 1u : 0u,
                            static_cast<unsigned long long>(
                                shadow.properties),
                            static_cast<unsigned long long>(
                                shadow.arrays),
                            static_cast<unsigned long long>(
                                shadow.structs),
                            static_cast<unsigned long long>(
                                shadow.integralLeaves),
                            static_cast<unsigned long long>(
                                shadow.enumLeaves),
                            static_cast<unsigned long long>(
                                shadow.unsupported),
                            static_cast<unsigned long long>(
                                shadow.maxDepth));

                        // Shadow-only for now: an otherwise valid payload
                        // remains uncertified until value traversal and the
                        // serialization-representation contract are proven.
                        ++deviceUnknownPresent;
                        return true;
                    }))
            {
                g_gameStateDeviceMalformed.fetch_add(
                    deviceMalformed,
                    std::memory_order_relaxed);
                state.Failures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            if (!Commit(Section::GameStateData, staged))
            {
                state.Failures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            g_gameStatePlayers.fetch_add(players, std::memory_order_relaxed);
            g_gameStateFloorValues.fetch_add(
                floorValues,
                std::memory_order_relaxed);
            g_gameStateAntennaFogValues.fetch_add(
                antennaFogValues,
                std::memory_order_relaxed);
            g_gameStateDevicePayloads.fetch_add(
                devicePayloads,
                std::memory_order_relaxed);
            g_gameStateDeviceEmpty.fetch_add(
                deviceEmpty,
                std::memory_order_relaxed);
            g_gameStateDeviceMalformed.fetch_add(
                deviceMalformed,
                std::memory_order_relaxed);
            g_gameStateDeviceUnknownPresent.fetch_add(
                deviceUnknownPresent,
                std::memory_order_relaxed);
            g_gameStateOpaqueStoreEntries.fetch_add(
                opaqueStoreEntries,
                std::memory_order_relaxed);
            g_gameStateDiscoveredBuildingValues.fetch_add(
                discoveredBuildingValues,
                std::memory_order_relaxed);

            // Item-store wrappers and DiscoveredBuildings are certified
            // non-PID domains for this audited binary/SDK pair. A present
            // unknown DevicesCustomData payload remains fail-closed.
            if (deviceUnknownPresent != 0)
            {
                state.IncompleteCoverage.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }

            return true;
        }
        catch (...)
        {
            state.Failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    Snapshot GetSnapshot()
    {
        Snapshot result;
        result.buildingCustomNames = SnapshotOf(Section::BuildingCustomNames);
        result.antennasData = SnapshotOf(Section::AntennasData);
        result.ziplineReplicator = SnapshotOf(Section::ZiplineReplicator);
        result.ziplineSubsystem = SnapshotOf(Section::ZiplineSubsystem);
        result.baseCoreReplication = SnapshotOf(Section::BaseCoreReplication);
        result.gameStateData = SnapshotOf(Section::GameStateData);

        result.gameStatePlayers =
            g_gameStatePlayers.load(std::memory_order_relaxed);
        result.gameStateFloorValues =
            g_gameStateFloorValues.load(std::memory_order_relaxed);
        result.gameStateAntennaFogValues =
            g_gameStateAntennaFogValues.load(std::memory_order_relaxed);
        result.gameStateDevicePayloads =
            g_gameStateDevicePayloads.load(std::memory_order_relaxed);
        result.gameStateDeviceEmpty =
            g_gameStateDeviceEmpty.load(std::memory_order_relaxed);
        result.gameStateDeviceMalformed =
            g_gameStateDeviceMalformed.load(std::memory_order_relaxed);
        result.gameStateDeviceUnknownPresent =
            g_gameStateDeviceUnknownPresent.load(std::memory_order_relaxed);
        result.gameStateDeviceShadowSupported =
            g_gameStateDeviceShadowSupported.load(std::memory_order_relaxed);
        result.gameStateDeviceShadowUnsupported =
            g_gameStateDeviceShadowUnsupported.load(std::memory_order_relaxed);
        result.gameStateDeviceShadowProperties =
            g_gameStateDeviceShadowProperties.load(std::memory_order_relaxed);
        result.gameStateDeviceShadowArrays =
            g_gameStateDeviceShadowArrays.load(std::memory_order_relaxed);
        result.gameStateDeviceShadowStructs =
            g_gameStateDeviceShadowStructs.load(std::memory_order_relaxed);
        result.gameStateDeviceShadowIntegralLeaves =
            g_gameStateDeviceShadowIntegralLeaves.load(
                std::memory_order_relaxed);
        result.gameStateDeviceShadowEnumLeaves =
            g_gameStateDeviceShadowEnumLeaves.load(
                std::memory_order_relaxed);
        result.gameStateDeviceShadowMaxDepth =
            g_gameStateDeviceShadowMaxDepth.load(std::memory_order_relaxed);
        result.gameStateOpaqueStoreEntries =
            g_gameStateOpaqueStoreEntries.load(std::memory_order_relaxed);
        result.gameStateDiscoveredBuildingValues =
            g_gameStateDiscoveredBuildingValues.load(std::memory_order_relaxed);
        return result;
    }

    bool AppendCollectedValues(
        std::vector<std::uint32_t>& destination)
    {
        try
        {
            std::lock_guard<std::mutex> lock(g_valuesMutex);

            for (const auto& values : g_values)
            {
                destination.insert(
                    destination.end(),
                    values.begin(),
                    values.end());
            }

            return true;
        }
        catch (...)
        {
            return false;
        }
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
        LogSection(
            phase,
            "GameState",
            snapshot.gameStateData);

        LOG_INFO(
            "PersistentIdFix: GameState source details [%s]: players=%llu floor=%llu antennaFog=%llu devicePayloads=%llu deviceEmpty=%llu deviceMalformed=%llu deviceUnknown=%llu opaqueStoreEntries=%llu discoveredBuildings=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.gameStatePlayers),
            static_cast<unsigned long long>(snapshot.gameStateFloorValues),
            static_cast<unsigned long long>(snapshot.gameStateAntennaFogValues),
            static_cast<unsigned long long>(snapshot.gameStateDevicePayloads),
            static_cast<unsigned long long>(snapshot.gameStateDeviceEmpty),
            static_cast<unsigned long long>(snapshot.gameStateDeviceMalformed),
            static_cast<unsigned long long>(snapshot.gameStateDeviceUnknownPresent),
            static_cast<unsigned long long>(snapshot.gameStateOpaqueStoreEntries),
            static_cast<unsigned long long>(snapshot.gameStateDiscoveredBuildingValues));

        LOG_INFO(
            "PersistentIdFix: GameState device reflection shadow [%s]: supported=%llu unsupported=%llu properties=%llu arrays=%llu structs=%llu integralLeaves=%llu enumLeaves=%llu maxDepth=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowSupported),
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowUnsupported),
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowProperties),
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowArrays),
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowStructs),
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowIntegralLeaves),
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowEnumLeaves),
            static_cast<unsigned long long>(
                snapshot.gameStateDeviceShadowMaxDepth));
    }

    void Reset()
    {
        for (std::size_t i = 0; i < g_states.size(); ++i)
            ResetSectionState(static_cast<Section>(i));

        g_gameStatePlayers.store(0, std::memory_order_relaxed);
        g_gameStateFloorValues.store(0, std::memory_order_relaxed);
        g_gameStateAntennaFogValues.store(0, std::memory_order_relaxed);
        g_gameStateDevicePayloads.store(0, std::memory_order_relaxed);
        g_gameStateDeviceEmpty.store(0, std::memory_order_relaxed);
        g_gameStateDeviceMalformed.store(0, std::memory_order_relaxed);
        g_gameStateDeviceUnknownPresent.store(0, std::memory_order_relaxed);
        g_gameStateDeviceShadowSupported.store(0, std::memory_order_relaxed);
        g_gameStateDeviceShadowUnsupported.store(0, std::memory_order_relaxed);
        g_gameStateDeviceShadowProperties.store(0, std::memory_order_relaxed);
        g_gameStateDeviceShadowArrays.store(0, std::memory_order_relaxed);
        g_gameStateDeviceShadowStructs.store(0, std::memory_order_relaxed);
        g_gameStateDeviceShadowIntegralLeaves.store(
            0,
            std::memory_order_relaxed);
        g_gameStateDeviceShadowEnumLeaves.store(
            0,
            std::memory_order_relaxed);
        g_gameStateDeviceShadowMaxDepth.store(0, std::memory_order_relaxed);
        g_gameStateOpaqueStoreEntries.store(0, std::memory_order_relaxed);
        g_gameStateDiscoveredBuildingValues.store(0, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(g_valuesMutex);
        for (auto& values : g_values)
            values.clear();
    }
}

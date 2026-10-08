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
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#ifndef PERSISTENTIDFIX_REFLECTION_TEST_MODE
#define PERSISTENTIDFIX_REFLECTION_TEST_MODE 1
#endif

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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
    const UScriptStruct* g_reflectionTestPersistentIdDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestMapMenuDescriptor = nullptr;
    const UScriptStruct* g_reflectionTestPlayerDescriptor = nullptr;
    // Patch 28: diagnostic-only canonical identities; never used for PID certification.
    const UScriptStruct* g_step28Store = nullptr;
    const UScriptStruct* g_step28ItemId = nullptr;
    const UScriptStruct* g_step28Guid = nullptr;
    const UScriptStruct* g_step28Json = nullptr;
#endif

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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
    // Native Client/Server ABI; the generated FMapProperty stops at 0x80.
    constexpr std::size_t kShadowNativeMapDescriptorSize = 0xA0;
    constexpr std::size_t kShadowNativeMapLayoutOffset = 0x80;
    constexpr std::size_t kShadowNativeMapFlagsOffset = 0x98;
    // Diagnostic work bound, not a claim about the engine's maximum size.
    constexpr std::int32_t kShadowMaxMapSlotStride = 1024 * 1024;

    struct ShadowNativeMapLayout
    {
        std::int32_t ValueOffset = -1;
        std::int32_t HashNextIdOffset = -1;
        std::int32_t HashIndexOffset = -1;
        std::int32_t SetSize = -1;
        std::int32_t SparseAlignment = -1;
        std::int32_t SparseSlotStride = -1;
    };
    static_assert(sizeof(ShadowNativeMapLayout) == 0x18);
    static_assert(offsetof(ShadowNativeMapLayout, ValueOffset) == 0x00);
    static_assert(offsetof(ShadowNativeMapLayout, HashNextIdOffset) == 0x04);
    static_assert(offsetof(ShadowNativeMapLayout, HashIndexOffset) == 0x08);
    static_assert(offsetof(ShadowNativeMapLayout, SetSize) == 0x0C);
    static_assert(offsetof(ShadowNativeMapLayout, SparseAlignment) == 0x10);
    static_assert(offsetof(ShadowNativeMapLayout, SparseSlotStride) == 0x14);
    static_assert(kShadowNativeMapLayoutOffset + sizeof(ShadowNativeMapLayout) ==
        kShadowNativeMapFlagsOffset);
    static_assert(kShadowNativeMapFlagsOffset + sizeof(std::uint8_t) <=
        kShadowNativeMapDescriptorSize);

    struct ShadowNativeMapMetadata
    {
        ShadowNativeMapLayout Layout;
        std::uint8_t AllocatorFlags = 0;
        bool DescriptorReadable = false;
        bool FlagsKnown = false;
        bool AllocatorSupported = false;
        bool LayoutValid = false;
    };

    ShadowNativeMapMetadata ReadShadowNativeMapMetadata(
        const FProperty* property)
    {
        ShadowNativeMapMetadata metadata{};
        metadata.DescriptorReadable =
            IsReadableRange(property, kShadowNativeMapDescriptorSize);
        if (!metadata.DescriptorReadable)
            return metadata;

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(property);
        std::memcpy(
            &metadata.Layout,
            bytes + kShadowNativeMapLayoutOffset,
            sizeof(metadata.Layout));
        std::memcpy(
            &metadata.AllocatorFlags,
            bytes + kShadowNativeMapFlagsOffset,
            sizeof(metadata.AllocatorFlags));

        metadata.FlagsKnown = (metadata.AllocatorFlags & 0xFEu) == 0;
        metadata.AllocatorSupported = metadata.AllocatorFlags == 0;

        const auto& layout = metadata.Layout;
        metadata.LayoutValid =
            layout.SparseSlotStride > 0 &&
            layout.SparseSlotStride <= kShadowMaxMapSlotStride &&
            layout.SparseAlignment > 0 &&
            (layout.SparseAlignment & (layout.SparseAlignment - 1)) == 0 &&
            layout.SparseSlotStride % layout.SparseAlignment == 0 &&
            layout.SetSize >= static_cast<std::int32_t>(sizeof(std::int32_t)) &&
            layout.SetSize <= layout.SparseSlotStride &&
            layout.ValueOffset >= 0 && layout.ValueOffset < layout.SetSize &&
            layout.HashNextIdOffset >= 0 &&
            layout.HashNextIdOffset <= layout.SetSize -
                static_cast<std::int32_t>(sizeof(std::int32_t)) &&
            layout.HashIndexOffset >= 0 &&
            layout.HashIndexOffset <= layout.SetSize -
                static_cast<std::int32_t>(sizeof(std::int32_t));
        return metadata;
    }
#endif

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
        ShadowSchemaSummary& summary)
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

        return AnalyzeShadowStructShape(
            payload.ScriptStruct,
            0,
            summary);
    }

    constexpr std::uint64_t kShadowMaxArrayElements = 1000000;
    constexpr std::uint64_t kShadowMaxValues = 2000000;

    struct ShadowValueSummary
    {
        bool complete = true;
        std::uint64_t exactPersistentIds = 0;
        std::uint64_t integralValues = 0;
        std::uint64_t candidateValues = 0;
        std::uint64_t arrays = 0;
        std::uint64_t arrayElements = 0;
        std::uint64_t structs = 0;
        std::uint64_t unsupported = 0;
        std::uint64_t zeros = 0;
        std::uint64_t invalidSentinels = 0;
        std::uint64_t maxDepth = 0;
        std::uint32_t firstCandidate = 0;
        std::uint32_t lastCandidate = 0;
    };

    struct ShadowScriptArray
    {
        const std::uint8_t* Data;
        std::int32_t Num;
        std::int32_t Max;
    };
    static_assert(sizeof(ShadowScriptArray) == 0x10);

    bool RecordShadowCandidate(
        ShadowValueSummary& summary,
        std::uint32_t value)
    {
        if (value == 0)
        {
            ++summary.zeros;
            return true;
        }

        if (value == UINT32_MAX)
        {
            ++summary.invalidSentinels;
            return true;
        }

        if (summary.candidateValues >= kShadowMaxValues)
        {
            summary.complete = false;
            ++summary.unsupported;
            return false;
        }

        if (summary.candidateValues == 0)
            summary.firstCandidate = value;

        summary.lastCandidate = value;
        ++summary.candidateValues;
        return true;
    }

    bool TraverseShadowPropertyValue(
        const FProperty* property,
        const void* valueStorage,
        std::uint32_t depth,
        const UScriptStruct* persistentIdDescriptor,
        ShadowValueSummary& summary);

    bool TraverseShadowStructValue(
        const UStruct* descriptor,
        const void* storage,
        std::uint32_t depth,
        const UScriptStruct* persistentIdDescriptor,
        ShadowValueSummary& summary)
    {
        if (descriptor == nullptr ||
            storage == nullptr ||
            depth > kShadowMaxStructDepth ||
            !IsReadableRange(descriptor, sizeof(UStruct)) ||
            descriptor->Size <= 0 ||
            !IsReadableRange(
                storage,
                static_cast<std::size_t>(descriptor->Size)))
        {
            summary.complete = false;
            ++summary.unsupported;
            return false;
        }

        if (depth > summary.maxDepth)
            summary.maxDepth = depth;

        if (persistentIdDescriptor != nullptr &&
            descriptor == persistentIdDescriptor)
        {
            if (descriptor->Size < static_cast<std::int32_t>(
                    sizeof(std::uint32_t)) ||
                !IsReadableRange(storage, sizeof(std::uint32_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const std::uint32_t value =
                *reinterpret_cast<const std::uint32_t*>(storage);

            ++summary.exactPersistentIds;
            return RecordShadowCandidate(summary, value);
        }

        if (descriptor->SuperStruct != nullptr)
        {
            // An inherited descriptor must fit within the storage
            // advertised by the derived descriptor.
            if (!IsReadableRange(descriptor->SuperStruct, sizeof(UStruct)) ||
                descriptor->SuperStruct->Size <= 0 ||
                descriptor->SuperStruct->Size > descriptor->Size)
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            if (!TraverseShadowStructValue(
                    descriptor->SuperStruct,
                    storage,
                    depth + 1u,
                    persistentIdDescriptor,
                    summary))
            {
                return false;
            }
        }

        ++summary.structs;

        const FField* field = descriptor->ChildProperties;
        std::uint64_t chainGuard = 0;

        while (field != nullptr)
        {
            if (++chainGuard > kShadowMaxProperties ||
                !IsReadableRange(field, sizeof(FField)) ||
                !HasCastFlag(field, EClassCastFlags::Property))
            {
                summary.complete = false;
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
                summary.complete = false;
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
                summary.complete = false;
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
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            // A reflected nested struct must fit within this property's
            // per-element storage, not merely within readable memory.
            if (HasCastFlag(property, EClassCastFlags::StructProperty))
            {
                if (!IsReadableRange(property, sizeof(FStructProperty)))
                {
                    summary.complete = false;
                    ++summary.unsupported;
                    return false;
                }

                const auto* structProperty =
                    static_cast<const FStructProperty*>(property);
                if (structProperty->Struct == nullptr ||
                    !IsReadableRange(
                        structProperty->Struct,
                        sizeof(UStruct)) ||
                    structProperty->Struct->Size <= 0 ||
                    static_cast<std::uint64_t>(
                        structProperty->Struct->Size) > elementSize)
                {
                    summary.complete = false;
                    ++summary.unsupported;
                    return false;
                }
            }

            const auto* propertyBase =
                static_cast<const std::uint8_t*>(storage) +
                property->Offset;

            for (std::int32_t arrayIndex = 0;
                arrayIndex < property->ArrayDim;
                ++arrayIndex)
            {
                const auto* elementStorage =
                    propertyBase +
                    static_cast<std::size_t>(arrayIndex) *
                        static_cast<std::size_t>(
                            property->ElementSize);

                if (!TraverseShadowPropertyValue(
                        property,
                        elementStorage,
                        depth,
                        persistentIdDescriptor,
                        summary))
                {
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
                    LOG_WARN(
                        "PersistentIdFix: reflection parent failure breadcrumb: "
                        "stage=property depth=%u descriptor=%p descriptorSize=%d "
                        "property=%p offset=%d elementSize=%d arrayDim=%d "
                        "index=%d struct=%u array=%u enum=%u integral=%u "
                        "unsupported=%llu maxDepth=%llu",
                        depth,
                        static_cast<const void*>(descriptor),
                        descriptor->Size,
                        static_cast<const void*>(property),
                        property->Offset,
                        property->ElementSize,
                        property->ArrayDim,
                        arrayIndex,
                        HasCastFlag(property, EClassCastFlags::StructProperty) ? 1u : 0u,
                        HasCastFlag(property, EClassCastFlags::ArrayProperty) ? 1u : 0u,
                        HasCastFlag(property, EClassCastFlags::EnumProperty) ? 1u : 0u,
                        IsSupportedIntegralProperty(property) ? 1u : 0u,
                        static_cast<unsigned long long>(summary.unsupported),
                        static_cast<unsigned long long>(summary.maxDepth));
#endif
                    LOG_WARN(
                        "PersistentIdFix: reflection step28 parent identity: "
                        "depth=%u storeReady=%u exactStore=%u mapOffset16=%u "
                        "mapStorage80=%u playerReady=%u playerExact=%u "
                        "playerOffset=%d",
                        depth,
                        g_step28Store != nullptr ? 1u : 0u,
                        g_step28Store != nullptr &&
                            descriptor == g_step28Store ? 1u : 0u,
                        property->Offset == 0x10 ? 1u : 0u,
                        property->ElementSize == 0x50 ? 1u : 0u,
                        g_reflectionTestPlayerDescriptor != nullptr ? 1u : 0u,
                        g_reflectionTestPlayerDescriptor != nullptr &&
                            descriptor == g_reflectionTestPlayerDescriptor ? 1u : 0u,
                        property->Offset);
                    return false;
                }
            }

            field = field->Next;
        }

        return true;
    }

    bool ReadShadowIntegralValue(
        const FProperty* property,
        const void* storage,
        ShadowValueSummary& summary)
    {
        if (property == nullptr || storage == nullptr) {
            summary.complete = false;
            ++summary.unsupported;
            return false;
        }

        ++summary.integralValues;

        if (HasCastFlag(property, EClassCastFlags::BoolProperty))
            return true;

        if (HasCastFlag(property, EClassCastFlags::Int8Property))
        {
            if (!IsReadableRange(storage, sizeof(std::int8_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const auto value =
                *reinterpret_cast<const std::int8_t*>(storage);

            if (value < 0)
                return true;

            return RecordShadowCandidate(
                summary,
                static_cast<std::uint32_t>(value));
        }

        if (HasCastFlag(property, EClassCastFlags::ByteProperty))
        {
            if (!IsReadableRange(storage, sizeof(std::uint8_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return RecordShadowCandidate(
                summary,
                *reinterpret_cast<const std::uint8_t*>(storage));
        }

        if (HasCastFlag(property, EClassCastFlags::Int16Property))
        {
            if (!IsReadableRange(storage, sizeof(std::int16_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const auto value =
                *reinterpret_cast<const std::int16_t*>(storage);

            if (value < 0)
                return true;

            return RecordShadowCandidate(
                summary,
                static_cast<std::uint32_t>(value));
        }

        if (HasCastFlag(property, EClassCastFlags::UInt16Property))
        {
            if (!IsReadableRange(storage, sizeof(std::uint16_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return RecordShadowCandidate(
                summary,
                *reinterpret_cast<const std::uint16_t*>(storage));
        }

        if (HasCastFlag(property, EClassCastFlags::IntProperty))
        {
            if (!IsReadableRange(storage, sizeof(std::int32_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const auto value =
                *reinterpret_cast<const std::int32_t*>(storage);

            if (value < 0)
                return true;

            return RecordShadowCandidate(
                summary,
                static_cast<std::uint32_t>(value));
        }

        if (HasCastFlag(property, EClassCastFlags::UInt32Property))
        {
            if (!IsReadableRange(storage, sizeof(std::uint32_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return RecordShadowCandidate(
                summary,
                *reinterpret_cast<const std::uint32_t*>(storage));
        }

        if (HasCastFlag(property, EClassCastFlags::Int64Property))
        {
            if (!IsReadableRange(storage, sizeof(std::int64_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const std::int64_t value =
                *reinterpret_cast<const std::int64_t*>(storage);

            if (value >= 0 &&
                static_cast<std::uint64_t>(value) <= UINT32_MAX)
            {
                return RecordShadowCandidate(
                    summary,
                    static_cast<std::uint32_t>(value));
            }

            return true;
        }

        if (HasCastFlag(property, EClassCastFlags::UInt64Property))
        {
            if (!IsReadableRange(storage, sizeof(std::uint64_t)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const std::uint64_t value =
                *reinterpret_cast<const std::uint64_t*>(storage);

            if (value <= UINT32_MAX)
            {
                return RecordShadowCandidate(
                    summary,
                    static_cast<std::uint32_t>(value));
            }

            return true;
        }

        summary.complete = false;
        ++summary.unsupported;
        return false;
    }

    bool TraverseShadowPropertyValue(
        const FProperty* property,
        const void* valueStorage,
        std::uint32_t depth,
        const UScriptStruct* persistentIdDescriptor,
        ShadowValueSummary& summary)
    {
        if (property == nullptr ||
            valueStorage == nullptr ||
            depth > kShadowMaxStructDepth ||
            !IsReadableRange(property, sizeof(FProperty)))
        {
            summary.complete = false;
            ++summary.unsupported;
            return false;
        }

        if (HasCastFlag(property, EClassCastFlags::ArrayProperty))
        {
            if (!IsReadableRange(property, sizeof(FArrayProperty)) ||
                !IsReadableRange(valueStorage, sizeof(ShadowScriptArray)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const auto* arrayProperty =
                static_cast<const FArrayProperty*>(property);

            if (arrayProperty->InnerProperty == nullptr ||
                !IsReadableRange(
                    arrayProperty->InnerProperty,
                    sizeof(FProperty)) ||
                arrayProperty->InnerProperty->ElementSize <= 0)
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const auto& array =
                *reinterpret_cast<const ShadowScriptArray*>(valueStorage);

            if (array.Num < 0 ||
                array.Max < 0 ||
                array.Num > array.Max ||
                static_cast<std::uint64_t>(array.Num) >
                    kShadowMaxArrayElements)
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            ++summary.arrays;

            if (array.Num == 0)
                return true;

            if (array.Data == nullptr)
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const std::size_t stride =
                static_cast<std::size_t>(
                    arrayProperty->InnerProperty->ElementSize);
            const std::size_t count =
                static_cast<std::size_t>(array.Num);

            // Readable neighboring elements cannot make a primitive read
            // valid when the reflected element stride is too small.
            const FProperty* innerProperty =
                arrayProperty->InnerProperty;
            std::size_t minimumIntegralBytes = 0;
            if (HasCastFlag(innerProperty, EClassCastFlags::Int8Property) ||
                HasCastFlag(innerProperty, EClassCastFlags::ByteProperty))
                minimumIntegralBytes = sizeof(std::uint8_t);
            else if (HasCastFlag(innerProperty, EClassCastFlags::Int16Property) ||
                HasCastFlag(innerProperty, EClassCastFlags::UInt16Property))
                minimumIntegralBytes = sizeof(std::uint16_t);
            else if (HasCastFlag(innerProperty, EClassCastFlags::IntProperty) ||
                HasCastFlag(innerProperty, EClassCastFlags::UInt32Property))
                minimumIntegralBytes = sizeof(std::uint32_t);
            else if (HasCastFlag(innerProperty, EClassCastFlags::Int64Property) ||
                HasCastFlag(innerProperty, EClassCastFlags::UInt64Property))
                minimumIntegralBytes = sizeof(std::uint64_t);

            if (minimumIntegralBytes != 0 &&
                stride < minimumIntegralBytes)
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            // A readable allocation alone does not prove that a nested
            // reflected struct fits inside one array element.
            if (HasCastFlag(
                    arrayProperty->InnerProperty,
                    EClassCastFlags::StructProperty))
            {
                const auto* innerStructProperty =
                    reinterpret_cast<const FStructProperty*>(
                        arrayProperty->InnerProperty);
                if (!IsReadableRange(
                        innerStructProperty,
                        sizeof(FStructProperty)) ||
                    innerStructProperty->Struct == nullptr ||
                    !IsReadableRange(
                        innerStructProperty->Struct,
                        sizeof(UStruct)) ||
                    innerStructProperty->Struct->Size <= 0 ||
                    static_cast<std::size_t>(
                        innerStructProperty->Struct->Size) > stride)
                {
                    summary.complete = false;
                    ++summary.unsupported;
                    return false;
                }
            }

            if (count >
                    (std::numeric_limits<std::size_t>::max)() / stride ||
                !IsReadableRange(array.Data, count * stride))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            for (std::size_t index = 0; index < count; ++index)
            {
                if (summary.arrayElements >= kShadowMaxArrayElements)
                {
                    summary.complete = false;
                    ++summary.unsupported;
                    return false;
                }

                ++summary.arrayElements;

                if (!TraverseShadowPropertyValue(
                        arrayProperty->InnerProperty,
                        array.Data + index * stride,
                        depth + 1u,
                        persistentIdDescriptor,
                        summary))
                {
                    return false;
                }
            }

            return true;
        }

        if (HasCastFlag(property, EClassCastFlags::StructProperty))
        {
            if (!IsReadableRange(property, sizeof(FStructProperty)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const auto* structProperty =
                static_cast<const FStructProperty*>(property);

            if (structProperty->Struct == nullptr)
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return TraverseShadowStructValue(
                structProperty->Struct,
                valueStorage,
                depth + 1u,
                persistentIdDescriptor,
                summary);
        }

        if (HasCastFlag(property, EClassCastFlags::EnumProperty))
        {
            if (!IsReadableRange(property, sizeof(FEnumProperty)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            const auto* enumProperty =
                static_cast<const FEnumProperty*>(property);

            if (enumProperty->UnderlayingProperty == nullptr ||
                !IsReadableRange(
                    enumProperty->UnderlayingProperty,
                    sizeof(FProperty)) ||
                enumProperty->ElementSize <= 0 ||
                enumProperty->UnderlayingProperty->ElementSize <= 0 ||
                enumProperty->UnderlayingProperty->ElementSize >
                    enumProperty->ElementSize)
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return ReadShadowIntegralValue(
                enumProperty->UnderlayingProperty,
                valueStorage,
                summary);
        }

        if (IsSupportedIntegralProperty(property))
        {
            return ReadShadowIntegralValue(
                property,
                valueStorage,
                summary);
        }

        // Non-PID floating-point values are skipped, not collected.
        // Validate reflected element width and readable storage first.
        if (HasCastFlag(property, EClassCastFlags::FloatProperty) ||
            HasCastFlag(property, EClassCastFlags::DoubleProperty))
        {
            const std::size_t requiredBytes =
                HasCastFlag(property, EClassCastFlags::DoubleProperty)
                    ? sizeof(double)
                    : sizeof(float);

            if (property->ElementSize <= 0 ||
                static_cast<std::size_t>(property->ElementSize) <
                    requiredBytes ||
                !IsReadableRange(valueStorage, requiredBytes))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return true;
        }

        // Non-PID object references are skipped; their pointer values
        // are not persistent IDs. Never follow or interpret the pointer.
        if (HasCastFlag(property, EClassCastFlags::ObjectProperty))
        {
            if (property->ElementSize !=
                    static_cast<std::int32_t>(sizeof(void*)) ||
                !IsReadableRange(valueStorage, sizeof(void*)))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return true;
        }

        // Non-PID name values are skipped. An FName is a value token,
        // not a Mass persistent entity ID; do not decode its contents.
        if (HasCastFlag(property, EClassCastFlags::NameProperty))
        {
            constexpr std::size_t kExpectedNameBytes =
                sizeof(std::uint64_t);
            if (property->ElementSize !=
                    static_cast<std::int32_t>(kExpectedNameBytes) ||
                !IsReadableRange(valueStorage, kExpectedNameBytes))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return true;
        }

        // Non-PID UTF-8 string values are skipped without accessing payload.
        // Unreal's FUtf8StrProperty cast flag is 0x1000000000000000.
        // Require the expected 16-byte inline field representation and
        // validate storage, retaining fail-closed behavior on mismatch.
        constexpr std::uint64_t kUtf8StringPropertyCastFlag =
            0x1000000000000000ULL;
        if (property->ClassPrivate != nullptr &&
            IsReadableRange(property->ClassPrivate, sizeof(FFieldClass)) &&
            (property->ClassPrivate->CastFlags &
                kUtf8StringPropertyCastFlag) != 0)
        {
            if (property->ElementSize != 16 ||
                !IsReadableRange(valueStorage, 16u))
            {
                summary.complete = false;
                ++summary.unsupported;
                return false;
            }

            return true;
        }

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        // Read-only metadata only: no map storage access or entry traversal.
        if (HasCastFlag(property, EClassCastFlags::MapProperty))
        {
            const auto nativeMetadata = ReadShadowNativeMapMetadata(property);
            const auto& nativeLayout = nativeMetadata.Layout;
            LOG_WARN(
                "PersistentIdFix: reflection native map metadata: "
                "property=%p descriptorReadable=%u nativeDescriptorSize=%llu "
                "valueOffset=%d hashNextIdOffset=%d hashIndexOffset=%d "
                "setSize=%d sparseAlignment=%d sparseSlotStride=%d "
                "allocatorFlags=0x%02X flagsKnown=%u allocatorSupported=%u "
                "layoutValid=%u metadataValid=%u",
                static_cast<const void*>(property),
                nativeMetadata.DescriptorReadable ? 1u : 0u,
                static_cast<unsigned long long>(kShadowNativeMapDescriptorSize),
                nativeLayout.ValueOffset,
                nativeLayout.HashNextIdOffset,
                nativeLayout.HashIndexOffset,
                nativeLayout.SetSize,
                nativeLayout.SparseAlignment,
                nativeLayout.SparseSlotStride,
                static_cast<unsigned int>(nativeMetadata.AllocatorFlags),
                nativeMetadata.FlagsKnown ? 1u : 0u,
                nativeMetadata.AllocatorSupported ? 1u : 0u,
                nativeMetadata.LayoutValid ? 1u : 0u,
                nativeMetadata.DescriptorReadable &&
                    nativeMetadata.FlagsKnown &&
                    nativeMetadata.AllocatorSupported &&
                    nativeMetadata.LayoutValid ? 1u : 0u);
            const bool mapReadable =
                IsReadableRange(property, sizeof(FMapProperty));
            const auto* mapProperty = mapReadable
                ? static_cast<const FMapProperty*>(property)
                : nullptr;
            const FProperty* key = mapProperty != nullptr
                ? mapProperty->KeyProperty : nullptr;
            const FProperty* value = mapProperty != nullptr
                ? mapProperty->ValueProperty : nullptr;
            const bool keyReadable = key != nullptr &&
                IsReadableRange(key, sizeof(FProperty));
            const bool valueReadable = value != nullptr &&
                IsReadableRange(value, sizeof(FProperty));
            const unsigned long long keyFlags =
                keyReadable && key->ClassPrivate != nullptr &&
                IsReadableRange(key->ClassPrivate, sizeof(FFieldClass))
                    ? static_cast<unsigned long long>(
                        key->ClassPrivate->CastFlags)
                    : 0ULL;
            const unsigned long long valueFlags =
                valueReadable && value->ClassPrivate != nullptr &&
                IsReadableRange(value->ClassPrivate, sizeof(FFieldClass))
                    ? static_cast<unsigned long long>(
                        value->ClassPrivate->CastFlags)
                    : 0ULL;
            LOG_WARN(
                "PersistentIdFix: reflection map key/value metadata: "
                "depth=%u storageSize=%d mapReadable=%u "
                "descriptorSize=%llu keyReadable=%u keySize=%d "
                "keyOffset=%d keyFlags=0x%llX "
                "valueReadable=%u valueSize=%d valueOffset=%d "
                "valueFlags=0x%llX",
                depth,
                property->ElementSize,
                mapReadable ? 1u : 0u,
                static_cast<unsigned long long>(sizeof(FMapProperty)),
                keyReadable ? 1u : 0u,
                keyReadable ? key->ElementSize : -1,
                keyReadable ? key->Offset : -1,
                keyFlags,
                valueReadable ? 1u : 0u,
                valueReadable ? value->ElementSize : -1,
                valueReadable ? value->Offset : -1,
                valueFlags);
            // Metadata only. Do not inspect map storage or entry contents.
            const bool keyStructProperty = keyReadable &&
                HasCastFlag(key, EClassCastFlags::StructProperty) &&
                IsReadableRange(key, sizeof(FStructProperty));
            const bool valueStructProperty = valueReadable &&
                HasCastFlag(value, EClassCastFlags::StructProperty) &&
                IsReadableRange(value, sizeof(FStructProperty));
            const UStruct* keyDescriptor = keyStructProperty
                ? static_cast<const FStructProperty*>(key)->Struct : nullptr;
            const UStruct* valueDescriptor = valueStructProperty
                ? static_cast<const FStructProperty*>(value)->Struct : nullptr;
            const bool keyDescriptorReadable = keyDescriptor != nullptr &&
                IsReadableRange(keyDescriptor, sizeof(UStruct));
            const bool valueDescriptorReadable = valueDescriptor != nullptr &&
                IsReadableRange(valueDescriptor, sizeof(UStruct));
            LOG_WARN(
                "PersistentIdFix: reflection map struct descriptors: "
                "depth=%u keyIsStruct=%u keyDescriptorReadable=%u "
                "keyStructSize=%d keyExactPid=%u "
                "valueIsStruct=%u valueDescriptorReadable=%u "
                "valueStructSize=%d valueExactPid=%u",
                depth,
                keyStructProperty ? 1u : 0u,
                keyDescriptorReadable ? 1u : 0u,
                keyDescriptorReadable ? keyDescriptor->Size : -1,
                keyDescriptorReadable &&
                    keyDescriptor == persistentIdDescriptor ? 1u : 0u,
                valueStructProperty ? 1u : 0u,
                valueDescriptorReadable ? 1u : 0u,
                valueDescriptorReadable ? valueDescriptor->Size : -1,
                valueDescriptorReadable &&
                    valueDescriptor == persistentIdDescriptor ? 1u : 0u);
            LOG_WARN(
                "PersistentIdFix: reflection step28 map identity: "
                "property=%p keyReady=%u keyExactItemId=%u "
                "valueReady=%u valueExactJson=%u",
                static_cast<const void*>(property),
                g_step28ItemId != nullptr ? 1u : 0u,
                keyDescriptorReadable && g_step28ItemId != nullptr &&
                    keyDescriptor == g_step28ItemId ? 1u : 0u,
                g_step28Json != nullptr ? 1u : 0u,
                valueDescriptorReadable && g_step28Json != nullptr &&
                    valueDescriptor == g_step28Json ? 1u : 0u);
            // Metadata-only bounded child-property inspection. No map data reads.
            const UStruct* mapDescriptors[2] = {
                keyDescriptorReadable ? keyDescriptor : nullptr,
                valueDescriptorReadable ? valueDescriptor : nullptr
            };
            for (unsigned int side = 0; side < 2u; ++side)
            {
                const UStruct* descriptor = mapDescriptors[side];
                if (descriptor == nullptr || descriptor->Size <= 0)
                    continue;

                const FField* child = descriptor->ChildProperties;
                unsigned int inspected = 0;
                while (child != nullptr && inspected < 8u)
                {
                    const bool fieldReadable =
                        IsReadableRange(child, sizeof(FField));
                    const bool propertyType = fieldReadable &&
                        HasCastFlag(child, EClassCastFlags::Property);
                    const auto* nested = propertyType
                        ? reinterpret_cast<const FProperty*>(child)
                        : nullptr;
                    const bool propertyReadable = nested != nullptr &&
                        IsReadableRange(nested, sizeof(FProperty));
                    unsigned long long castFlags = 0ULL;
                    if (fieldReadable && child->ClassPrivate != nullptr &&
                        IsReadableRange(child->ClassPrivate, sizeof(FFieldClass)))
                    {
                        castFlags = static_cast<unsigned long long>(
                            child->ClassPrivate->CastFlags);
                    }
                    LOG_WARN(
                        "PersistentIdFix: reflection map child property: "
                        "side=%u index=%u fieldReadable=%u propertyReadable=%u "
                        "offset=%d size=%d arrayDim=%d castFlags=0x%llX",
                        side, inspected, fieldReadable ? 1u : 0u,
                        propertyReadable ? 1u : 0u,
                        propertyReadable ? nested->Offset : -1,
                        propertyReadable ? nested->ElementSize : -1,
                        propertyReadable ? nested->ArrayDim : -1,
                        castFlags);
                    // Descriptor-only nested struct inspection; never read map entries.
                    if (propertyReadable &&
                        HasCastFlag(nested, EClassCastFlags::StructProperty))
                    {
                        const bool structPropertyReadable =
                            IsReadableRange(nested, sizeof(FStructProperty));
                        const auto* structProp = structPropertyReadable
                            ? static_cast<const FStructProperty*>(nested) : nullptr;
                        const UStruct* nestedDescriptor = structProp != nullptr
                            ? structProp->Struct : nullptr;
                        const bool descriptorReadable = nestedDescriptor != nullptr &&
                            IsReadableRange(nestedDescriptor, sizeof(UStruct));
                        LOG_WARN(
                            "PersistentIdFix: reflection map nested child struct: "
                            "side=%u index=%u structPropertyReadable=%u "
                            "descriptorReadable=%u descriptorSize=%d "
                            "exactPid=%u hasChildren=%u",
                            side, inspected,
                            structPropertyReadable ? 1u : 0u,
                            descriptorReadable ? 1u : 0u,
                            descriptorReadable ? nestedDescriptor->Size : -1,
                            descriptorReadable &&
                                nestedDescriptor == persistentIdDescriptor ? 1u : 0u,
                            descriptorReadable &&
                                nestedDescriptor->ChildProperties != nullptr ? 1u : 0u);
                    }
                    // Patch 23: descriptor-only inspection of the nested
                    // map-key structure (side 0), limited to eight children.
                    // Do not read map storage or dereference object values.
                    if (side == 0u && propertyReadable &&
                        HasCastFlag(nested, EClassCastFlags::StructProperty) &&
                        IsReadableRange(nested, sizeof(FStructProperty)))
                    {
                        const UStruct* innerDescriptor =
                            static_cast<const FStructProperty*>(nested)->Struct;
                        if (innerDescriptor != nullptr &&
                            IsReadableRange(innerDescriptor, sizeof(UStruct)) &&
                            innerDescriptor->Size > 0)
                        {
                            LOG_WARN(
                                "PersistentIdFix: reflection step28 nested identity: "
                                "keyReady=%u keyExactItemId=%u guidReady=%u "
                                "nestedOffset=%d nestedSize=%d nestedArrayDim=%d "
                                "nestedExactGuid=%u",
                                g_step28ItemId != nullptr ? 1u : 0u,
                                keyDescriptorReadable && g_step28ItemId != nullptr &&
                                    keyDescriptor == g_step28ItemId ? 1u : 0u,
                                g_step28Guid != nullptr ? 1u : 0u,
                                nested->Offset, nested->ElementSize, nested->ArrayDim,
                                g_step28Guid != nullptr &&
                                    innerDescriptor == g_step28Guid ? 1u : 0u);
                            const FField* inner = innerDescriptor->ChildProperties;
                            unsigned int innerIndex = 0u;
                            while (inner != nullptr && innerIndex < 8u)
                            {
                                if (!IsReadableRange(inner, sizeof(FField)))
                                {
                                    LOG_WARN(
                                        "PersistentIdFix: reflection map nested key field unreadable: "
                                        "index=%u", innerIndex);
                                    break;
                                }
                                const bool innerIsProperty =
                                    HasCastFlag(inner, EClassCastFlags::Property);
                                const FProperty* innerProperty = innerIsProperty
                                    ? reinterpret_cast<const FProperty*>(inner)
                                    : nullptr;
                                const bool innerReadable =
                                    innerProperty != nullptr &&
                                    IsReadableRange(innerProperty, sizeof(FProperty));
                                const bool classReadable =
                                    inner->ClassPrivate != nullptr &&
                                    IsReadableRange(inner->ClassPrivate, sizeof(FFieldClass));
                                const unsigned long long innerFlags = classReadable
                                    ? static_cast<unsigned long long>(
                                        inner->ClassPrivate->CastFlags)
                                    : 0ULL;
                                LOG_WARN(
                                    "PersistentIdFix: reflection map nested key property: "
                                    "index=%u readable=%u offset=%d size=%d "
                                    "arrayDim=%d flags=0x%llX integral=%u "
                                    "struct=%u array=%u exactPidStruct=%u",
                                    innerIndex,
                                    innerReadable ? 1u : 0u,
                                    innerReadable ? innerProperty->Offset : -1,
                                    innerReadable ? innerProperty->ElementSize : -1,
                                    innerReadable ? innerProperty->ArrayDim : -1,
                                    innerFlags,
                                    innerReadable &&
                                        IsSupportedIntegralProperty(innerProperty) ? 1u : 0u,
                                    innerReadable &&
                                        HasCastFlag(innerProperty, EClassCastFlags::StructProperty) ? 1u : 0u,
                                    innerReadable &&
                                        HasCastFlag(innerProperty, EClassCastFlags::ArrayProperty) ? 1u : 0u,
                                    innerReadable &&
                                        HasCastFlag(innerProperty, EClassCastFlags::StructProperty) &&
                                        IsReadableRange(innerProperty, sizeof(FStructProperty)) &&
                                        static_cast<const FStructProperty*>(innerProperty)->Struct ==
                                            persistentIdDescriptor ? 1u : 0u);

                                if (!innerReadable)
                                    break;
                                inner = inner->Next;
                                ++innerIndex;
                            }
                            LOG_WARN(
                                "PersistentIdFix: reflection map nested key scan: "
                                "inspected=%u remaining=%u descriptorSize=%d",
                                innerIndex, inner != nullptr ? 1u : 0u,
                                innerDescriptor->Size);
                        }
                    }
                    if (!fieldReadable || !propertyReadable)
                        break;
                    child = child->Next;
                    ++inspected;
                }
                LOG_WARN(
                    "PersistentIdFix: reflection map child scan: "
                    "side=%u inspected=%u truncated=%u",
                    side, inspected, child != nullptr ? 1u : 0u);
            }
        }
#endif

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        // Classification-only breadcrumb. Never read or reinterpret the
        // unknown payload, and retain the existing fail-closed behavior.
        LOG_WARN(
            "PersistentIdFix: reflection unsupported leaf classification: "
            "depth=%u elementSize=%d offset=%d "
            "name=%u object=%u str=%u text=%u "
            "float=%u double=%u",
            depth,
            property->ElementSize,
            property->Offset,
            HasCastFlag(property, EClassCastFlags::NameProperty) ? 1u : 0u,
            HasCastFlag(property, EClassCastFlags::ObjectProperty) ? 1u : 0u,
            HasCastFlag(property, EClassCastFlags::StrProperty) ? 1u : 0u,
            HasCastFlag(property, EClassCastFlags::TextProperty) ? 1u : 0u,
            HasCastFlag(property, EClassCastFlags::FloatProperty) ? 1u : 0u,
            HasCastFlag(property, EClassCastFlags::DoubleProperty) ? 1u : 0u);
#endif

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        // Report reflected type bits without interpreting unknown value storage.
        // The class pointer is separately validated before any metadata access.
        const auto* leafClass = property->ClassPrivate;
        if (leafClass != nullptr &&
            IsReadableRange(leafClass, sizeof(FFieldClass)))
        {
            LOG_WARN(
                "PersistentIdFix: reflection unsupported leaf cast flags: "
                "depth=%u offset=%d elementSize=%d castFlags=0x%llX",
                depth,
                property->Offset,
                property->ElementSize,
                static_cast<unsigned long long>(leafClass->CastFlags));
        }
        else
        {
            LOG_WARN(
                "PersistentIdFix: reflection unsupported leaf cast flags: "
                "depth=%u classUnreadable=1",
                depth);
        }
#endif
        summary.complete = false;
        ++summary.unsupported;
        return false;
    }

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
    const FProperty* FindFirstIntegralArrayProperty(
        const UStruct* descriptor)
    {
        const UStruct* current = descriptor;
        std::uint32_t inheritanceGuard = 0;

        while (current != nullptr)
        {
            if (++inheritanceGuard > kShadowMaxStructDepth ||
                !IsReadableRange(current, sizeof(UStruct)))
            {
                return nullptr;
            }

            const FField* field = current->ChildProperties;
            std::uint64_t chainGuard = 0;

            while (field != nullptr)
            {
                if (++chainGuard > kShadowMaxProperties ||
                    !IsReadableRange(field, sizeof(FField)))
                {
                    return nullptr;
                }

                if (HasCastFlag(field, EClassCastFlags::ArrayProperty))
                {
                    const auto* property =
                        reinterpret_cast<const FProperty*>(field);

                    if (!IsReadableRange(
                            property,
                            sizeof(FArrayProperty)))
                    {
                        return nullptr;
                    }

                    const auto* arrayProperty =
                        static_cast<const FArrayProperty*>(property);

                    if (arrayProperty->InnerProperty != nullptr &&
                        IsSupportedIntegralProperty(
                            arrayProperty->InnerProperty))
                    {
                        return property;
                    }
                }

                field = field->Next;
            }

            current = current->SuperStruct;
        }

        return nullptr;
    }

    const FProperty* FindIntegralArrayPropertyByOffset(
        const UStruct* descriptor,
        std::int32_t offset)
    {
        const UStruct* current = descriptor;
        std::uint32_t inheritanceGuard = 0;

        while (current != nullptr)
        {
            if (++inheritanceGuard > kShadowMaxStructDepth ||
                !IsReadableRange(current, sizeof(UStruct)))
            {
                return nullptr;
            }

            const FField* field = current->ChildProperties;
            std::uint64_t chainGuard = 0;

            while (field != nullptr)
            {
                if (++chainGuard > kShadowMaxProperties ||
                    !IsReadableRange(field, sizeof(FField)))
                {
                    return nullptr;
                }

                if (HasCastFlag(field, EClassCastFlags::ArrayProperty))
                {
                    const auto* property =
                        reinterpret_cast<const FProperty*>(field);

                    if (!IsReadableRange(property, sizeof(FArrayProperty)))
                        return nullptr;

                    if (property->Offset == offset)
                    {
                        const auto* arrayProperty =
                            static_cast<const FArrayProperty*>(property);

                        if (arrayProperty->InnerProperty != nullptr &&
                            IsReadableRange(
                                arrayProperty->InnerProperty,
                                sizeof(FProperty)) &&
                            IsSupportedIntegralProperty(
                                arrayProperty->InnerProperty))
                        {
                            return property;
                        }
                    }
                }

                field = field->Next;
            }

            current = current->SuperStruct;
        }

        return nullptr;
    }

    // Diagnostic-only: find a nested reflected struct field by a known
    // typed member offset, without trusting a property name or raw bytes.
    const FStructProperty* FindStructPropertyByOffset(
        const UStruct* descriptor,
        std::int32_t offset,
        const UScriptStruct* expectedStruct)
    {
        if (descriptor == nullptr || expectedStruct == nullptr || offset < 0)
            return nullptr;

        const UStruct* current = descriptor;
        std::uint32_t inheritanceGuard = 0;
        while (current != nullptr)
        {
            if (++inheritanceGuard > kShadowMaxStructDepth ||
                !IsReadableRange(current, sizeof(UStruct)))
                return nullptr;

            const FField* field = current->ChildProperties;
            std::uint64_t chainGuard = 0;
            while (field != nullptr)
            {
                if (++chainGuard > kShadowMaxProperties ||
                    !IsReadableRange(field, sizeof(FField)))
                    return nullptr;

                if (HasCastFlag(field, EClassCastFlags::StructProperty))
                {
                    const auto* property =
                        reinterpret_cast<const FStructProperty*>(field);
                    if (!IsReadableRange(property, sizeof(FStructProperty)))
                        return nullptr;

                    if (property->Offset == offset &&
                        property->ArrayDim == 1 &&
                        property->Struct == expectedStruct &&
                        expectedStruct->Size > 0 &&
                        property->ElementSize >= expectedStruct->Size)
                        return property;
                }
                field = field->Next;
            }
            current = current->SuperStruct;
        }
        return nullptr;
    }

    void RunForcedReflectionShapeTests()
    {
        ShadowSchemaSummary persistentIdSummary{};
        const bool persistentIdSupported =
            g_reflectionTestPersistentIdDescriptor != nullptr &&
            AnalyzeShadowStructShape(
                g_reflectionTestPersistentIdDescriptor,
                0,
                persistentIdSummary);

        LOG_INFO(
            "PersistentIdFix: forced reflection test: target=CrMassPersistentEntityID "
            "supportedShape=%u properties=%llu arrays=%llu structs=%llu "
            "integralLeaves=%llu enumLeaves=%llu unsupported=%llu maxDepth=%llu",
            persistentIdSupported ? 1u : 0u,
            static_cast<unsigned long long>(persistentIdSummary.properties),
            static_cast<unsigned long long>(persistentIdSummary.arrays),
            static_cast<unsigned long long>(persistentIdSummary.structs),
            static_cast<unsigned long long>(persistentIdSummary.integralLeaves),
            static_cast<unsigned long long>(persistentIdSummary.enumLeaves),
            static_cast<unsigned long long>(persistentIdSummary.unsupported),
            static_cast<unsigned long long>(persistentIdSummary.maxDepth));

        ShadowSchemaSummary arraySummary{};
        const FProperty* arrayProperty =
            FindFirstIntegralArrayProperty(
                g_reflectionTestMapMenuDescriptor);

        bool arraySupported = false;
        if (arrayProperty != nullptr)
        {
            arraySummary.properties = 1;
            arraySupported =
                AnalyzeShadowPropertyShape(
                    arrayProperty,
                    0,
                    arraySummary);
        }
        else
        {
            arraySummary.supported = false;
            ++arraySummary.unsupported;
        }

        LOG_INFO(
            "PersistentIdFix: forced reflection test: "
            "target=CrPlayersMapMenuState.firstIntegralArray "
            "propertyFound=%u supportedShape=%u properties=%llu arrays=%llu "
            "structs=%llu integralLeaves=%llu enumLeaves=%llu "
            "unsupported=%llu maxDepth=%llu",
            arrayProperty != nullptr ? 1u : 0u,
            arraySupported ? 1u : 0u,
            static_cast<unsigned long long>(arraySummary.properties),
            static_cast<unsigned long long>(arraySummary.arrays),
            static_cast<unsigned long long>(arraySummary.structs),
            static_cast<unsigned long long>(arraySummary.integralLeaves),
            static_cast<unsigned long long>(arraySummary.enumLeaves),
            static_cast<unsigned long long>(arraySummary.unsupported),
            static_cast<unsigned long long>(arraySummary.maxDepth));
    }

    void RunForcedReflectionValueTests(
        const FCrCharacterPlayerBaseSaveDataPerPlayer& player)
    {
        // Diagnostic only. Traverse the entire reflected parent, rather
        // than a single property selected by its typed field offset.
        // This deliberately does not classify encountered integers as PIDs.
        ShadowValueSummary parentSummary{};
        const bool parentEligible =
            g_reflectionTestPlayerDescriptor != nullptr &&
            IsReadableRange(
                g_reflectionTestPlayerDescriptor,
                sizeof(UScriptStruct)) &&
            g_reflectionTestPlayerDescriptor->Size > 0 &&
            static_cast<std::size_t>(
                g_reflectionTestPlayerDescriptor->Size) <= sizeof(player);

        const bool parentComplete =
            parentEligible &&
            TraverseShadowStructValue(
                g_reflectionTestPlayerDescriptor,
                &player,
                0,
                g_reflectionTestPersistentIdDescriptor,
                parentSummary);

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=PlayerSave.wholeParent eligible=%u complete=%u "
            "exactPid=%llu integral=%llu candidates=%llu "
            "arrays=%llu elements=%llu structs=%llu "
            "unsupported=%llu maxDepth=%llu "
            "floorExpected=%u",
            parentEligible ? 1u : 0u,
            parentComplete ? 1u : 0u,
            static_cast<unsigned long long>(
                parentSummary.exactPersistentIds),
            static_cast<unsigned long long>(
                parentSummary.integralValues),
            static_cast<unsigned long long>(
                parentSummary.candidateValues),
            static_cast<unsigned long long>(parentSummary.arrays),
            static_cast<unsigned long long>(parentSummary.arrayElements),
            static_cast<unsigned long long>(parentSummary.structs),
            static_cast<unsigned long long>(parentSummary.unsupported),
            static_cast<unsigned long long>(parentSummary.maxDepth),
            player.FloorPersistentEntityID.ID);

        ShadowValueSummary pidSummary{};

        const bool pidComplete =
            g_reflectionTestPersistentIdDescriptor != nullptr &&
            TraverseShadowStructValue(
                g_reflectionTestPersistentIdDescriptor,
                &player.FloorPersistentEntityID,
                0,
                g_reflectionTestPersistentIdDescriptor,
                pidSummary);

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=FloorPersistentEntityID complete=%u "
            "exactPid=%llu integral=%llu candidates=%llu "
            "first=%u last=%u expected=%u zero=%llu sentinel=%llu "
            "unsupported=%llu maxDepth=%llu",
            pidComplete ? 1u : 0u,
            static_cast<unsigned long long>(pidSummary.exactPersistentIds),
            static_cast<unsigned long long>(pidSummary.integralValues),
            static_cast<unsigned long long>(pidSummary.candidateValues),
            pidSummary.firstCandidate,
            pidSummary.lastCandidate,
            player.FloorPersistentEntityID.ID,
            static_cast<unsigned long long>(pidSummary.zeros),
            static_cast<unsigned long long>(pidSummary.invalidSentinels),
            static_cast<unsigned long long>(pidSummary.unsupported),
            static_cast<unsigned long long>(pidSummary.maxDepth));

        // Traverse a real nested struct *property* rather than invoking
        // the PID wrapper's struct walker directly. The typed member is
        // the independent value oracle; no PID is emitted to certification.
        const auto typedPlayerBase =
            reinterpret_cast<std::uintptr_t>(&player);
        const auto typedFloorAddress =
            reinterpret_cast<std::uintptr_t>(&player.FloorPersistentEntityID);
        const bool floorOffsetValid =
            typedFloorAddress >= typedPlayerBase &&
            (typedFloorAddress - typedPlayerBase) <=
                static_cast<std::uintptr_t>(
                    (std::numeric_limits<std::int32_t>::max)());

        const FStructProperty* nestedFloorProperty =
            floorOffsetValid
                ? FindStructPropertyByOffset(
                    g_reflectionTestPlayerDescriptor,
                    static_cast<std::int32_t>(
                        typedFloorAddress - typedPlayerBase),
                    g_reflectionTestPersistentIdDescriptor)
                : nullptr;
        ShadowValueSummary nestedSummary{};
        bool nestedComplete = false;
        if (nestedFloorProperty != nullptr)
        {
            nestedComplete = TraverseShadowPropertyValue(
                nestedFloorProperty,
                &player.FloorPersistentEntityID,
                0,
                g_reflectionTestPersistentIdDescriptor,
                nestedSummary);
        }
        else
        {
            nestedSummary.complete = false;
            ++nestedSummary.unsupported;
        }

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=FloorPersistentEntityID.nestedProperty "
            "propertyFound=%u complete=%u exactPid=%llu "
            "candidates=%llu first=%u last=%u expected=%u "
            "unsupported=%llu maxDepth=%llu",
            nestedFloorProperty != nullptr ? 1u : 0u,
            nestedComplete ? 1u : 0u,
            static_cast<unsigned long long>(nestedSummary.exactPersistentIds),
            static_cast<unsigned long long>(nestedSummary.candidateValues),
            nestedSummary.firstCandidate,
            nestedSummary.lastCandidate,
            player.FloorPersistentEntityID.ID,
            static_cast<unsigned long long>(nestedSummary.unsupported),
            static_cast<unsigned long long>(nestedSummary.maxDepth));
        const auto playerBase =
            reinterpret_cast<std::uintptr_t>(&player);
        const auto arrayAddress =
            reinterpret_cast<std::uintptr_t>(&player.DiscoveredBuildings);

        bool offsetValid = arrayAddress >= playerBase;
        const std::uint64_t offsetWide =
            offsetValid
                ? static_cast<std::uint64_t>(arrayAddress - playerBase)
                : 0;

        if (offsetWide >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::int32_t>::max)()))
        {
            offsetValid = false;
        }

        const FProperty* discoveredProperty =
            offsetValid
                ? FindIntegralArrayPropertyByOffset(
                    g_reflectionTestPlayerDescriptor,
                    static_cast<std::int32_t>(offsetWide))
                : nullptr;

        ShadowValueSummary arraySummary{};
        bool arrayComplete = false;

        if (discoveredProperty != nullptr)
        {
            const auto* arrayStorage =
                reinterpret_cast<const std::uint8_t*>(&player) +
                discoveredProperty->Offset;

            arrayComplete =
                TraverseShadowPropertyValue(
                    discoveredProperty,
                    arrayStorage,
                    0,
                    g_reflectionTestPersistentIdDescriptor,
                    arraySummary);
        }
        else
        {
            arraySummary.complete = false;
            ++arraySummary.unsupported;
        }

        LOG_INFO(
            "PersistentIdFix: forced reflection value test: "
            "target=DiscoveredBuildings propertyFound=%u complete=%u "
            "typedNum=%d arrays=%llu elements=%llu integral=%llu "
            "candidates=%llu first=%u last=%u zero=%llu sentinel=%llu "
            "unsupported=%llu maxDepth=%llu",
            discoveredProperty != nullptr ? 1u : 0u,
            arrayComplete ? 1u : 0u,
            player.DiscoveredBuildings.Num(),
            static_cast<unsigned long long>(arraySummary.arrays),
            static_cast<unsigned long long>(arraySummary.arrayElements),
            static_cast<unsigned long long>(arraySummary.integralValues),
            static_cast<unsigned long long>(arraySummary.candidateValues),
            arraySummary.firstCandidate,
            arraySummary.lastCandidate,
            static_cast<unsigned long long>(arraySummary.zeros),
            static_cast<unsigned long long>(arraySummary.invalidSentinels),
            static_cast<unsigned long long>(arraySummary.unsupported),
            static_cast<unsigned long long>(arraySummary.maxDepth));
    }
#endif

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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        g_reflectionTestPersistentIdDescriptor = nullptr;
        g_reflectionTestMapMenuDescriptor = nullptr;
        g_reflectionTestPlayerDescriptor = nullptr;
        g_step28Store = nullptr;
        g_step28ItemId = nullptr;
        g_step28Guid = nullptr;
        g_step28Json = nullptr;
#endif

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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        const auto resolveTestDescriptor =
            [&](const wchar_t* path) -> const UScriptStruct*
            {
                UObject* object =
                    findSafe(scriptStructClass, nullptr, path, true);

                return object != nullptr
                    ? reinterpret_cast<const UScriptStruct*>(object)
                    : nullptr;
            };

        g_reflectionTestPersistentIdDescriptor =
            resolveTestDescriptor(
                L"/Script/ChimeraMassCommon.CrMassPersistentEntityID");

        g_reflectionTestMapMenuDescriptor =
            resolveTestDescriptor(
                L"/Script/Chimera.CrPlayersMapMenuState");

        g_reflectionTestPlayerDescriptor =
            resolveTestDescriptor(
                L"/Script/Chimera.CrCharacterPlayerBaseSaveDataPerPlayer");

        // Diagnostic-only lookups; missing or invalid descriptors remain unavailable.
        const auto resolveStep28 = [&](const wchar_t* path, int expectedSize)
            -> const UScriptStruct*
        {
            const UScriptStruct* result = resolveTestDescriptor(path);
            if (result == nullptr ||
                !IsReadableRange(result, sizeof(UScriptStruct)) ||
                result->Size != expectedSize)
                return nullptr;
            return result;
        };
        g_step28Store = resolveStep28(
            L"/Script/AuItems.AuItemsStoreComponentState", 0xC0);
        g_step28ItemId = resolveStep28(
            L"/Script/AuItems.AuItemId", 0x10);
        g_step28Guid = resolveStep28(
            L"/Script/CoreUObject.Guid", 0x10);
        g_step28Json = resolveStep28(
            L"/Script/JsonUtilities.JsonObjectWrapper", 0x20);
        LOG_WARN(
            "PersistentIdFix: reflection step28 descriptor identity: "
            "storeReady=%u itemIdReady=%u guidReady=%u jsonReady=%u",
            g_step28Store != nullptr ? 1u : 0u,
            g_step28ItemId != nullptr ? 1u : 0u,
            g_step28Guid != nullptr ? 1u : 0u,
            g_step28Json != nullptr ? 1u : 0u);

        LOG_INFO(
            "PersistentIdFix: reflection test mode descriptor lookup: "
            "persistentId=%p mapMenu=%p player=%p",
            static_cast<const void*>(
                g_reflectionTestPersistentIdDescriptor),
            static_cast<const void*>(
                g_reflectionTestMapMenuDescriptor),
            static_cast<const void*>(
                g_reflectionTestPlayerDescriptor));
#endif

        g_registryReady = true;

        LOG_INFO(
            "PersistentIdFix: independent source descriptor registry ready: "
            "profiles=6 reflectionTestMode=%u",
            PERSISTENTIDFIX_REFLECTION_TEST_MODE ? 1u : 0u);

        return true;
    }

    void ResetDescriptorRegistry()
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        g_registryReady = false;
        g_descriptors.fill(nullptr);

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        g_reflectionTestPersistentIdDescriptor = nullptr;
        g_reflectionTestMapMenuDescriptor = nullptr;
        g_reflectionTestPlayerDescriptor = nullptr;
        g_step28Store = nullptr;
        g_step28ItemId = nullptr;
        g_step28Guid = nullptr;
        g_step28Json = nullptr;
#endif
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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        RunForcedReflectionShapeTests();
#endif

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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
                        RunForcedReflectionValueTests(player);
#endif

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

                        const bool shadowSupported =
                            AnalyzeShadowPayload(
                                payload,
                                shadow);

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
                            "schema=%p size=%d supportedShape=%u "
                            "properties=%llu arrays=%llu structs=%llu "
                            "integralLeaves=%llu enumLeaves=%llu "
                            "unsupported=%llu maxDepth=%llu",
                            static_cast<const void*>(
                                payload.ScriptStruct),
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

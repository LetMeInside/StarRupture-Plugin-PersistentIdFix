#include "MassFragmentClassifier.h"

#include "plugin_helpers.h"

#include "SDK/Chimera_structs.hpp"
#include "SDK/AuActorPlacement_structs.hpp"
#include "SDK/CoreUObject_structs.hpp"
#include "SDK/CoreUObject_classes.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <type_traits>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace
{
    using namespace SDK;

    enum class FragmentClass : std::uint8_t
    {
        PidFree,
        PidBearing
    };

    struct RegistryEntry
    {
        const char* FullName;
        const char* ImmediateBaseFullName;
        std::int32_t ExpectedSize;
        std::int16_t ExpectedMinAlignment;
        bool EmptyMarkerSizeRule;
        FragmentClass Class;
    };

    // Hotfix 0.3.5 audited ABI/layout invariants. These assertions deliberately
    // make an SDK drift a build failure instead of silently accepting it.
    static_assert(sizeof(FInstancedStruct) == 0x10);
    static_assert(offsetof(FInstancedStruct, ScriptStruct) == 0x00);
    static_assert(offsetof(FInstancedStruct, StructMemory) == 0x08);

    static_assert(sizeof(FCrEntitySaveData) == 0x90);
    static_assert(offsetof(FCrEntitySaveData, Tags) == 0x70);
    static_assert(offsetof(FCrEntitySaveData, FragmentValues) == 0x80);

    static_assert(sizeof(FAuAPMassFragment) == 80);
    static_assert(sizeof(FAuSplineConnectionFragment) == 224);
    static_assert(sizeof(FCrAlienObeliskFragment) == 56);
    static_assert(sizeof(FCrBuildingInfectionAlarmFragment) == 8);
    static_assert(sizeof(FCrBuildingInfectionFragment) == 48);
    static_assert(sizeof(FCrBuildingStateFragment) == 4);
    static_assert(sizeof(FCrCraftingFragment) == 128);
    static_assert(sizeof(FCrElectricityFragment) == 20);
    static_assert(sizeof(FCrHeaterCoolerFragment) == 40);
    static_assert(sizeof(FCrInventoryFragment) == 592);
    static_assert(sizeof(FCrLogisticsAgentFragment) == 216);
    static_assert(sizeof(FCrLogisticsIntersectionFragment) == 48);
    static_assert(sizeof(FCrLogisticsLineFragment) == 56);
    static_assert(sizeof(FCrLogisticsRequestContainerFragment) == 40);
    static_assert(sizeof(FCrLogisticsRequestOptionsFragment) == 1);
    static_assert(sizeof(FCrLogisticsRoundaboutFragment) == 32);
    static_assert(sizeof(FCrLogisticsSocketsFragment) == 16);
    // Native/PDB/reflected Hotfix 0.3.5 size is 0x4 in both modes.
    // Do not sizeof-assert this generated wrapper: the Client SDK injects a
    // synthetic byte into its empty CustomOnRep base and can inflate the
    // compiler layout of this derived type without changing native storage.
    static_assert(sizeof(FCrLogisticsVerticalConnectorFragment) == 48);
    static_assert(sizeof(FCrMassBuildingBaseCoreFragment) == 104);
    static_assert(sizeof(FCrMassBuildingStabilityData) == 12);
    static_assert(sizeof(FCrMassDynamicPillarFragment) == 8);
    static_assert(sizeof(FCrMassFoundableFragment) == 16);
    static_assert(sizeof(FCrMassTemperatureFragment) == 128);
    static_assert(sizeof(FCrPackageReceiverFragment) == 56);
    static_assert(sizeof(FCrStandaloneInfectionFragment) == 40);

    // The empty marker base has a generated C++ size annotation discrepancy
    // between Client and Server, so it is intentionally identified by exact
    // UScriptStruct identity but not sizeof-asserted here.

    std::atomic<std::uint64_t> g_massPayloads{0};
    std::atomic<std::uint64_t> g_classifiedPayloads{0};
    std::atomic<std::uint64_t> g_pidBearingPayloads{0};
    std::atomic<std::uint64_t> g_pidFreePayloads{0};
    std::atomic<std::uint64_t> g_emptyPayloads{0};
    std::atomic<std::uint64_t> g_malformedPayloads{0};
    std::atomic<std::uint64_t> g_unsupportedPayloads{0};
    std::atomic<std::uint64_t> g_schemaMismatches{0};
    std::atomic<std::uint64_t> g_massDescriptorMismatches{0};
    std::atomic<std::uint64_t> g_entitySlotsVisited{0};
    std::atomic<std::uint64_t> g_entityCountMismatches{0};
    std::atomic<std::uint64_t> g_tagPayloads{0};

    std::atomic<std::uint64_t> g_semanticAttempts{0};
    std::atomic<std::uint64_t> g_semanticSuccesses{0};
    std::atomic<std::uint64_t> g_semanticFailures{0};
    std::atomic<std::uint64_t> g_semanticNumericValues{0};
    std::atomic<std::uint64_t> g_semanticZeroValues{0};
    std::atomic<std::uint64_t> g_semanticInvalidSentinels{0};
    std::atomic<std::uint64_t> g_semanticContainerFailures{0};

    std::mutex g_semanticValuesMutex;
    std::vector<std::uint32_t> g_semanticValues;

    const std::array<RegistryEntry, 27>& Registry()
    {
        static const std::array<RegistryEntry, 27> registry{{
            { "ScriptStruct AuActorPlacement.AuAPMassFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 80, 8, false, FragmentClass::PidFree },
            { "ScriptStruct AuActorPlacement.AuSplineConnectionFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 224, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrAlienObeliskFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 56, 8, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrBuildingInfectionAlarmFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 8, 4, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrBuildingInfectionFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 48, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrBuildingStateFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 4, 2, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrCraftingFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 128, 8, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrElectricityFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 20, 4, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrHeaterCoolerFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 40, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrInventoryFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 592, 8, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrLogisticsAgentFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 216, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrLogisticsIntersectionFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 48, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrLogisticsLineFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 56, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrLogisticsRequestContainerFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 40, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrLogisticsRequestOptionsFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 1, 1, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrLogisticsRoundaboutFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 32, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrLogisticsSocketsFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 16, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrLogisticsTierFragment", "ScriptStruct Chimera.CrMassSavableFragmentWithCustomOnRepCallActor", 4, 4, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrLogisticsVerticalConnectorFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 48, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrMassBuildingBaseCoreFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 104, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrMassBuildingStabilityData", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 12, 4, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrMassDynamicPillarFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 8, 4, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrMassFoundableFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 16, 4, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrMassSavableFragmentWithCustomOnRepCallActor", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 0, 1, true, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrMassTemperatureFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 128, 8, false, FragmentClass::PidBearing },
            { "ScriptStruct Chimera.CrPackageReceiverFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 56, 8, false, FragmentClass::PidFree },
            { "ScriptStruct Chimera.CrStandaloneInfectionFragment", "ScriptStruct ChimeraMassCommon.CrMassSavableFragment", 40, 8, false, FragmentClass::PidBearing }
        }};
        return registry;
    }

    struct ResolvedRegistryEntry
    {
        const RegistryEntry* Profile = nullptr;
        const UScriptStruct* Descriptor = nullptr;
        const UStruct* ExpectedBase = nullptr;
    };

    using StaticFindObjectSafeByNameFn =
        UObject* (__fastcall*)(UClass*, UObject*, const wchar_t*, bool);

    static std::mutex g_registryMutex;
    static std::vector<ResolvedRegistryEntry> g_resolvedRegistry;
    static std::unordered_map<const UScriptStruct*, std::size_t> g_registryByPointer;
    static const UScriptStruct* g_massSaveDataDescriptor = nullptr;
    static bool g_descriptorRegistryReady = false;

    std::wstring NativePathFromProfileName(const char* profileName)
    {
        static constexpr const char Prefix[] = "ScriptStruct ";
        if (profileName == nullptr)
            return {};

        const std::string name(profileName);
        if (name.rfind(Prefix, 0) != 0)
            return {};

        const std::string suffix = name.substr(sizeof(Prefix) - 1);
        std::wstring path = L"/Script/";
        path.reserve(path.size() + suffix.size());
        for (const unsigned char ch : suffix)
            path.push_back(static_cast<wchar_t>(ch));
        return path;
    }

    const ResolvedRegistryEntry* FindResolvedEntry(const UScriptStruct* descriptor)
    {
        if (descriptor == nullptr || !g_descriptorRegistryReady)
            return nullptr;

        const auto it = g_registryByPointer.find(descriptor);
        if (it == g_registryByPointer.end())
            return nullptr;

        return &g_resolvedRegistry[it->second];
    }

    bool ValidateDescriptor(
        const UScriptStruct* descriptor,
        const ResolvedRegistryEntry& resolved)
    {
        if (descriptor == nullptr ||
            resolved.Profile == nullptr ||
            descriptor != resolved.Descriptor)
            return false;

        const RegistryEntry& profile = *resolved.Profile;

        if (descriptor->SuperStruct != resolved.ExpectedBase)
            return false;

        if (profile.EmptyMarkerSizeRule)
        {
            return (descriptor->Size == 0 || descriptor->Size == 1) &&
                descriptor->MinAlignment == profile.ExpectedMinAlignment &&
                descriptor->ChildProperties == nullptr;
        }

        return descriptor->Size == profile.ExpectedSize &&
            descriptor->MinAlignment == profile.ExpectedMinAlignment;
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

            if (regionStart > (std::numeric_limits<std::uintptr_t>::max)() - regionSize)
                return false;

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
        if (countWide > (std::numeric_limits<std::size_t>::max)() / sizeof(T))
            return false;

        return IsReadableRange(data, countWide * sizeof(T));
    }

    template <typename T, typename Visitor>
    bool CheckedEach(const TArray<T>& values, Visitor&& visitor)
    {
        if (!ValidateArray(values))
        {
            g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const std::int32_t count = values.Num();
        for (std::int32_t index = 0; index < count; ++index)
        {
            if (!values.IsValidIndex(index))
            {
                g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            if (!visitor(values[index]))
                return false;
        }

        return true;
    }

    template <typename K, typename V, typename Visitor>
    bool CheckedSparseMap(
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
            g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto& flags = values.GetAllocationFlags();
        if (flags.Num() < 0 ||
            flags.Max() < 0 ||
            flags.Num() != allocated ||
            flags.Max() < flags.Num())
        {
            g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (allocated == 0)
        {
            if (live != 0)
            {
                g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            return true;
        }

        if (!values.IsValid() || flags.GetData() == nullptr)
        {
            g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const auto wordCount =
            (static_cast<std::size_t>(allocated) + 31u) / 32u;
        if (wordCount >
            (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t))
        {
            g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!IsReadableRange(
                flags.GetData(),
                wordCount * sizeof(std::uint32_t)))
        {
            g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
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
            g_semanticContainerFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        return true;
    }

    bool EmitNumeric(
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

    bool CollectSemanticValues(
        const RegistryEntry& profile,
        const std::uint8_t* memory,
        std::vector<std::uint32_t>& staged)
    {
        if (memory == nullptr)
            return false;

        const std::string_view name(profile.FullName);

        if (name == "ScriptStruct AuActorPlacement.AuSplineConnectionFragment")
        {
            const auto& f =
                *reinterpret_cast<const FAuSplineConnectionFragment*>(memory);
            return EmitNumeric(staged, f.StartEntity.ID) &&
                EmitNumeric(staged, f.EndEntity.ID);
        }

        if (name == "ScriptStruct Chimera.CrBuildingInfectionFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrBuildingInfectionFragment*>(memory);

            if (!CheckedEach(
                    f.StandaloneInfectionEntities,
                    [&](const FCrMassPersistentEntityID& value)
                    {
                        return EmitNumeric(staged, value.ID);
                    }))
            {
                return false;
            }

            return EmitNumeric(staged, f.InfectionEntityHandle.ID);
        }

        if (name == "ScriptStruct Chimera.CrHeaterCoolerFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrHeaterCoolerFragment*>(memory);

            return CheckedEach(
                f.Connections,
                [&](const FCrHeaterCoolerConnectionData& value)
                {
                    return EmitNumeric(staged, value.Entity.ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrLogisticsAgentFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrLogisticsAgentFragment*>(memory);

            if (!EmitNumeric(staged, f.RequestUId.ID) ||
                !EmitNumeric(staged, f.CurrentMovementStart.ID) ||
                !EmitNumeric(staged, f.CurrentMovementTarget.ID))
            {
                return false;
            }

            return CheckedEach(
                f.CurrentPath,
                [&](const FCrLogisticsPathSegmentData& value)
                {
                    return EmitNumeric(staged, value.Start.Entity.ID) &&
                        EmitNumeric(staged, value.Connection.Entity.ID) &&
                        EmitNumeric(staged, value.End.Entity.ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrLogisticsIntersectionFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrLogisticsIntersectionFragment*>(memory);

            if (!CheckedEach(
                    f.State.Items,
                    [&](const FCrLogisticsIntersectionItem& value)
                    {
                        return EmitNumeric(staged, value.Entity.Entity.ID) &&
                            EmitNumeric(staged, value.TargetLine.Entity.ID);
                    }) ||
                !CheckedEach(
                    f.State.WaitingItems,
                    [&](const FCrMassEntityReplicationHelper& value)
                    {
                        return EmitNumeric(staged, value.Entity.ID);
                    }))
            {
                return false;
            }

            return CheckedEach(
                f.CachedMoveSpeedPerLine,
                [&](const FCrLogisticsIntersectionMoveSpeedPerLine& value)
                {
                    return EmitNumeric(staged, value.Entity.Entity.ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrLogisticsLineFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrLogisticsLineFragment*>(memory);

            if (!CheckedEach(
                    f.State.OrderedItems,
                    [&](const FCrLogisticsLineItem& value)
                    {
                        return EmitNumeric(staged, value.Entity.Entity.ID);
                    }))
            {
                return false;
            }

            return CheckedEach(
                f.State.WaitingItems,
                [&](const FCrLogisticsLineWaitingItem& value)
                {
                    return EmitNumeric(staged, value.Entity.Entity.ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrLogisticsRequestContainerFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrLogisticsRequestContainerFragment*>(memory);

            if (!CheckedEach(
                    f.Requests,
                    [&](const FCrMassPersistentEntityID& value)
                    {
                        return EmitNumeric(staged, value.ID);
                    }))
            {
                return false;
            }

            return CheckedEach(
                f.PendingRequests,
                [&](const FCrLogisticsRequestData& request)
                {
                    if (!EmitNumeric(staged, request.UId.ID) ||
                        !EmitNumeric(staged, request.RequesterEntity.ID))
                    {
                        return false;
                    }

                    return CheckedEach(
                        request.RuntimeData,
                        [&](const FCrLogisticsRequestRuntimeData& runtime)
                        {
                            return EmitNumeric(staged, runtime.AgentEntity.ID) &&
                                EmitNumeric(staged, runtime.ItemSource.ID) &&
                                EmitNumeric(staged, runtime.ItemDestination.ID);
                        });
                });
        }

        if (name == "ScriptStruct Chimera.CrLogisticsRoundaboutFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrLogisticsRoundaboutFragment*>(memory);

            return CheckedEach(
                f.State.Items,
                [&](const FCrLogisticsRoundaboutItem& value)
                {
                    return EmitNumeric(staged, value.Entity.Entity.ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrLogisticsSocketsFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrLogisticsSocketsFragment*>(memory);

            return CheckedEach(
                f.Sockets,
                [&](const FCrLogisticsSocketRuntimeData& value)
                {
                    return EmitNumeric(
                        staged,
                        value.SocketPairInvisibleConnector.ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrLogisticsVerticalConnectorFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrLogisticsVerticalConnectorFragment*>(memory);

            if (!CheckedEach(
                    f.State.OrderedItems,
                    [&](const FCrLogisticsIntersectionItem& value)
                    {
                        return EmitNumeric(staged, value.Entity.Entity.ID) &&
                            EmitNumeric(staged, value.TargetLine.Entity.ID);
                    }))
            {
                return false;
            }

            return CheckedEach(
                f.CachedMoveSpeedPerLine,
                [&](const FCrLogisticsIntersectionMoveSpeedPerLine& value)
                {
                    return EmitNumeric(staged, value.Entity.Entity.ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrMassBuildingBaseCoreFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrMassBuildingBaseCoreFragment*>(memory);
            return EmitNumeric(staged, f.DeconstructionTimerInstigator.ID);
        }

        if (name == "ScriptStruct Chimera.CrMassTemperatureFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrMassTemperatureFragment*>(memory);

            return CheckedSparseMap(
                f.Modifiers.Modifiers,
                [&](const auto& pair)
                {
                    return EmitNumeric(staged, pair.Key().ID);
                });
        }

        if (name == "ScriptStruct Chimera.CrStandaloneInfectionFragment")
        {
            const auto& f =
                *reinterpret_cast<const FCrStandaloneInfectionFragment*>(memory);
            return EmitNumeric(staged, f.InfectedBuilding.ID);
        }

        return false;
    }

    bool CommitSemanticValues(
        const RegistryEntry& profile,
        const std::uint8_t* memory)
    {
        g_semanticAttempts.fetch_add(1, std::memory_order_relaxed);

        std::vector<std::uint32_t> staged;
        try
        {
            if (!CollectSemanticValues(profile, memory, staged))
            {
                g_semanticFailures.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            std::uint64_t zeros = 0;
            std::uint64_t sentinels = 0;
            for (const std::uint32_t value : staged)
            {
                if (value == 0)
                    ++zeros;
                if (value == (std::numeric_limits<std::uint32_t>::max)())
                    ++sentinels;
            }

            {
                std::lock_guard<std::mutex> lock(g_semanticValuesMutex);
                g_semanticValues.insert(
                    g_semanticValues.end(),
                    staged.begin(),
                    staged.end());
            }

            g_semanticNumericValues.fetch_add(
                static_cast<std::uint64_t>(staged.size()),
                std::memory_order_relaxed);
            g_semanticZeroValues.fetch_add(zeros, std::memory_order_relaxed);
            g_semanticInvalidSentinels.fetch_add(
                sentinels,
                std::memory_order_relaxed);
            g_semanticSuccesses.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        catch (...)
        {
            g_semanticFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    void ObservePayload(const FInstancedStruct& payload)
    {
        g_massPayloads.fetch_add(1, std::memory_order_relaxed);

        const UScriptStruct* type = payload.ScriptStruct;
        const bool hasType = type != nullptr;
        const bool hasMemory = payload.StructMemory != nullptr;

        if (!hasType && !hasMemory)
        {
            g_emptyPayloads.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (hasType != hasMemory)
        {
            g_malformedPayloads.fetch_add(1, std::memory_order_relaxed);
            g_unsupportedPayloads.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: malformed Mass fragment payload type=%p memory=%p",
                type,
                payload.StructMemory);
            return;
        }

        const ResolvedRegistryEntry* resolved = FindResolvedEntry(type);
        if (resolved == nullptr || resolved->Profile == nullptr)
        {
            g_unsupportedPayloads.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: unsupported Mass fragment descriptor: %p",
                type);
            return;
        }

        if (!ValidateDescriptor(type, *resolved))
        {
            g_schemaMismatches.fetch_add(1, std::memory_order_relaxed);
            g_unsupportedPayloads.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass fragment schema mismatch: descriptor=%p size=%d alignment=%d actualBase=%p expectedBase=%p",
                type,
                type->Size,
                static_cast<int>(type->MinAlignment),
                type->SuperStruct,
                resolved->ExpectedBase);
            return;
        }

        g_classifiedPayloads.fetch_add(1, std::memory_order_relaxed);
        if (resolved->Profile->Class == FragmentClass::PidBearing)
        {
            g_pidBearingPayloads.fetch_add(1, std::memory_order_relaxed);

            if (!CommitSemanticValues(
                    *resolved->Profile,
                    payload.StructMemory))
            {
                LOG_WARN(
                    "PersistentIdFix: Mass semantic PID collection failed: descriptor=%p",
                    type);
            }
        }
        else
        {
            g_pidFreePayloads.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
namespace PersistentIdFixMassFragmentClassifier
{
    bool InitializeDescriptorRegistry(IPluginEngineEvents* engineEvents)
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);

        g_descriptorRegistryReady = false;
        g_massSaveDataDescriptor = nullptr;
        g_registryByPointer.clear();
        g_resolvedRegistry.clear();

        if (engineEvents == nullptr ||
            engineEvents->GetStaticFindObjectSafeByNameAddress == nullptr)
        {
            LOG_WARN(
                "PersistentIdFix: descriptor registry unavailable: native object lookup interface missing");
            return false;
        }

        const uintptr_t resolverAddress =
            engineEvents->GetStaticFindObjectSafeByNameAddress();
        if (resolverAddress == 0)
        {
            LOG_WARN(
                "PersistentIdFix: descriptor registry unavailable: SafeByName resolver unresolved");
            return false;
        }

        const auto findSafe =
            reinterpret_cast<StaticFindObjectSafeByNameFn>(resolverAddress);

        UObject* scriptStructClassObject =
            findSafe(nullptr, nullptr, L"/Script/CoreUObject.ScriptStruct", true);
        if (scriptStructClassObject == nullptr)
        {
            LOG_WARN(
                "PersistentIdFix: descriptor registry unavailable: ScriptStruct class lookup failed");
            return false;
        }

        UClass* scriptStructClass =
            reinterpret_cast<UClass*>(scriptStructClassObject);

        UObject* massObject =
            findSafe(
                scriptStructClass,
                nullptr,
                L"/Script/Chimera.CrMassSaveData",
                true);
        if (massObject == nullptr)
        {
            LOG_WARN(
                "PersistentIdFix: descriptor registry unavailable: CrMassSaveData lookup failed");
            return false;
        }

        const UScriptStruct* massDescriptor =
            reinterpret_cast<const UScriptStruct*>(massObject);
        if (massDescriptor->Size != 0x4F8 ||
            massDescriptor->MinAlignment != 8)
        {
            LOG_WARN(
                "PersistentIdFix: descriptor registry unavailable: CrMassSaveData schema mismatch size=%d alignment=%d",
                massDescriptor->Size,
                static_cast<int>(massDescriptor->MinAlignment));
            return false;
        }

        const auto& registry = Registry();
        g_resolvedRegistry.reserve(registry.size());
        g_registryByPointer.reserve(registry.size());

        for (const RegistryEntry& profile : registry)
        {
            const std::wstring path =
                NativePathFromProfileName(profile.FullName);
            const std::wstring basePath =
                NativePathFromProfileName(profile.ImmediateBaseFullName);

            if (path.empty() || basePath.empty())
            {
                LOG_WARN(
                    "PersistentIdFix: descriptor registry unavailable: invalid profile path: %s",
                    profile.FullName != nullptr ? profile.FullName : "<null>");
                g_resolvedRegistry.clear();
                g_registryByPointer.clear();
                return false;
            }

            UObject* object =
                findSafe(scriptStructClass, nullptr, path.c_str(), true);
            UObject* baseObject =
                findSafe(scriptStructClass, nullptr, basePath.c_str(), true);

            if (object == nullptr || baseObject == nullptr)
            {
                LOG_WARN(
                    "PersistentIdFix: descriptor registry unavailable: lookup failed for %s",
                    profile.FullName);
                g_resolvedRegistry.clear();
                g_registryByPointer.clear();
                return false;
            }

            const UScriptStruct* descriptor =
                reinterpret_cast<const UScriptStruct*>(object);
            const UStruct* expectedBase =
                reinterpret_cast<const UStruct*>(baseObject);

            if (g_registryByPointer.find(descriptor) != g_registryByPointer.end())
            {
                LOG_WARN(
                    "PersistentIdFix: descriptor registry unavailable: duplicate descriptor pointer %p",
                    descriptor);
                g_resolvedRegistry.clear();
                g_registryByPointer.clear();
                return false;
            }

            ResolvedRegistryEntry resolved{
                &profile,
                descriptor,
                expectedBase };

            if (!ValidateDescriptor(descriptor, resolved))
            {
                const bool baseMatches =
                    descriptor->SuperStruct == resolved.ExpectedBase;
                const bool sizeMatches = profile.EmptyMarkerSizeRule
                    ? (descriptor->Size == 0 || descriptor->Size == 1)
                    : (descriptor->Size == profile.ExpectedSize);
                const bool alignmentMatches =
                    descriptor->MinAlignment == profile.ExpectedMinAlignment;
                const bool markerPropertiesMatch =
                    !profile.EmptyMarkerSizeRule ||
                    descriptor->ChildProperties == nullptr;

                LOG_WARN(
                    "PersistentIdFix: descriptor registry unavailable: schema mismatch for %s descriptor=%p actualBase=%p expectedBase=%p baseMatch=%d actualSize=%d expectedSize=%d emptyMarkerRule=%d sizeMatch=%d actualAlignment=%d expectedAlignment=%d alignmentMatch=%d childProperties=%p markerPropertiesMatch=%d",
                    profile.FullName,
                    descriptor,
                    descriptor->SuperStruct,
                    resolved.ExpectedBase,
                    baseMatches ? 1 : 0,
                    descriptor->Size,
                    profile.ExpectedSize,
                    profile.EmptyMarkerSizeRule ? 1 : 0,
                    sizeMatches ? 1 : 0,
                    static_cast<int>(descriptor->MinAlignment),
                    static_cast<int>(profile.ExpectedMinAlignment),
                    alignmentMatches ? 1 : 0,
                    descriptor->ChildProperties,
                    markerPropertiesMatch ? 1 : 0);

                g_resolvedRegistry.clear();
                g_registryByPointer.clear();
                return false;
            }

            const std::size_t index = g_resolvedRegistry.size();
            g_resolvedRegistry.push_back(resolved);
            g_registryByPointer.emplace(descriptor, index);
        }

        g_massSaveDataDescriptor = massDescriptor;
        g_descriptorRegistryReady = true;

        LOG_INFO(
            "PersistentIdFix: Mass descriptor registry ready: profiles=%llu Mass=%p resolver=%p",
            static_cast<unsigned long long>(g_resolvedRegistry.size()),
            g_massSaveDataDescriptor,
            reinterpret_cast<void*>(resolverAddress));
        return true;
    }

    void ResetDescriptorRegistry()
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        g_descriptorRegistryReady = false;
        g_massSaveDataDescriptor = nullptr;
        g_registryByPointer.clear();
        g_resolvedRegistry.clear();
    }

    bool IsDescriptorRegistryReady()
    {
        return g_descriptorRegistryReady;
    }

    void ObserveMassSaveData(
        const UScriptStruct* structType,
        const FCrMassSaveData* massSaveData)
    {
        if (!IsDescriptorRegistryReady())
        {
            g_massDescriptorMismatches.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass fragment classification unavailable: descriptor registry is not ready");
            return;
        }

        if (structType == nullptr || massSaveData == nullptr)
        {
            g_massDescriptorMismatches.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass fragment classification received a null descriptor or destination");
            return;
        }

        if (structType != g_massSaveDataDescriptor)
        {
            g_massDescriptorMismatches.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass destination descriptor mismatch: actual=%p expected=%p",
                structType,
                g_massSaveDataDescriptor);
            return;
        }

        if (structType->Size != 0x4F8 ||
            structType->MinAlignment != 8)
        {
            g_massDescriptorMismatches.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass destination schema mismatch: descriptor=%p size=%d alignment=%d",
                structType,
                structType->Size,
                static_cast<int>(structType->MinAlignment));
            return;
        }

        const auto& entities = massSaveData->Entities;
        const std::int32_t expected = entities.Num();
        const std::int32_t allocated = entities.NumAllocated();

        if (expected < 0 || allocated < 0 || expected > allocated)
        {
            g_entityCountMismatches.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass entity map header invalid: Num=%d NumAllocated=%d",
                expected,
                allocated);
            return;
        }

        std::int32_t visited = 0;
        for (auto it = begin(entities); it != end(entities); ++it)
        {
            ++visited;
            g_entitySlotsVisited.fetch_add(1, std::memory_order_relaxed);

            const FCrEntitySaveData& entity = it->Value();

            for (const FInstancedStruct& payload : entity.FragmentValues)
                ObservePayload(payload);

            // Tags are a separate domain in Step 3C.
            g_tagPayloads.fetch_add(
                static_cast<std::uint64_t>(entity.Tags.Num()),
                std::memory_order_relaxed);
        }

        if (visited != expected)
        {
            g_entityCountMismatches.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass entity sparse-map traversal mismatch: visited=%d Num=%d NumAllocated=%d",
                visited,
                expected,
                allocated);
        }
    }
    ClassificationSnapshot GetSnapshot()
    {
        ClassificationSnapshot snapshot;
        snapshot.massPayloads = g_massPayloads.load(std::memory_order_relaxed);
        snapshot.classifiedPayloads = g_classifiedPayloads.load(std::memory_order_relaxed);
        snapshot.pidBearingPayloads = g_pidBearingPayloads.load(std::memory_order_relaxed);
        snapshot.pidFreePayloads = g_pidFreePayloads.load(std::memory_order_relaxed);
        snapshot.emptyPayloads = g_emptyPayloads.load(std::memory_order_relaxed);
        snapshot.malformedPayloads = g_malformedPayloads.load(std::memory_order_relaxed);
        snapshot.unsupportedPayloads = g_unsupportedPayloads.load(std::memory_order_relaxed);
        snapshot.schemaMismatches = g_schemaMismatches.load(std::memory_order_relaxed);
        snapshot.massDescriptorMismatches = g_massDescriptorMismatches.load(std::memory_order_relaxed);
        snapshot.entitySlotsVisited = g_entitySlotsVisited.load(std::memory_order_relaxed);
        snapshot.entityCountMismatches = g_entityCountMismatches.load(std::memory_order_relaxed);
        snapshot.tagPayloads = g_tagPayloads.load(std::memory_order_relaxed);
        snapshot.semanticAttempts = g_semanticAttempts.load(std::memory_order_relaxed);
        snapshot.semanticSuccesses = g_semanticSuccesses.load(std::memory_order_relaxed);
        snapshot.semanticFailures = g_semanticFailures.load(std::memory_order_relaxed);
        snapshot.semanticNumericValues = g_semanticNumericValues.load(std::memory_order_relaxed);
        snapshot.semanticZeroValues = g_semanticZeroValues.load(std::memory_order_relaxed);
        snapshot.semanticInvalidSentinels = g_semanticInvalidSentinels.load(std::memory_order_relaxed);
        snapshot.semanticContainerFailures = g_semanticContainerFailures.load(std::memory_order_relaxed);
        return snapshot;
    }

    void LogSnapshot(const char* phase)
    {
        const ClassificationSnapshot snapshot = GetSnapshot();
        LOG_INFO(
            "PersistentIdFix: Mass fragments [%s]: payloads=%llu classified=%llu pidBearing=%llu pidFree=%llu empty=%llu malformed=%llu unsupported=%llu schemaMismatch=%llu massDescriptorMismatch=%llu entities=%llu mapMismatch=%llu tags=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.massPayloads),
            static_cast<unsigned long long>(snapshot.classifiedPayloads),
            static_cast<unsigned long long>(snapshot.pidBearingPayloads),
            static_cast<unsigned long long>(snapshot.pidFreePayloads),
            static_cast<unsigned long long>(snapshot.emptyPayloads),
            static_cast<unsigned long long>(snapshot.malformedPayloads),
            static_cast<unsigned long long>(snapshot.unsupportedPayloads),
            static_cast<unsigned long long>(snapshot.schemaMismatches),
            static_cast<unsigned long long>(snapshot.massDescriptorMismatches),
            static_cast<unsigned long long>(snapshot.entitySlotsVisited),
            static_cast<unsigned long long>(snapshot.entityCountMismatches),
            static_cast<unsigned long long>(snapshot.tagPayloads));

        LOG_INFO(
            "PersistentIdFix: Mass semantic PIDs [%s]: attempts=%llu success=%llu failed=%llu values=%llu zero=%llu invalidSentinel=%llu containerFailures=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.semanticAttempts),
            static_cast<unsigned long long>(snapshot.semanticSuccesses),
            static_cast<unsigned long long>(snapshot.semanticFailures),
            static_cast<unsigned long long>(snapshot.semanticNumericValues),
            static_cast<unsigned long long>(snapshot.semanticZeroValues),
            static_cast<unsigned long long>(snapshot.semanticInvalidSentinels),
            static_cast<unsigned long long>(snapshot.semanticContainerFailures));
    }

    void Reset()
    {
        g_massPayloads.store(0, std::memory_order_relaxed);
        g_classifiedPayloads.store(0, std::memory_order_relaxed);
        g_pidBearingPayloads.store(0, std::memory_order_relaxed);
        g_pidFreePayloads.store(0, std::memory_order_relaxed);
        g_emptyPayloads.store(0, std::memory_order_relaxed);
        g_malformedPayloads.store(0, std::memory_order_relaxed);
        g_unsupportedPayloads.store(0, std::memory_order_relaxed);
        g_schemaMismatches.store(0, std::memory_order_relaxed);
        g_massDescriptorMismatches.store(0, std::memory_order_relaxed);
        g_entitySlotsVisited.store(0, std::memory_order_relaxed);
        g_entityCountMismatches.store(0, std::memory_order_relaxed);
        g_tagPayloads.store(0, std::memory_order_relaxed);
        g_semanticAttempts.store(0, std::memory_order_relaxed);
        g_semanticSuccesses.store(0, std::memory_order_relaxed);
        g_semanticFailures.store(0, std::memory_order_relaxed);
        g_semanticNumericValues.store(0, std::memory_order_relaxed);
        g_semanticZeroValues.store(0, std::memory_order_relaxed);
        g_semanticInvalidSentinels.store(0, std::memory_order_relaxed);
        g_semanticContainerFailures.store(0, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(g_semanticValuesMutex);
        g_semanticValues.clear();
    }
}
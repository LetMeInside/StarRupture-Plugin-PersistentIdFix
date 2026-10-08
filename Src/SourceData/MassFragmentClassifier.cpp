#include "MassFragmentClassifier.h"

#include "plugin_helpers.h"
#include "IndependentSourceCollector.h"
#include "ReflectionDiagnostics.h"

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

    std::atomic<std::uint64_t> g_tagClassified{0};
    std::atomic<std::uint64_t> g_tagEmpty{0};
    std::atomic<std::uint64_t> g_tagMalformed{0};
    std::atomic<std::uint64_t> g_tagUnsupported{0};
    std::atomic<std::uint64_t> g_tagSchemaMismatches{0};
    std::atomic<std::uint64_t> g_tagContainerFailures{0};

    std::atomic<std::uint64_t> g_identityAttempts{0};
    std::atomic<std::uint64_t> g_identitySuccesses{0};
    std::atomic<std::uint64_t> g_identityFailures{0};
    std::atomic<std::uint64_t> g_identityValues{0};
    std::atomic<std::uint64_t> g_identityZeroValues{0};
    std::atomic<std::uint64_t> g_identityInvalidSentinels{0};
    std::atomic<std::uint64_t> g_identityContainerFailures{0};

    std::mutex g_identityValuesMutex;
    std::vector<std::uint32_t> g_identitySourceValues;

    std::atomic<std::uint64_t> g_massRemainderAttempts{0};
    std::atomic<std::uint64_t> g_massRemainderSuccesses{0};
    std::atomic<std::uint64_t> g_massRemainderFailures{0};
    std::atomic<std::uint64_t> g_massRemainderValues{0};
    std::atomic<std::uint64_t> g_massRemainderZeroValues{0};
    std::atomic<std::uint64_t> g_massRemainderInvalidSentinels{0};
    std::atomic<std::uint64_t> g_massRemainderContainerFailures{0};

    std::atomic<std::uint64_t> g_stabilityValues{0};
    std::atomic<std::uint64_t> g_electricityValues{0};
    std::atomic<std::uint64_t> g_logisticsValues{0};
    std::atomic<std::uint64_t> g_waveValues{0};
    std::atomic<std::uint64_t> g_spawnOwnershipValues{0};
    std::atomic<std::uint64_t> g_foundableValues{0};

    std::mutex g_massRemainderValuesMutex;
    std::vector<std::uint32_t> g_massRemainderSourceValues;

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

    struct TagRegistryEntry
    {
        const char* FullName;
        const char* ImmediateBaseFullName;
    };

    const std::array<TagRegistryEntry, 12>& TagRegistry()
    {
        static const std::array<TagRegistryEntry, 12> registry{{
            { "ScriptStruct ChimeraMassCommon.CrMassSavableTag", "ScriptStruct MassEntity.MassTag" },
            { "ScriptStruct Chimera.CrMassEnviroWaveStageGrowbackTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrBuildingInfectionNonZeroTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrMassInEnviroWaveTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrBuildingBaseCoreUnavailableTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrBuildingInfectionActiveTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrMassEnviroWaveStagePrewaveTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrMassEnviroWaveStageMovingTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrMassEnviroWaveStageFadeoutTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrMassEnviroWaveTypeHeatTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrMassEnviroWaveTypeColdTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" },
            { "ScriptStruct Chimera.CrMassTemperatureUpdatingTag", "ScriptStruct ChimeraMassCommon.CrMassSavableTag" }
        }};
        return registry;
    }

    struct ResolvedTagRegistryEntry
    {
        const TagRegistryEntry* Profile = nullptr;
        const UScriptStruct* Descriptor = nullptr;
        const UStruct* ExpectedBase = nullptr;
    };

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
    static std::vector<ResolvedTagRegistryEntry> g_resolvedTagRegistry;
    static std::unordered_map<const UScriptStruct*, std::size_t> g_tagRegistryByPointer;
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

    const ResolvedTagRegistryEntry* FindResolvedTagEntry(const UScriptStruct* descriptor)
    {
        if (descriptor == nullptr || !g_descriptorRegistryReady)
            return nullptr;
        const auto it = g_tagRegistryByPointer.find(descriptor);
        return it == g_tagRegistryByPointer.end()
            ? nullptr
            : &g_resolvedTagRegistry[it->second];
    }

    bool ValidateTagDescriptor(
        const UScriptStruct* descriptor,
        const ResolvedTagRegistryEntry& resolved)
    {
        return descriptor != nullptr &&
            resolved.Profile != nullptr &&
            descriptor == resolved.Descriptor &&
            descriptor->SuperStruct == resolved.ExpectedBase &&
            descriptor->Size == 1 &&
            descriptor->MinAlignment == 1 &&
            descriptor->ChildProperties == nullptr;
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

    void RecordContainerFailure(std::atomic<std::uint64_t>* counter)
    {
        if (counter != nullptr)
            counter->fetch_add(1, std::memory_order_relaxed);
    }

    template <typename T, typename Visitor>
    bool CheckedEach(
        const TArray<T>& values,
        Visitor&& visitor,
        std::atomic<std::uint64_t>* failureCounter = &g_semanticContainerFailures)
    {
        if (!ValidateArray(values))
        {
            RecordContainerFailure(failureCounter);
            return false;
        }

        const std::int32_t count = values.Num();
        for (std::int32_t index = 0; index < count; ++index)
        {
            if (!values.IsValidIndex(index))
            {
                RecordContainerFailure(failureCounter);
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
        Visitor&& visitor,
        std::atomic<std::uint64_t>* failureCounter = &g_semanticContainerFailures)
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
            RecordContainerFailure(failureCounter);
            return false;
        }

        const auto& flags = values.GetAllocationFlags();
        if (flags.Num() < 0 ||
            flags.Max() < 0 ||
            flags.Num() != allocated ||
            flags.Max() < flags.Num())
        {
            RecordContainerFailure(failureCounter);
            return false;
        }

        if (allocated == 0)
        {
            if (live != 0)
            {
                RecordContainerFailure(failureCounter);
                return false;
            }
            return true;
        }

        if (!values.IsValid() || flags.GetData() == nullptr)
        {
            RecordContainerFailure(failureCounter);
            return false;
        }

        const auto wordCount =
            (static_cast<std::size_t>(allocated) + 31u) / 32u;
        if (wordCount >
            (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t))
        {
            RecordContainerFailure(failureCounter);
            return false;
        }

        if (!IsReadableRange(
                flags.GetData(),
                wordCount * sizeof(std::uint32_t)))
        {
            RecordContainerFailure(failureCounter);
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
            RecordContainerFailure(failureCounter);
            return false;
        }

        const auto allocatedWide = static_cast<std::size_t>(allocated);
        if (allocatedWide >
            (std::numeric_limits<std::size_t>::max)() / sizeof(SlotType) ||
            !IsReadableRange(
                sparseData.Data,
                allocatedWide * sizeof(SlotType)))
        {
            RecordContainerFailure(failureCounter);
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
            RecordContainerFailure(failureCounter);
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

    bool ObserveTag(const FInstancedStruct& payload)
    {
        g_tagPayloads.fetch_add(1, std::memory_order_relaxed);

        const UScriptStruct* type = payload.ScriptStruct;
        const bool hasType = type != nullptr;
        const bool hasMemory = payload.StructMemory != nullptr;

        if (!hasType && !hasMemory)
        {
            g_tagEmpty.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if (hasType != hasMemory)
        {
            g_tagMalformed.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const ResolvedTagRegistryEntry* resolved = FindResolvedTagEntry(type);
        if (resolved == nullptr || resolved->Profile == nullptr)
        {
            g_tagUnsupported.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!ValidateTagDescriptor(type, *resolved) ||
            !IsReadableRange(payload.StructMemory, 1))
        {
            g_tagSchemaMismatches.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        g_tagClassified.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool CommitEntityIdentities(const FCrMassSaveData& mass)
    {
        g_identityAttempts.fetch_add(1, std::memory_order_relaxed);

        std::vector<std::uint32_t> staged;
        try
        {
            if (!CheckedSparseMap(
                    mass.Entities,
                    [&](const auto& pair)
                    {
                        return EmitNumeric(staged, pair.Key().ID);
                    },
                    &g_identityContainerFailures))
            {
                g_identityFailures.fetch_add(1, std::memory_order_relaxed);
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
                std::lock_guard<std::mutex> lock(g_identityValuesMutex);
                g_identitySourceValues.insert(
                    g_identitySourceValues.end(),
                    staged.begin(),
                    staged.end());
            }

            g_identityValues.fetch_add(
                static_cast<std::uint64_t>(staged.size()),
                std::memory_order_relaxed);
            g_identityZeroValues.fetch_add(zeros, std::memory_order_relaxed);
            g_identityInvalidSentinels.fetch_add(
                sentinels,
                std::memory_order_relaxed);
            g_identitySuccesses.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        catch (...)
        {
            g_identityFailures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    struct MassRemainderDomainCounts
    {
        std::uint64_t stability = 0;
        std::uint64_t electricity = 0;
        std::uint64_t logistics = 0;
        std::uint64_t wave = 0;
        std::uint64_t spawnOwnership = 0;
        std::uint64_t foundable = 0;
    };

    bool EmitMassRemainder(
        std::vector<std::uint32_t>& staged,
        std::uint32_t value,
        std::uint64_t& domainCount)
    {
        if (!EmitNumeric(staged, value))
            return false;

        ++domainCount;
        return true;
    }

    template <typename T>
    bool CollectMassPidArray(
        const TArray<T>& values,
        std::vector<std::uint32_t>& staged,
        std::uint64_t& domainCount)
    {
        return CheckedEach(
            values,
            [&](const T& value)
            {
                return EmitMassRemainder(
                    staged,
                    value.ID,
                    domainCount);
            },
            &g_massRemainderContainerFailures);
    }

    bool CollectMassRemainderValues(
        const FCrMassSaveData& mass,
        std::vector<std::uint32_t>& staged,
        MassRemainderDomainCounts& counts)
    {
        const auto& stability = mass.StabilitySubsystemState;
        const auto& graph = stability.GraphData;

        if (!CheckedSparseMap(
                graph.Neighbours,
                [&](const auto& pair)
                {
                    return EmitMassRemainder(
                            staged,
                            pair.Key().ID,
                            counts.stability) &&
                        CollectMassPidArray(
                            pair.Value().Values,
                            staged,
                            counts.stability);
                },
                &g_massRemainderContainerFailures) ||
            !CheckedSparseMap(
                graph.NodeDatas,
                [&](const auto& pair)
                {
                    return EmitMassRemainder(
                        staged,
                        pair.Key().ID,
                        counts.stability);
                },
                &g_massRemainderContainerFailures) ||
            !EmitMassRemainder(
                staged,
                graph.GroundNode.ID,
                counts.stability))
        {
            return false;
        }

        if (!CheckedSparseMap(
                stability.CustomConnectionData,
                [&](const auto& pair)
                {
                    const auto& value = pair.Value();

                    if (!EmitMassRemainder(
                            staged,
                            pair.Key().ID,
                            counts.stability) ||
                        !CollectMassPidArray(
                            value.Ramps,
                            staged,
                            counts.stability) ||
                        !CollectMassPidArray(
                            value.Buildings,
                            staged,
                            counts.stability))
                    {
                        return false;
                    }

                    return CheckedSparseMap(
                        value.SocketConnections,
                        [&](const auto& socketPair)
                        {
                            return CollectMassPidArray(
                                socketPair.Value().Values,
                                staged,
                                counts.stability);
                        },
                        &g_massRemainderContainerFailures);
                },
                &g_massRemainderContainerFailures))
        {
            return false;
        }

        const auto collectStabilityPidArrayMap =
            [&](const auto& values) -> bool
            {
                return CheckedSparseMap(
                    values,
                    [&](const auto& pair)
                    {
                        return EmitMassRemainder(
                                staged,
                                pair.Key().ID,
                                counts.stability) &&
                            CollectMassPidArray(
                                pair.Value().Values,
                                staged,
                                counts.stability);
                    },
                    &g_massRemainderContainerFailures);
            };

        if (!collectStabilityPidArrayMap(stability.RampConnectionData) ||
            !collectStabilityPidArrayMap(stability.BuildingFoundationData))
        {
            return false;
        }

        const auto& electricity = mass.ElectricitySubsystemState;

        if (!CheckedSparseMap(
                electricity.NodeData,
                [&](const auto& pair)
                {
                    const auto& node = pair.Value();

                    if (!EmitMassRemainder(
                            staged,
                            pair.Key().UId.ID,
                            counts.electricity) ||
                        !EmitMassRemainder(
                            staged,
                            node.Handle.ID,
                            counts.electricity))
                    {
                        return false;
                    }

                    return CheckedEach(
                        node.NeighbourData,
                        [&](const auto& neighbour)
                        {
                            return EmitMassRemainder(
                                    staged,
                                    neighbour.Neighbour.ID,
                                    counts.electricity) &&
                                CollectMassPidArray(
                                    neighbour.Connectors,
                                    staged,
                                    counts.electricity);
                        },
                        &g_massRemainderContainerFailures);
                },
                &g_massRemainderContainerFailures))
        {
            return false;
        }

        if (!CheckedSparseMap(
                electricity.SubgraphData,
                [&](const auto& pair)
                {
                    const auto& value = pair.Value();
                    return CollectMassPidArray(
                            value.Nodes,
                            staged,
                            counts.electricity) &&
                        CollectMassPidArray(
                            value.Connectors,
                            staged,
                            counts.electricity);
                },
                &g_massRemainderContainerFailures) ||
            !CheckedSparseMap(
                electricity.ConnectorData,
                [&](const auto& pair)
                {
                    const auto& value = pair.Value();
                    return EmitMassRemainder(
                            staged,
                            pair.Key().ID,
                            counts.electricity) &&
                        EmitMassRemainder(
                            staged,
                            value.Node1.ID,
                            counts.electricity) &&
                        EmitMassRemainder(
                            staged,
                            value.Node2.ID,
                            counts.electricity);
                },
                &g_massRemainderContainerFailures))
        {
            return false;
        }

        const auto& logistics = mass.LogisticsRequestSubsystemState;

        if (!CheckedSparseMap(
                logistics.RequestData,
                [&](const auto& pair)
                {
                    const auto& request = pair.Value();

                    if (!EmitMassRemainder(
                            staged,
                            pair.Key().ID,
                            counts.logistics) ||
                        !EmitMassRemainder(
                            staged,
                            request.UId.ID,
                            counts.logistics) ||
                        !EmitMassRemainder(
                            staged,
                            request.RequesterEntity.ID,
                            counts.logistics))
                    {
                        return false;
                    }

                    return CheckedEach(
                        request.RuntimeData,
                        [&](const auto& runtime)
                        {
                            return EmitMassRemainder(
                                    staged,
                                    runtime.AgentEntity.ID,
                                    counts.logistics) &&
                                EmitMassRemainder(
                                    staged,
                                    runtime.ItemSource.ID,
                                    counts.logistics) &&
                                EmitMassRemainder(
                                    staged,
                                    runtime.ItemDestination.ID,
                                    counts.logistics);
                        },
                        &g_massRemainderContainerFailures);
                },
                &g_massRemainderContainerFailures) ||
            !CheckedSparseMap(
                logistics.StorageToItemsInTransfer,
                [&](const auto& pair)
                {
                    return EmitMassRemainder(
                        staged,
                        pair.Key().ID,
                        counts.logistics);
                },
                &g_massRemainderContainerFailures))
        {
            return false;
        }

        if (!CollectMassPidArray(
                mass.EnviroWaveSubsystemState.EntitiesInWave.Values,
                staged,
                counts.wave) ||
            !CheckedSparseMap(
                mass.BuildingSpawnPointsSaveData.SpawnPointOwnerships,
                [&](const auto& pair)
                {
                    return EmitMassRemainder(
                        staged,
                        pair.Key().ID,
                        counts.spawnOwnership);
                },
                &g_massRemainderContainerFailures) ||
            !CheckedSparseMap(
                mass.FoundableEntitiesSpawnData,
                [&](const auto& pair)
                {
                    return EmitMassRemainder(
                        staged,
                        pair.Key().ID,
                        counts.foundable);
                },
                &g_massRemainderContainerFailures) ||
            !CheckedSparseMap(
                mass.FoundableEntityToPersistentIdMap,
                [&](const auto& pair)
                {
                    return EmitMassRemainder(
                        staged,
                        pair.Value().ID,
                        counts.foundable);
                },
                &g_massRemainderContainerFailures))
        {
            return false;
        }

        return true;
    }

    bool CommitMassRemainderValues(const FCrMassSaveData& mass)
    {
        g_massRemainderAttempts.fetch_add(1, std::memory_order_relaxed);

        std::vector<std::uint32_t> staged;
        MassRemainderDomainCounts counts{};

        try
        {
            if (!CollectMassRemainderValues(mass, staged, counts))
            {
                g_massRemainderFailures.fetch_add(1, std::memory_order_relaxed);
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
                std::lock_guard<std::mutex> lock(g_massRemainderValuesMutex);
                g_massRemainderSourceValues.insert(
                    g_massRemainderSourceValues.end(),
                    staged.begin(),
                    staged.end());
            }

            g_massRemainderValues.fetch_add(
                static_cast<std::uint64_t>(staged.size()),
                std::memory_order_relaxed);
            g_massRemainderZeroValues.fetch_add(
                zeros,
                std::memory_order_relaxed);
            g_massRemainderInvalidSentinels.fetch_add(
                sentinels,
                std::memory_order_relaxed);

            g_stabilityValues.fetch_add(
                counts.stability,
                std::memory_order_relaxed);
            g_electricityValues.fetch_add(
                counts.electricity,
                std::memory_order_relaxed);
            g_logisticsValues.fetch_add(
                counts.logistics,
                std::memory_order_relaxed);
            g_waveValues.fetch_add(
                counts.wave,
                std::memory_order_relaxed);
            g_spawnOwnershipValues.fetch_add(
                counts.spawnOwnership,
                std::memory_order_relaxed);
            g_foundableValues.fetch_add(
                counts.foundable,
                std::memory_order_relaxed);

            g_massRemainderSuccesses.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        catch (...)
        {
            g_massRemainderFailures.fetch_add(1, std::memory_order_relaxed);
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
        g_tagRegistryByPointer.clear();
        g_resolvedTagRegistry.clear();

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

        const auto& tagRegistry = TagRegistry();
        g_resolvedTagRegistry.reserve(tagRegistry.size());
        g_tagRegistryByPointer.reserve(tagRegistry.size());

        for (const TagRegistryEntry& profile : tagRegistry)
        {
            const std::wstring path = NativePathFromProfileName(profile.FullName);
            const std::wstring basePath = NativePathFromProfileName(profile.ImmediateBaseFullName);

            UObject* object =
                path.empty() ? nullptr :
                findSafe(scriptStructClass, nullptr, path.c_str(), true);
            UObject* baseObject =
                basePath.empty() ? nullptr :
                findSafe(scriptStructClass, nullptr, basePath.c_str(), true);

            if (object == nullptr || baseObject == nullptr)
            {
                LOG_WARN(
                    "PersistentIdFix: tag descriptor registry lookup failed for %s",
                    profile.FullName);
                g_resolvedTagRegistry.clear();
                g_tagRegistryByPointer.clear();
                return false;
            }

            const UScriptStruct* descriptor =
                reinterpret_cast<const UScriptStruct*>(object);
            ResolvedTagRegistryEntry resolved{
                &profile,
                descriptor,
                reinterpret_cast<const UStruct*>(baseObject) };

            if (!ValidateTagDescriptor(descriptor, resolved))
            {
                LOG_WARN(
                    "PersistentIdFix: tag descriptor schema mismatch for %s",
                    profile.FullName);
                g_resolvedTagRegistry.clear();
                g_tagRegistryByPointer.clear();
                return false;
            }

            const std::size_t index = g_resolvedTagRegistry.size();
            g_resolvedTagRegistry.push_back(resolved);
            g_tagRegistryByPointer.emplace(descriptor, index);
        }

        g_massSaveDataDescriptor = massDescriptor;
        g_descriptorRegistryReady = true;

        LOG_INFO(
            "PersistentIdFix: Mass descriptor registry ready: profiles=%llu tags=%llu Mass=%p resolver=%p",
            static_cast<unsigned long long>(g_resolvedRegistry.size()),
            static_cast<unsigned long long>(g_resolvedTagRegistry.size()),
            g_massSaveDataDescriptor,
            reinterpret_cast<void*>(resolverAddress));

        if (!PersistentIdFixIndependentSourceCollector::InitializeDescriptorRegistry(
                engineEvents))
        {
            LOG_WARN(
                "PersistentIdFix: independent fixed-source descriptor registry unavailable; reuse coverage will remain incomplete");
        }

        return true;
    }

    void ResetDescriptorRegistry()
    {
        PersistentIdFixIndependentSourceCollector::ResetDescriptorRegistry();

        std::lock_guard<std::mutex> lock(g_registryMutex);
        g_descriptorRegistryReady = false;
        g_massSaveDataDescriptor = nullptr;
        g_registryByPointer.clear();
        g_resolvedRegistry.clear();
        g_tagRegistryByPointer.clear();
        g_resolvedTagRegistry.clear();
    }

    bool IsDescriptorRegistryReady()
    {
        return g_descriptorRegistryReady;
    }

    bool ValidateMassSaveDataEnvelope(
        const UScriptStruct* structType,
        const FCrMassSaveData* massSaveData)
    {
        return
            IsDescriptorRegistryReady() &&
            structType != nullptr &&
            massSaveData != nullptr &&
            structType == g_massSaveDataDescriptor &&
            structType->Size == 0x4F8 &&
            structType->MinAlignment == 8 &&
            IsReadableRange(massSaveData, 0x4F8);
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

        if (!IsReadableRange(massSaveData, 0x4F8))
        {
            g_massDescriptorMismatches.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass destination storage is not readable for the validated schema: destination=%p size=0x4F8",
                massSaveData);
            return;
        }

        if (!CommitEntityIdentities(*massSaveData))
        {
            LOG_WARN(
                "PersistentIdFix: Mass entity identity collection failed");
        }

        if (!CommitMassRemainderValues(*massSaveData))
        {
            LOG_WARN(
                "PersistentIdFix: fixed-layout Mass remainder collection failed");
        }
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        else
        {
            // Each successful source observation gets its own synchronous test.
            // No diagnostic state or values carry across observations/generations.
            PersistentIdFixReflectionDiagnostics::RunStabilityRampMapValueTest(
                structType, *massSaveData);
            PersistentIdFixReflectionDiagnostics::RunSparseMapCensus(structType, *massSaveData);
        }
#endif

        if (!CheckedSparseMap(
                massSaveData->Entities,
                [&](const auto& pair)
                {
                    g_entitySlotsVisited.fetch_add(
                        1,
                        std::memory_order_relaxed);

                    const FCrEntitySaveData& entity = pair.Value();

                    if (!CheckedEach(
                            entity.FragmentValues,
                            [&](const FInstancedStruct& payload)
                            {
                                ObservePayload(payload);
                                return true;
                            },
                            &g_semanticContainerFailures))
                    {
                        LOG_WARN(
                            "PersistentIdFix: Mass fragment container validation failed for entity");
                        return false;
                    }

                    if (!CheckedEach(
                            entity.Tags,
                            [&](const FInstancedStruct& tag)
                            {
                                return ObserveTag(tag);
                            },
                            &g_tagContainerFailures))
                    {
                        LOG_WARN(
                            "PersistentIdFix: Mass tag collection incomplete for entity");
                        return false;
                    }

                    return true;
                },
                nullptr))
        {
            g_entityCountMismatches.fetch_add(
                1,
                std::memory_order_relaxed);
            LOG_WARN(
                "PersistentIdFix: Mass entity traversal failed validation");
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

        snapshot.tagClassified = g_tagClassified.load(std::memory_order_relaxed);
        snapshot.tagEmpty = g_tagEmpty.load(std::memory_order_relaxed);
        snapshot.tagMalformed = g_tagMalformed.load(std::memory_order_relaxed);
        snapshot.tagUnsupported = g_tagUnsupported.load(std::memory_order_relaxed);
        snapshot.tagSchemaMismatches = g_tagSchemaMismatches.load(std::memory_order_relaxed);
        snapshot.tagContainerFailures = g_tagContainerFailures.load(std::memory_order_relaxed);

        snapshot.identityAttempts = g_identityAttempts.load(std::memory_order_relaxed);
        snapshot.identitySuccesses = g_identitySuccesses.load(std::memory_order_relaxed);
        snapshot.identityFailures = g_identityFailures.load(std::memory_order_relaxed);
        snapshot.identityValues = g_identityValues.load(std::memory_order_relaxed);
        snapshot.identityZeroValues = g_identityZeroValues.load(std::memory_order_relaxed);
        snapshot.identityInvalidSentinels = g_identityInvalidSentinels.load(std::memory_order_relaxed);
        snapshot.identityContainerFailures = g_identityContainerFailures.load(std::memory_order_relaxed);

        snapshot.massRemainderAttempts = g_massRemainderAttempts.load(std::memory_order_relaxed);
        snapshot.massRemainderSuccesses = g_massRemainderSuccesses.load(std::memory_order_relaxed);
        snapshot.massRemainderFailures = g_massRemainderFailures.load(std::memory_order_relaxed);
        snapshot.massRemainderValues = g_massRemainderValues.load(std::memory_order_relaxed);
        snapshot.massRemainderZeroValues = g_massRemainderZeroValues.load(std::memory_order_relaxed);
        snapshot.massRemainderInvalidSentinels = g_massRemainderInvalidSentinels.load(std::memory_order_relaxed);
        snapshot.massRemainderContainerFailures = g_massRemainderContainerFailures.load(std::memory_order_relaxed);

        snapshot.stabilityValues = g_stabilityValues.load(std::memory_order_relaxed);
        snapshot.electricityValues = g_electricityValues.load(std::memory_order_relaxed);
        snapshot.logisticsValues = g_logisticsValues.load(std::memory_order_relaxed);
        snapshot.waveValues = g_waveValues.load(std::memory_order_relaxed);
        snapshot.spawnOwnershipValues = g_spawnOwnershipValues.load(std::memory_order_relaxed);
        snapshot.foundableValues = g_foundableValues.load(std::memory_order_relaxed);
        return snapshot;
    }

    bool AppendCollectedValues(
        std::vector<std::uint32_t>& destination)
    {
        try
        {
            {
                std::lock_guard<std::mutex> lock(g_semanticValuesMutex);
                destination.insert(
                    destination.end(),
                    g_semanticValues.begin(),
                    g_semanticValues.end());
            }

            {
                std::lock_guard<std::mutex> lock(g_identityValuesMutex);
                destination.insert(
                    destination.end(),
                    g_identitySourceValues.begin(),
                    g_identitySourceValues.end());
            }

            {
                std::lock_guard<std::mutex> lock(g_massRemainderValuesMutex);
                destination.insert(
                    destination.end(),
                    g_massRemainderSourceValues.begin(),
                    g_massRemainderSourceValues.end());
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

        LOG_INFO(
            "PersistentIdFix: Mass tags [%s]: payloads=%llu classified=%llu empty=%llu malformed=%llu unsupported=%llu schemaMismatch=%llu containerFailures=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.tagPayloads),
            static_cast<unsigned long long>(snapshot.tagClassified),
            static_cast<unsigned long long>(snapshot.tagEmpty),
            static_cast<unsigned long long>(snapshot.tagMalformed),
            static_cast<unsigned long long>(snapshot.tagUnsupported),
            static_cast<unsigned long long>(snapshot.tagSchemaMismatches),
            static_cast<unsigned long long>(snapshot.tagContainerFailures));

        LOG_INFO(
            "PersistentIdFix: Mass entity identities [%s]: attempts=%llu success=%llu failed=%llu values=%llu zero=%llu invalidSentinel=%llu containerFailures=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.identityAttempts),
            static_cast<unsigned long long>(snapshot.identitySuccesses),
            static_cast<unsigned long long>(snapshot.identityFailures),
            static_cast<unsigned long long>(snapshot.identityValues),
            static_cast<unsigned long long>(snapshot.identityZeroValues),
            static_cast<unsigned long long>(snapshot.identityInvalidSentinels),
            static_cast<unsigned long long>(snapshot.identityContainerFailures));

        LOG_INFO(
            "PersistentIdFix: Mass fixed remainder PIDs [%s]: attempts=%llu success=%llu failed=%llu values=%llu zero=%llu invalidSentinel=%llu containerFailures=%llu stability=%llu electricity=%llu logistics=%llu wave=%llu spawn=%llu foundable=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.massRemainderAttempts),
            static_cast<unsigned long long>(snapshot.massRemainderSuccesses),
            static_cast<unsigned long long>(snapshot.massRemainderFailures),
            static_cast<unsigned long long>(snapshot.massRemainderValues),
            static_cast<unsigned long long>(snapshot.massRemainderZeroValues),
            static_cast<unsigned long long>(snapshot.massRemainderInvalidSentinels),
            static_cast<unsigned long long>(snapshot.massRemainderContainerFailures),
            static_cast<unsigned long long>(snapshot.stabilityValues),
            static_cast<unsigned long long>(snapshot.electricityValues),
            static_cast<unsigned long long>(snapshot.logisticsValues),
            static_cast<unsigned long long>(snapshot.waveValues),
            static_cast<unsigned long long>(snapshot.spawnOwnershipValues),
            static_cast<unsigned long long>(snapshot.foundableValues));
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

        g_tagClassified.store(0, std::memory_order_relaxed);
        g_tagEmpty.store(0, std::memory_order_relaxed);
        g_tagMalformed.store(0, std::memory_order_relaxed);
        g_tagUnsupported.store(0, std::memory_order_relaxed);
        g_tagSchemaMismatches.store(0, std::memory_order_relaxed);
        g_tagContainerFailures.store(0, std::memory_order_relaxed);

        g_identityAttempts.store(0, std::memory_order_relaxed);
        g_identitySuccesses.store(0, std::memory_order_relaxed);
        g_identityFailures.store(0, std::memory_order_relaxed);
        g_identityValues.store(0, std::memory_order_relaxed);
        g_identityZeroValues.store(0, std::memory_order_relaxed);
        g_identityInvalidSentinels.store(0, std::memory_order_relaxed);
        g_identityContainerFailures.store(0, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(g_semanticValuesMutex);
            g_semanticValues.clear();
        }

        {
            std::lock_guard<std::mutex> lock(g_identityValuesMutex);
            g_identitySourceValues.clear();
        }

        g_massRemainderAttempts.store(0, std::memory_order_relaxed);
        g_massRemainderSuccesses.store(0, std::memory_order_relaxed);
        g_massRemainderFailures.store(0, std::memory_order_relaxed);
        g_massRemainderValues.store(0, std::memory_order_relaxed);
        g_massRemainderZeroValues.store(0, std::memory_order_relaxed);
        g_massRemainderInvalidSentinels.store(0, std::memory_order_relaxed);
        g_massRemainderContainerFailures.store(0, std::memory_order_relaxed);

        g_stabilityValues.store(0, std::memory_order_relaxed);
        g_electricityValues.store(0, std::memory_order_relaxed);
        g_logisticsValues.store(0, std::memory_order_relaxed);
        g_waveValues.store(0, std::memory_order_relaxed);
        g_spawnOwnershipValues.store(0, std::memory_order_relaxed);
        g_foundableValues.store(0, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(g_massRemainderValuesMutex);
            g_massRemainderSourceValues.clear();
        }
    }
}
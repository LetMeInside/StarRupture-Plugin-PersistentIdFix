#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "IndependentSourceCollector.h"
#include "ReflectionDiagnostics.h"
#include "SourceMemory.h"

#include "plugin.h"
#include "plugin_helpers.h"

#include "SDK/Chimera_structs.hpp"
#include "SDK/CoreUObject_classes.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#ifndef PERSISTENTIDFIX_REFLECTION_TEST_MODE
#define PERSISTENTIDFIX_REFLECTION_TEST_MODE 0
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

    struct MulticardsNames
    {
        FName Field;
        FName ArrayClass;
        FName IntClass;
    };

    // Production-owned values only: never retain optional asset/function pointers.
    StaticFindObjectSafeByNameFn g_multicardsResolver = nullptr;
    MulticardsNames g_multicardsNames{};
    bool g_multicardsNamesAttempted = false;
    bool g_multicardsNamesReady = false;
    std::uint64_t g_registryEpoch = 0;

    struct NameConversionParams
    {
        FString InString;
        FName ReturnValue;
    };

    static_assert(sizeof(FName) == 0x08);
    static_assert(offsetof(FName, ComparisonIndex) == 0);
    static_assert(offsetof(FName, Number) == 4);
    static_assert(offsetof(FField, Name) == 0x20);
    static_assert(sizeof(NameConversionParams) == 0x18);
    static_assert(alignof(NameConversionParams) == 8);
    static_assert(offsetof(NameConversionParams, ReturnValue) == 0x10);
    static_assert(sizeof(TArray<std::int32_t>) == 0x10);
    static_assert(alignof(TArray<std::int32_t>) == 8);


    std::atomic<std::uint64_t> g_gameStatePlayers{0};
    std::atomic<std::uint64_t> g_gameStateFloorValues{0};
    std::atomic<std::uint64_t> g_gameStateAntennaFogValues{0};
    std::atomic<std::uint64_t> g_gameStateDevicePayloads{0};
    std::atomic<std::uint64_t> g_gameStateDeviceEmpty{0};
    std::atomic<std::uint64_t> g_gameStateDeviceMalformed{0};
    std::atomic<std::uint64_t> g_gameStateDeviceUnknownPresent{0};
    std::atomic<std::uint64_t> g_gameStateDeviceRecognizedPidFree{0};
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

    using PersistentIdFixSourceMemory::IsReadableRange;

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

    bool ValidateMulticardsIndexes(const TArray<std::int32_t>& indexes)
    {
        // An explicit compatibility bound; exceeding it leaves coverage unknown.
        constexpr std::int32_t maxKeycardIndexes = 65536;
        if (indexes.Num() < 0 || indexes.Max() < 0 || indexes.Num() > indexes.Max() ||
            indexes.Max() > maxKeycardIndexes ||
            (indexes.GetDataPtr() != nullptr &&
                reinterpret_cast<std::uintptr_t>(indexes.GetDataPtr()) % alignof(std::int32_t) != 0))
            return false;
        return ValidateArray(indexes);
    }

    bool SameName(const FName& actual, const FName& expected)
    {
        return actual.ComparisonIndex == expected.ComparisonIndex &&
            actual.Number == expected.Number;
    }

    bool InitializeMulticardsNames(StaticFindObjectSafeByNameFn findSafe,
        IPluginObjectWalker* walker, MulticardsNames& names)
    {
        // The public resolver selects class/function by name. Independently
        // verify the resolved function's owner and the non-null CDO target.
        UObject* classObject = findSafe(nullptr, nullptr, L"/Script/CoreUObject.Class", true);
        if (!IsReadableRange(classObject, sizeof(UClass)))
            return false;
        auto* classClass = reinterpret_cast<UClass*>(classObject);
        auto* libraryClass = reinterpret_cast<UClass*>(findSafe(classClass, nullptr,
            L"/Script/Engine.KismetStringLibrary", true));
        auto* functionClass = reinterpret_cast<UClass*>(findSafe(classClass, nullptr,
            L"/Script/CoreUObject.Function", true));
        if (!IsReadableRange(libraryClass, sizeof(UClass)) ||
            !IsReadableRange(functionClass, sizeof(UClass)) ||
            libraryClass->Class != classClass || functionClass->Class != classClass)
            return false;

        UObject* target = findSafe(libraryClass, nullptr,
            L"/Script/Engine.Default__KismetStringLibrary", true);
        auto* function = static_cast<UFunction*>(walker->ResolveUFunction(
            "KismetStringLibrary", "Conv_StringToName"));
        if (!IsReadableRange(target, sizeof(UObject)) || target->Class != libraryClass ||
            !IsReadableRange(function, sizeof(UFunction)) ||
            function->Class != functionClass || function->Outer != libraryClass ||
            function->Size != sizeof(NameConversionParams) ||
            (function->FunctionFlags & 0x2400u) != 0x2400u ||
            function->ExecFunction == nullptr)
            return false;

        // Verify the reflected parameter layout before supplying borrowed input.
        const FField* parameter = function->ChildProperties;
        bool inputSeen = false;
        bool outputSeen = false;
        for (std::size_t i = 0; i < 2; ++i)
        {
            const auto* property = reinterpret_cast<const FProperty*>(parameter);
            if (!IsReadableRange(property, sizeof(FProperty)) ||
                !IsReadableRange(property->ClassPrivate, sizeof(FFieldClass)) ||
                property->ArrayDim != 1)
                return false;
            const auto flags = static_cast<EClassCastFlags>(property->ClassPrivate->CastFlags);
            if (property->Offset == 0 && property->ElementSize == 0x10 &&
                (flags & EClassCastFlags::StrProperty) && !inputSeen &&
                (property->PropertyFlags & 0x480u) == 0x80u)
                inputSeen = true;
            else if (property->Offset == 0x10 && property->ElementSize == 8 &&
                (flags & EClassCastFlags::NameProperty) && !outputSeen &&
                (property->PropertyFlags & 0x480u) == 0x480u)
                outputSeen = true;
            else
                return false;
            parameter = property->Next;
        }
        if (parameter != nullptr || !inputSeen || !outputSeen)
            return false;

        const auto convert = [&](const wchar_t* literal, FName& name)
        {
            // SDK FString(const wchar_t*) is a non-owning header. The literal
            // outlives ProcessEvent; the audited thunk owns/frees its input copy.
            NameConversionParams params{FString(literal), FName{}};
            if (!walker->InvokeResolvedUFunction(target, function, &params) ||
                params.ReturnValue.IsNone() || params.ReturnValue.Number != 0)
                return false;
            name = params.ReturnValue;
            return true;
        };

        return convert(L"EnteredKeycardIndexes_4_BC9CABA9934C80F62FC5349C733585CA", names.Field) &&
            convert(L"ArrayProperty", names.ArrayClass) &&
            convert(L"IntProperty", names.IntClass);
    }

    bool GetMulticardsNames(StaticFindObjectSafeByNameFn& findSafe,
        MulticardsNames& names)
    {
        IPluginObjectWalker* walker = g_self != nullptr && g_self->hooks != nullptr
            ? g_self->hooks->ObjectWalker : nullptr;
        if (walker == nullptr || walker->IsReady == nullptr || !walker->IsReady() ||
            walker->ResolveUFunction == nullptr || walker->InvokeResolvedUFunction == nullptr)
            return false;

        std::uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lock(g_registryMutex);
            if (!g_registryReady || g_multicardsResolver == nullptr)
                return false;
            findSafe = g_multicardsResolver;
            if (g_multicardsNamesReady)
            {
                names = g_multicardsNames;
                return true;
            }
            if (g_multicardsNamesAttempted)
                return false;
            g_multicardsNamesAttempted = true;
            epoch = g_registryEpoch;
        }

        // Never hold the registry lock across ProcessEvent. Concurrent or
        // reentrant observations fail closed while this one attempt is pending.
        MulticardsNames resolved{};
        const bool ready = InitializeMulticardsNames(findSafe, walker, resolved);
        std::lock_guard<std::mutex> lock(g_registryMutex);
        if (epoch != g_registryEpoch || !g_registryReady)
            return false;
        g_multicardsNamesReady = ready;
        if (ready)
        {
            g_multicardsNames = resolved;
            names = resolved;
        }
        LOG_INFO("PersistentIdFix: Multicards PID-free schema names: ready=%u", ready ? 1u : 0u);
        return ready;
    }

    bool ExactPropertyClass(const FProperty* property, const FName& name,
        EClassCastFlags requiredFlag)
    {
        if (!IsReadableRange(property, sizeof(FProperty)) ||
            !IsReadableRange(property->ClassPrivate, sizeof(FFieldClass)))
            return false;
        FName actual{};
        std::memcpy(&actual, &property->ClassPrivate->Name, sizeof(actual));
        return SameName(actual, name) &&
            (static_cast<EClassCastFlags>(property->ClassPrivate->CastFlags) & requiredFlag);
    }

    bool IsVerifiedMulticardsPayload(const FInstancedStruct& payload)
    {
        StaticFindObjectSafeByNameFn findSafe = nullptr;
        MulticardsNames names{};
        if (!GetMulticardsNames(findSafe, names))
            return false;

        auto* expectedClass = reinterpret_cast<UClass*>(findSafe(nullptr, nullptr,
            L"/Script/CoreUObject.UserDefinedStruct", true));
        if (!IsReadableRange(expectedClass, sizeof(UClass)))
            return false;
        auto* expected = reinterpret_cast<const UUserDefinedStruct*>(findSafe(
            expectedClass, nullptr,
            L"/Game/Chimera/Environment/Megamachines/MulticardsTerminalSaveData.MulticardsTerminalSaveData",
            true));
        if (expected == nullptr || payload.ScriptStruct != expected ||
            !IsReadableRange(expected, sizeof(UUserDefinedStruct)) ||
            expected->Class != expectedClass ||
            expected->Status != EUserDefinedStructureStatus::UDSS_UpToDate ||
            expected->Size != 0x10 || expected->MinAlignment != 8 ||
            expected->SuperStruct != nullptr || expected->Children != nullptr ||
            !IsReadableRange(payload.StructMemory, 0x10) ||
            reinterpret_cast<std::uintptr_t>(payload.StructMemory) % 8 != 0)
            return false;

        // Exactly one top-level field: no unbounded chain walk or skipped fields.
        const auto* property = reinterpret_cast<const FProperty*>(expected->ChildProperties);
        if (!ExactPropertyClass(property, names.ArrayClass, EClassCastFlags::ArrayProperty) ||
            !IsReadableRange(property, sizeof(FArrayProperty)) || property->Next != nullptr ||
            property->Offset != 0 || property->ArrayDim != 1 || property->ElementSize != 0x10)
            return false;
        FName actualName{};
        std::memcpy(&actualName, &property->Name, sizeof(actualName));
        if (!SameName(actualName, names.Field))
            return false;
        const auto* inner = static_cast<const FArrayProperty*>(property)->InnerProperty;
        if (!ExactPropertyClass(inner, names.IntClass, EClassCastFlags::IntProperty) ||
            inner->Offset != 0 || inner->ArrayDim != 1 || inner->ElementSize != 4 ||
            inner->Next != nullptr)
            return false;

        TArray<std::int32_t> indexes{};
        std::memcpy(&indexes, payload.StructMemory, sizeof(indexes));
        // This exact, audited Blueprint schema contains keycard indices, not
        // Mass PIDs. Runtime metadata cannot recover text fields discarded by
        // the native importer; this is not a generic serialization certificate.
        return ValidateMulticardsIndexes(indexes);
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
        g_multicardsResolver = nullptr;
        g_multicardsNames = {};
        g_multicardsNamesAttempted = false;
        g_multicardsNamesReady = false;
        ++g_registryEpoch;

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        PersistentIdFixReflectionDiagnostics::ResetDescriptorRegistry();
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
        PersistentIdFixReflectionDiagnostics::InitializeDescriptorRegistry(findSafe, scriptStructClass);
#endif

        g_multicardsResolver = findSafe;
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
        g_multicardsResolver = nullptr;
        g_multicardsNames = {};
        g_multicardsNamesAttempted = false;
        g_multicardsNamesReady = false;
        ++g_registryEpoch;

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        PersistentIdFixReflectionDiagnostics::ResetDescriptorRegistry();
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
        const bool success = Observe(
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
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        // Observational only: production collection has already succeeded.
        if (success)
            PersistentIdFixReflectionDiagnostics::RunAntennaMapValueTest(structType, *data);
#endif
        return success;
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
        PersistentIdFixReflectionDiagnostics::RunForcedReflectionShapeTests();
#endif

        std::vector<std::uint32_t> staged;
        std::uint64_t players = 0;
        std::uint64_t floorValues = 0;
        std::uint64_t antennaFogValues = 0;
        std::uint64_t devicePayloads = 0;
        std::uint64_t deviceEmpty = 0;
        std::uint64_t deviceMalformed = 0;
        std::uint64_t deviceUnknownPresent = 0;
        std::uint64_t deviceRecognizedPidFree = 0;
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
                        PersistentIdFixReflectionDiagnostics::RunForcedReflectionValueTests(player);
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

                        if (!IsReadableRange(payload.ScriptStruct, sizeof(UScriptStruct)))
                        {
                            ++deviceMalformed;
                            return false;
                        }

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
                        try
                        {
                            PersistentIdFixReflectionDiagnostics::ShadowSchemaSummary shadow{};

                            const bool shadowSupported =
                                PersistentIdFixReflectionDiagnostics::AnalyzeShadowPayload(
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

                            PersistentIdFixReflectionDiagnostics::LogPayloadShape(
                                payload, shadowSupported, shadow);
                        }
                        catch (...)
                        {
                            // Optional diagnostics cannot fail production collection.
                            g_gameStateDeviceShadowUnsupported.fetch_add(1, std::memory_order_relaxed);
                        }
#endif

                        // Shadow results never determine production acceptance.
                        if (IsVerifiedMulticardsPayload(payload))
                            ++deviceRecognizedPidFree;
                        else
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
            g_gameStateDeviceRecognizedPidFree.fetch_add(
                deviceRecognizedPidFree,
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
        result.gameStateDeviceRecognizedPidFree =
            g_gameStateDeviceRecognizedPidFree.load(std::memory_order_relaxed);
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
            "PersistentIdFix: GameState source details [%s]: players=%llu floor=%llu antennaFog=%llu devicePayloads=%llu deviceEmpty=%llu deviceMalformed=%llu deviceUnknown=%llu deviceRecognizedPidFree=%llu opaqueStoreEntries=%llu discoveredBuildings=%llu",
            phase != nullptr ? phase : "<null>",
            static_cast<unsigned long long>(snapshot.gameStatePlayers),
            static_cast<unsigned long long>(snapshot.gameStateFloorValues),
            static_cast<unsigned long long>(snapshot.gameStateAntennaFogValues),
            static_cast<unsigned long long>(snapshot.gameStateDevicePayloads),
            static_cast<unsigned long long>(snapshot.gameStateDeviceEmpty),
            static_cast<unsigned long long>(snapshot.gameStateDeviceMalformed),
            static_cast<unsigned long long>(snapshot.gameStateDeviceUnknownPresent),
            static_cast<unsigned long long>(snapshot.gameStateDeviceRecognizedPidFree),
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
        g_gameStateDeviceRecognizedPidFree.store(0, std::memory_order_relaxed);
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

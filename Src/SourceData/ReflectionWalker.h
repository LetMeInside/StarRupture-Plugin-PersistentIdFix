#pragma once

#include "SDK/CoreUObject_classes.hpp"
#include "SDK/CoreUObject_structs.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

#ifndef PERSISTENTIDFIX_REFLECTION_TEST_MODE
#define PERSISTENTIDFIX_REFLECTION_TEST_MODE 0
#endif

namespace PersistentIdFixReflectionWalker
{
    using namespace SDK;

    inline constexpr std::uint32_t kShadowMaxStructDepth = 16;
    inline constexpr std::uint64_t kShadowMaxProperties = 512;
    inline constexpr std::uint64_t kShadowMaxArrayElements = 1000000;
    inline constexpr std::uint64_t kShadowMaxValues = 2000000;

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
    struct ShadowScriptArray
    {
        const std::uint8_t* Data;
        std::int32_t Num;
        std::int32_t Max;
    };

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
    // Native Client/Server ABI; the generated FMapProperty stops at 0x80.
    inline constexpr std::size_t kShadowNativeMapDescriptorSize = 0xA0;
    inline constexpr std::size_t kShadowNativeMapLayoutOffset = 0x80;
    inline constexpr std::size_t kShadowNativeMapFlagsOffset = 0x98;
    // Work bound, not a claim about the engine's maximum size.
    inline constexpr std::int32_t kShadowMaxMapSlotStride = 1024 * 1024;
    struct ShadowNativeMapLayout
    {
        std::int32_t ValueOffset = -1;
        std::int32_t HashNextIdOffset = -1;
        std::int32_t HashIndexOffset = -1;
        std::int32_t SetSize = -1;
        std::int32_t SparseAlignment = -1;
        std::int32_t SparseSlotStride = -1;
    };

    struct ShadowNativeMapMetadata
    {
        ShadowNativeMapLayout Layout;
        std::uint8_t AllocatorFlags = 0;
        bool DescriptorReadable = false;
        bool FlagsKnown = false;
        bool AllocatorSupported = false;
        bool LayoutValid = false;
    };

    struct ShadowHeapMapHeader
    {
        std::uintptr_t SparseData = 0;
        std::int32_t ArrayNum = 0;
        std::int32_t ArrayMax = 0;
        std::array<std::uint32_t, 4> InlineBitmap{};
        std::uintptr_t SecondaryBitmap = 0;
        std::int32_t NumBits = 0;
        std::int32_t MaxBits = 0;
        std::int32_t FirstFreeIndex = -1;
        std::int32_t NumFreeIndices = 0;
        std::uint32_t InlineHash = 0;
        std::uint32_t HashPadding = 0;
        std::uintptr_t SecondaryHash = 0;
        std::int32_t HashSize = 0;
        std::uint32_t TailPadding = 0;
    };

    struct ShadowHeapMapValidation
    {
        ShadowHeapMapHeader Header;
        std::int32_t LiveEntries = -1;
        std::uint64_t OccupiedBits = 0;
        const char* BitmapMode = "unavailable";
        const char* FailureReason = "not-validated";
        bool HeaderReadable = false;
        bool HeaderValid = false;
        bool BitmapValid = false;
        bool Valid = false;
    };

    struct ShadowMapSlotBudget
    {
        std::uint64_t RemainingSlots = 1000000;
        std::uint64_t RemainingEntries = 1000000;
        std::uint64_t RemainingBitmapWords = 1000000;
    };

    struct ShadowMapValueExtent
    {
        std::size_t Bytes = 0;
        std::size_t Alignment = 0;
    };

    struct ShadowMapSlotValidation
    {
        ShadowMapValueExtent Key;
        ShadowMapValueExtent Value;
        std::uint64_t ScannedSlots = 0;
        std::uint64_t OccupiedSlots = 0;
        std::uint64_t ValidatedEntries = 0;
        std::int32_t FailedSlot = -1;
        const char* FailureReason = "not-validated";
        bool ExtentsValid = false;
        bool AlignmentValid = false;
        bool AllValidated = false;
    };

    // Structural snapshots only; no recursive visitors or PID observations.
    struct ShadowMapInspection
    {
        ShadowNativeMapMetadata Metadata;
        ShadowHeapMapValidation Heap;
        ShadowMapSlotValidation Slots;
        const char* FailureReason = "not-inspected";
        bool StructuralValid = false;
    };

    ShadowMapInspection InspectShadowMap(const FProperty* property,
        const void* storage, ShadowMapSlotBudget& preflightBudget);

    // Caller-owned diagnostic storage, valid only during synchronous traversal.
    // Every exact occurrence is counted, including zero and the invalid sentinel.
    // Overflow affects observation completeness only, never traversal policy.
    struct ShadowExactPidSink
    {
        std::uint32_t* Values = nullptr;
        std::size_t Capacity = 0;
        std::size_t Count = 0;
        std::uint64_t Observed = 0;
        bool Overflow = false;

        void Observe(std::uint32_t value) noexcept
        {
            ++Observed;
            if (Values == nullptr || Count >= Capacity)
            {
                Overflow = true;
                return;
            }
            Values[Count++] = value;
        }
    };

    // Optional synchronous, observational sinks. No traversal budgets are exposed.
    struct ShadowDiagnosticSink
    {
        void (*Warn)(const char* message) = nullptr;
        void (*ParentFailure)(const UStruct*, const FProperty*, std::uint32_t) = nullptr;
        void (*MapValidated)(const FProperty*, const ShadowNativeMapMetadata&,
            const ShadowHeapMapValidation&, const ShadowMapSlotValidation&,
            const UScriptStruct*, std::uint32_t) = nullptr;
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
    static_assert(sizeof(ShadowHeapMapHeader) == 0x50);
    static_assert(offsetof(ShadowHeapMapHeader, SparseData) == 0x00);
    static_assert(offsetof(ShadowHeapMapHeader, ArrayNum) == 0x08);
    static_assert(offsetof(ShadowHeapMapHeader, ArrayMax) == 0x0C);
    static_assert(offsetof(ShadowHeapMapHeader, InlineBitmap) == 0x10);
    static_assert(offsetof(ShadowHeapMapHeader, SecondaryBitmap) == 0x20);
    static_assert(offsetof(ShadowHeapMapHeader, NumBits) == 0x28);
    static_assert(offsetof(ShadowHeapMapHeader, MaxBits) == 0x2C);
    static_assert(offsetof(ShadowHeapMapHeader, FirstFreeIndex) == 0x30);
    static_assert(offsetof(ShadowHeapMapHeader, NumFreeIndices) == 0x34);
    static_assert(offsetof(ShadowHeapMapHeader, InlineHash) == 0x38);
    static_assert(offsetof(ShadowHeapMapHeader, SecondaryHash) == 0x40);
    static_assert(offsetof(ShadowHeapMapHeader, HashSize) == 0x48);
#endif

    struct ShadowValueSummary
    {
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        // Distinct cumulative resource limits: complete structural preflight
        // and actual recursive traversal each charge a slot/entry once.
        const ShadowDiagnosticSink* Diagnostics = nullptr;
        ShadowExactPidSink* ExactPidSink = nullptr;
        ShadowMapSlotBudget MapPrevalidationBudget;
        ShadowMapSlotBudget MapSlotBudget;
        // Optional caller-supplied opaque-boundary policy; never a prerequisite
        // for unrelated structs/maps. Missing policy cannot certify serialization.
        const UStruct* OpaqueStructDescriptor = nullptr;
        std::uint64_t RemainingWork = 2000000;
        std::uint64_t RemainingMapLogs = 32;
        std::uint64_t RemainingFailureLogs = 64;
        std::uint64_t budgetFailures = 0;
        std::uint64_t depthFailures = 0;
        std::uint64_t opaqueBoundaries = 0;
        std::uint32_t TypedMapDepth = 0;
        const char* failureReason = "none";
#endif
        bool complete = true;
        std::uint64_t exactPersistentIds = 0;
        std::uint64_t integralValues = 0;
        // Historical shadow-only numeric observations, not confirmed PIDs.
        // Only exactPersistentIds denotes typed discoveries; neither is published.
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

    static_assert(sizeof(ShadowScriptArray) == 0x10);

    bool HasCastFlag(
        const FField* field,
        EClassCastFlags flag);

    bool IsSupportedIntegralProperty(
        const FProperty* property);

    bool AnalyzeShadowPropertyShape(
        const FProperty* property,
        std::uint32_t depth,
        ShadowSchemaSummary& summary);

    bool AnalyzeShadowStructShape(
        const UStruct* descriptor,
        std::uint32_t depth,
        ShadowSchemaSummary& summary);

    bool AnalyzeShadowPayload(
        const FInstancedStruct& payload,
        ShadowSchemaSummary& summary);

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
        ShadowValueSummary& summary);

}

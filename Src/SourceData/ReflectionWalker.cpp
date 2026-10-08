#include "ReflectionWalker.h"
#include "SourceMemory.h"
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

namespace PersistentIdFixReflectionWalker
{
    using PersistentIdFixSourceMemory::IsReadableRange;

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
    // The caller owns presentation. Logging cannot mutate traversal budgets.
    void ShadowLog(const ShadowValueSummary& summary, const char* format, ...)
    {
        if (summary.Diagnostics == nullptr || summary.Diagnostics->Warn == nullptr)
            return;
        char message[2048]{};
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        summary.Diagnostics->Warn(message);
    }
#endif

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
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

    ShadowHeapMapValidation ValidateShadowHeapMap(
        const ShadowNativeMapMetadata& metadata,
        const void* mapStorage,
        std::int32_t storageSize,
        std::uint64_t* remainingBitmapWords = nullptr)
    {
        // Diagnostic limits, not native container limits. No payload reads.
        constexpr std::int32_t maxSlots = 1000000;
        constexpr std::size_t maxReadableExtent = 256u * 1024u * 1024u;
        ShadowHeapMapValidation result{};
        const auto fail = [&](const char* reason)
        {
            result.FailureReason = reason;
            return result;
        };

        if (!metadata.DescriptorReadable)
            return fail("descriptor-unreadable");
        if (!metadata.FlagsKnown)
            return fail("unknown-allocator-flags");
        if (!metadata.AllocatorSupported)
            return fail("unsupported-memory-image-allocator");
        if (!metadata.LayoutValid)
            return fail("invalid-native-layout");
        if (storageSize != static_cast<std::int32_t>(sizeof(result.Header)))
            return fail("unexpected-map-storage-size");

        result.HeaderReadable = IsReadableRange(mapStorage, sizeof(result.Header));
        if (!result.HeaderReadable)
            return fail("header-unreadable");
        if (reinterpret_cast<std::uintptr_t>(mapStorage) % alignof(ShadowHeapMapHeader) != 0)
            return fail("header-misaligned");
        std::memcpy(&result.Header, mapStorage, sizeof(result.Header));
        const auto& header = result.Header;

        if (header.ArrayNum < 0 || header.ArrayMax < 0 ||
            header.NumFreeIndices < 0 || header.NumBits < 0 ||
            header.MaxBits < 0 || header.HashSize < 0)
            return fail("negative-count");
        if (header.ArrayNum > header.ArrayMax ||
            header.NumFreeIndices > header.ArrayNum ||
            header.NumBits != header.ArrayNum || header.NumBits > header.MaxBits)
            return fail("inconsistent-counts");
        if (header.ArrayMax > maxSlots || header.MaxBits > maxSlots ||
            header.HashSize > maxSlots)
            return fail("diagnostic-count-limit");

        result.LiveEntries = header.ArrayNum - header.NumFreeIndices;
        if ((header.NumFreeIndices == 0 && header.FirstFreeIndex != -1) ||
            (header.NumFreeIndices != 0 &&
                (header.FirstFreeIndex < 0 || header.FirstFreeIndex >= header.ArrayNum)))
            return fail("invalid-free-list-head");

        const auto readableAllocation = [](
            std::uintptr_t address, std::size_t count,
            std::size_t stride, std::size_t alignment)
        {
            if (address != 0 && address % alignment != 0)
                return false;
            if (count == 0)
                return true;
            if (address == 0 ||
                count > (std::numeric_limits<std::size_t>::max)() / stride)
                return false;
            const auto bytes = count * stride;
            return bytes <= maxReadableExtent &&
                IsReadableRange(reinterpret_cast<const void*>(address), bytes);
        };

        if (!readableAllocation(
                header.SparseData, static_cast<std::size_t>(header.ArrayMax),
                static_cast<std::size_t>(metadata.Layout.SparseSlotStride),
                static_cast<std::size_t>(metadata.Layout.SparseAlignment)))
            return fail("sparse-allocation-unreadable-or-out-of-bounds");

        result.BitmapMode = header.SecondaryBitmap != 0 ? "secondary" : "inline";
        // Widen before adding 31, and validate the capacity without reading it.
        const auto bitmapWords =
            (static_cast<std::size_t>(header.NumBits) + 31u) / 32u;
        const auto bitmapCapacityWords =
            (static_cast<std::size_t>(header.MaxBits) + 31u) / 32u;
        if (header.SecondaryBitmap == 0 && bitmapCapacityWords > header.InlineBitmap.size())
            return fail("inline-bitmap-capacity-exceeded");
        if (header.SecondaryBitmap != 0 &&
            !readableAllocation(
                header.SecondaryBitmap, bitmapCapacityWords,
                sizeof(std::uint32_t), alignof(std::uint32_t)))
            return fail("secondary-bitmap-unreadable-or-out-of-bounds");

        if (result.LiveEntries != 0 && header.HashSize == 0)
            return fail("live-map-without-hash-storage");
        if (header.SecondaryHash == 0 && header.HashSize > 1)
            return fail("inline-hash-capacity-exceeded");
        if (header.SecondaryHash != 0 &&
            !readableAllocation(
                header.SecondaryHash, static_cast<std::size_t>(header.HashSize),
                sizeof(std::int32_t), alignof(std::int32_t)))
            return fail("secondary-hash-unreadable-or-out-of-bounds");
        result.HeaderValid = true;

        if (remainingBitmapWords != nullptr)
        {
            if (bitmapWords > *remainingBitmapWords)
                return fail("bitmap-word-budget-exhausted");
            *remainingBitmapWords -= bitmapWords;
        }
        bool freeHeadOccupied = false;
        for (std::size_t wordIndex = 0; wordIndex < bitmapWords; ++wordIndex)
        {
            std::uint32_t word = 0;
            if (header.SecondaryBitmap == 0)
                word = header.InlineBitmap[wordIndex];
            else
            {
                // The checked readable extent covers this bounded word offset.
                const auto address = header.SecondaryBitmap +
                    wordIndex * sizeof(std::uint32_t);
                std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
            }
            const auto remaining = static_cast<std::size_t>(header.NumBits) - wordIndex * 32u;
            if (remaining < 32u)
                word &= (std::uint32_t{1} << remaining) - 1u;
            result.OccupiedBits += static_cast<std::uint64_t>(std::popcount(word));
            if (header.NumFreeIndices != 0 &&
                static_cast<std::size_t>(header.FirstFreeIndex) / 32u == wordIndex)
                freeHeadOccupied =
                    (word & (std::uint32_t{1} << (header.FirstFreeIndex % 32))) != 0;
        }
        if (result.OccupiedBits != static_cast<std::uint64_t>(result.LiveEntries))
            return fail("bitmap-population-mismatch");
        if (freeHeadOccupied)
            return fail("free-list-head-is-occupied");
        result.BitmapValid = true;

        // Detect header changes; this is not a lock against native mutation.
        ShadowHeapMapHeader after{};
        if (!IsReadableRange(mapStorage, sizeof(after)))
        {
            result.HeaderValid = false;
            return fail("header-became-unreadable");
        }
        std::memcpy(&after, mapStorage, sizeof(after));
        if (std::memcmp(&header, &after, sizeof(header)) != 0)
        {
            result.HeaderValid = false;
            return fail("header-changed-during-validation");
        }
        result.Valid = true;
        result.FailureReason = "none";
        return result;
    }
#endif

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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE

    // Property-kind ABI rules only; no game-specific descriptors or payload reads.
    bool ReadShadowMapValueExtent(
        const FProperty* property,
        ShadowMapValueExtent& extent,
        std::uint32_t depth = 0)
    {
        if (depth > kShadowMaxStructDepth || property == nullptr ||
            !IsReadableRange(property, sizeof(FProperty)) ||
            !HasCastFlag(property, EClassCastFlags::Property) ||
            property->ElementSize <= 0 || property->ArrayDim <= 0)
            return false;

        std::size_t minimumBytes = 0;
        if (HasCastFlag(property, EClassCastFlags::StructProperty))
        {
            if (!IsReadableRange(property, sizeof(FStructProperty)))
                return false;
            const auto* descriptor = static_cast<const FStructProperty*>(property)->Struct;
            if (descriptor == nullptr || !IsReadableRange(descriptor, sizeof(UStruct)) ||
                descriptor->Size <= 0 || descriptor->MinAlignment <= 0)
                return false;
            minimumBytes = static_cast<std::size_t>(descriptor->Size);
            extent.Alignment = static_cast<std::size_t>(descriptor->MinAlignment);
        }
        else if (HasCastFlag(property, EClassCastFlags::EnumProperty))
        {
            if (!IsReadableRange(property, sizeof(FEnumProperty)))
                return false;
            ShadowMapValueExtent underlying{};
            if (!ReadShadowMapValueExtent(
                    static_cast<const FEnumProperty*>(property)->UnderlayingProperty,
                    underlying, depth + 1u))
                return false;
            minimumBytes = underlying.Bytes;
            extent.Alignment = underlying.Alignment;
        }
        else if (HasCastFlag(property, EClassCastFlags::Int8Property) ||
            HasCastFlag(property, EClassCastFlags::ByteProperty))
            minimumBytes = extent.Alignment = 1;
        else if (HasCastFlag(property, EClassCastFlags::Int16Property) ||
            HasCastFlag(property, EClassCastFlags::UInt16Property))
            minimumBytes = extent.Alignment = 2;
        else if (HasCastFlag(property, EClassCastFlags::IntProperty) ||
            HasCastFlag(property, EClassCastFlags::UInt32Property) ||
            HasCastFlag(property, EClassCastFlags::FloatProperty))
            minimumBytes = extent.Alignment = 4;
        else if (HasCastFlag(property, EClassCastFlags::Int64Property) ||
            HasCastFlag(property, EClassCastFlags::UInt64Property) ||
            HasCastFlag(property, EClassCastFlags::DoubleProperty))
            minimumBytes = extent.Alignment = 8;
        else if (HasCastFlag(property, EClassCastFlags::ArrayProperty) ||
            HasCastFlag(property, EClassCastFlags::StrProperty))
        {
            minimumBytes = 0x10;
            extent.Alignment = alignof(void*);
        }
        else if (HasCastFlag(property, EClassCastFlags::MapProperty) ||
            HasCastFlag(property, EClassCastFlags::SetProperty))
        {
            minimumBytes = 0x50;
            extent.Alignment = alignof(void*);
        }
        else if (HasCastFlag(property, EClassCastFlags::ObjectProperty))
            minimumBytes = extent.Alignment = sizeof(void*);
        else
            return false; // Alignment not established for this property kind.

        const auto elementSize = static_cast<std::size_t>(property->ElementSize);
        const auto arrayDim = static_cast<std::size_t>(property->ArrayDim);
        if (extent.Alignment == 0 ||
            (extent.Alignment & (extent.Alignment - 1u)) != 0 ||
            minimumBytes > elementSize || elementSize % extent.Alignment != 0 ||
            arrayDim > (std::numeric_limits<std::size_t>::max)() / elementSize)
            return false;
        extent.Bytes = elementSize * arrayDim;
        return true;
    }

    ShadowMapSlotValidation ValidateShadowMapSlots(
        const ShadowNativeMapMetadata& metadata,
        const ShadowHeapMapValidation& heapMap,
        const void* mapStorage,
        const FProperty* key,
        const FProperty* value,
        ShadowMapSlotBudget& budget,
        bool (*visit)(const void*, const void*, void*) = nullptr,
        void* context = nullptr)
    {
        ShadowMapSlotValidation result{};
        const auto fail = [&](const char* reason)
        {
            result.FailureReason = reason;
            return result;
        };
        if (!heapMap.Valid)
            return fail(heapMap.FailureReason);
        if (!ReadShadowMapValueExtent(key, result.Key))
            return fail("key-extent-or-alignment-unavailable");
        if (!ReadShadowMapValueExtent(value, result.Value))
            return fail("value-extent-or-alignment-unavailable");

        const auto& layout = metadata.Layout;
        const auto stride = static_cast<std::size_t>(layout.SparseSlotStride);
        const auto valueOffset = static_cast<std::size_t>(layout.ValueOffset);
        const auto payloadEnd = static_cast<std::size_t>(
            layout.HashNextIdOffset < layout.HashIndexOffset
                ? layout.HashNextIdOffset : layout.HashIndexOffset);
        if (payloadEnd > stride || valueOffset > payloadEnd ||
            result.Key.Bytes > valueOffset ||
            result.Value.Bytes > payloadEnd - valueOffset)
            return fail("key-value-extents-outside-payload");
        result.ExtentsValid = true;
        const auto slotAlignment = static_cast<std::size_t>(layout.SparseAlignment);
        if (slotAlignment % result.Key.Alignment != 0 ||
            slotAlignment % result.Value.Alignment != 0 ||
            valueOffset % result.Value.Alignment != 0)
            return fail("layout-key-value-alignment-mismatch");
        result.AlignmentValid = true;

        const auto& header = heapMap.Header;
        std::uint32_t word = 0;
        for (std::int32_t index = 0; index < header.ArrayNum; ++index)
        {
            result.FailedSlot = index;
            if (budget.RemainingSlots == 0)
                return fail("scanned-slot-budget-exhausted");
            --budget.RemainingSlots;
            ++result.ScannedSlots;
            if ((index % 32) == 0)
            {
                if (budget.RemainingBitmapWords == 0)
                    return fail("bitmap-word-budget-exhausted");
                --budget.RemainingBitmapWords;
                const auto wordIndex = static_cast<std::size_t>(index) / 32u;
                if (header.SecondaryBitmap == 0)
                    word = header.InlineBitmap[wordIndex];
                else
                {
                    const auto offset = wordIndex * sizeof(word);
                    if (header.SecondaryBitmap >
                        (std::numeric_limits<std::uintptr_t>::max)() - offset)
                        return fail("bitmap-address-overflow");
                    const auto* address = reinterpret_cast<const void*>(
                        header.SecondaryBitmap + offset);
                    if (!IsReadableRange(address, sizeof(word)))
                        return fail("bitmap-word-became-unreadable");
                    std::memcpy(&word, address, sizeof(word));
                }
            }
            if ((word & (std::uint32_t{1} << (index % 32))) == 0)
                continue;
            ++result.OccupiedSlots;
            if (budget.RemainingEntries == 0)
                return fail("occupied-entry-budget-exhausted");
            --budget.RemainingEntries;
            const auto indexWide = static_cast<std::size_t>(index);
            if (indexWide > (std::numeric_limits<std::size_t>::max)() / stride)
                return fail("slot-offset-overflow");
            const auto offset = indexWide * stride;
            if (header.SparseData > (std::numeric_limits<std::uintptr_t>::max)() - offset)
                return fail("slot-address-overflow");
            const auto slot = header.SparseData + offset;
            if (slot > (std::numeric_limits<std::uintptr_t>::max)() - stride)
                return fail("slot-end-overflow");
            const auto valueAddress = slot + valueOffset;
            if (slot % result.Key.Alignment != 0 ||
                valueAddress % result.Value.Alignment != 0)
            {
                result.AlignmentValid = false;
                return fail("slot-key-value-misaligned");
            }
            if (!IsReadableRange(reinterpret_cast<const void*>(slot), result.Key.Bytes) ||
                !IsReadableRange(reinterpret_cast<const void*>(valueAddress), result.Value.Bytes))
                return fail("slot-key-value-unreadable");
            // The optional test visitor receives already-addressed values.
            // It must not apply FProperty::Offset to either address again.
            ++result.ValidatedEntries;
            if (visit != nullptr && !visit(
                    reinterpret_cast<const void*>(slot),
                    reinterpret_cast<const void*>(valueAddress), context))
                return fail("recursive-work-budget-exhausted");
        }
        result.FailedSlot = -1;
        if (result.OccupiedSlots != heapMap.OccupiedBits ||
            result.ValidatedEntries != static_cast<std::uint64_t>(heapMap.LiveEntries))
            return fail("occupied-slot-count-changed");
        ShadowHeapMapHeader after{};
        if (!IsReadableRange(mapStorage, sizeof(after)))
            return fail("slot-validation-header-unreadable");
        std::memcpy(&after, mapStorage, sizeof(after));
        if (std::memcmp(&header, &after, sizeof(after)) != 0)
            return fail("slot-validation-header-changed");
        result.AllValidated = true;
        result.FailureReason = "none";
        return result;
    }
#endif

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
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
            ++summary.budgetFailures;
            summary.failureReason = "candidate-limit";
#endif
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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE

    bool TakeShadowFailureLog(ShadowValueSummary& summary)
    {
        if (summary.RemainingFailureLogs == 0)
            return false;
        --summary.RemainingFailureLogs;
        return true;
    }

    bool ChargeShadowWork(ShadowValueSummary& summary, std::uint32_t depth)
    {
        if (depth > summary.maxDepth)
            summary.maxDepth = depth;
        if (depth > kShadowMaxStructDepth || summary.RemainingWork == 0)
        {
            summary.complete = false;
            ++summary.unsupported;
            if (depth > kShadowMaxStructDepth)
            {
                ++summary.depthFailures;
                summary.failureReason = "depth-limit";
            }
            else
            {
                ++summary.budgetFailures;
                summary.failureReason = "cumulative-work-limit";
            }
            return false;
        }
        --summary.RemainingWork;
        return true;
    }

    struct ShadowMapVisitContext
    {
        const FProperty* Key;
        const FProperty* Value;
        const UScriptStruct* PersistentId;
        ShadowValueSummary& Summary;
        std::uint32_t Depth;
        std::uint64_t Entries = 0;
        std::uint64_t KeyAttempts = 0;
        std::uint64_t ValueAttempts = 0;
        std::uint64_t KeysComplete = 0;
        std::uint64_t ValuesComplete = 0;
        bool RecursiveComplete = true;
    };

    bool VisitShadowMapEntry(const void* keyStorage, const void* valueStorage, void* opaque)
    {
        auto& context = *static_cast<ShadowMapVisitContext*>(opaque);
        auto& summary = context.Summary;
        ++context.Entries;
        const auto traverse = [&](const FProperty* property, const void* storage)
        {
            bool complete = true;
            // A static reflected array occupies ElementSize * ArrayDim bytes;
            // the property walker consumes one already-addressed element.
            for (std::int32_t index = 0; index < property->ArrayDim; ++index)
            {
                if (summary.RemainingWork == 0)
                {
                    ChargeShadowWork(summary, context.Depth);
                    return false;
                }
                const auto* element = static_cast<const std::uint8_t*>(storage) +
                    static_cast<std::size_t>(index) * property->ElementSize;
                if (!TraverseShadowPropertyValue(property, element, context.Depth,
                        context.PersistentId, summary))
                    complete = false;
                if (summary.budgetFailures != 0 || summary.depthFailures != 0)
                    break;
            }
            return complete;
        };
        ++summary.TypedMapDepth;
        ++context.KeyAttempts;
        if (traverse(context.Key, keyStorage))
            ++context.KeysComplete;
        else
            context.RecursiveComplete = false;
        if (summary.budgetFailures == 0 && summary.depthFailures == 0)
        {
            ++context.ValueAttempts;
            if (traverse(context.Value, valueStorage))
                ++context.ValuesComplete;
            else
                context.RecursiveComplete = false;
        }
        --summary.TypedMapDepth;
        // Unsupported content is recorded but does not skip the remaining
        // entries. Exhausted shared budgets stop all enclosing map scans.
        return summary.budgetFailures == 0 && summary.depthFailures == 0;
    }

    ShadowMapInspection InspectShadowMap(const FProperty* property,
        const void* storage, ShadowMapSlotBudget& preflightBudget)
    {
        ShadowMapInspection result{};
        if (property == nullptr || !IsReadableRange(property, sizeof(FProperty)) ||
            !HasCastFlag(property, EClassCastFlags::MapProperty) ||
            property->ArrayDim <= 0 || property->ElementSize != sizeof(ShadowHeapMapHeader))
        {
            result.FailureReason = "invalid-map-property";
            return result;
        }
        result.Metadata = ReadShadowNativeMapMetadata(property);
        result.Heap = ValidateShadowHeapMap(result.Metadata, storage, property->ElementSize,
            &preflightBudget.RemainingBitmapWords);
        const auto* map = reinterpret_cast<const FMapProperty*>(property);
        const auto* key = result.Metadata.DescriptorReadable ? map->KeyProperty : nullptr;
        const auto* value = result.Metadata.DescriptorReadable ? map->ValueProperty : nullptr;
        result.Slots = ValidateShadowMapSlots(result.Metadata, result.Heap, storage,
            key, value, preflightBudget);
        result.StructuralValid = result.Slots.AllValidated;
        result.FailureReason = result.Heap.Valid ? result.Slots.FailureReason : result.Heap.FailureReason;
        return result;
    }

    bool TraverseShadowMapValue(
        const FProperty* property, const void* storage, std::uint32_t depth,
        const UScriptStruct* persistentId, ShadowValueSummary& summary)
    {
        const bool log = summary.RemainingMapLogs != 0;
        if (log)
            --summary.RemainingMapLogs;
        const auto inspection = InspectShadowMap(property, storage, summary.MapPrevalidationBudget);
        const auto& metadata = inspection.Metadata;
        const auto& heap = inspection.Heap;
        const auto& slots = inspection.Slots;
        const auto* map = reinterpret_cast<const FMapProperty*>(property);
        const auto* key = metadata.DescriptorReadable ? map->KeyProperty : nullptr;
        const auto* value = metadata.DescriptorReadable ? map->ValueProperty : nullptr;
        // No recursive visitor can run until every occupied slot has passed
        // structural prevalidation. Diagnostics only consume these snapshots.
        if (log)
            if (summary.Diagnostics != nullptr && summary.Diagnostics->MapValidated != nullptr)
                summary.Diagnostics->MapValidated(property, metadata, heap, slots, persistentId, depth);
        const auto exactBefore = summary.exactPersistentIds;
        const auto unsupportedBefore = summary.unsupported;
        const auto opaqueBefore = summary.opaqueBoundaries;
        ShadowMapVisitContext context{key, value, persistentId, summary, depth + 1u};
        ShadowMapSlotValidation visited{};
        if (slots.AllValidated)
            visited = ValidateShadowMapSlots(metadata, heap, storage, key, value,
                summary.MapSlotBudget, VisitShadowMapEntry, &context);
        else
            visited.FailureReason = slots.FailureReason;
        const bool complete = visited.AllValidated && context.RecursiveComplete;
        if (!complete)
        {
            summary.complete = false;
            if (!visited.AllValidated)
            {
                ++summary.unsupported;
                summary.failureReason = visited.FailureReason;
                if (std::strcmp(visited.FailureReason, "scanned-slot-budget-exhausted") == 0 ||
                    std::strcmp(visited.FailureReason, "occupied-entry-budget-exhausted") == 0 ||
                    std::strcmp(visited.FailureReason, "bitmap-word-budget-exhausted") == 0)
                    ++summary.budgetFailures;
            }
            else if (std::strcmp(summary.failureReason, "none") == 0)
                summary.failureReason = "unsupported-key-or-value";
        }
        if (log)
            ShadowLog(summary,
                "PersistentIdFix: reflection recursive map: property=%p "
                "structuralValid=%u slotScanComplete=%u entriesVisited=%llu "
                "keyAttempts=%llu keyComplete=%llu valueAttempts=%llu valueComplete=%llu "
                "exactPid=%llu unsupported=%llu opaque=%llu reflectedComplete=%u "
                "semanticCertified=0 preflightSlots=%llu traversalSlots=%llu "
                "depth=%u maxDepth=%llu budgetFailures=%llu "
                "depthFailures=%llu reason=%s",
                static_cast<const void*>(property), slots.AllValidated ? 1u : 0u,
                visited.AllValidated ? 1u : 0u,
                static_cast<unsigned long long>(context.Entries),
                static_cast<unsigned long long>(context.KeyAttempts),
                static_cast<unsigned long long>(context.KeysComplete),
                static_cast<unsigned long long>(context.ValueAttempts),
                static_cast<unsigned long long>(context.ValuesComplete),
                static_cast<unsigned long long>(summary.exactPersistentIds - exactBefore),
                static_cast<unsigned long long>(summary.unsupported - unsupportedBefore),
                static_cast<unsigned long long>(summary.opaqueBoundaries - opaqueBefore),
                complete ? 1u : 0u,
                static_cast<unsigned long long>(slots.ScannedSlots),
                static_cast<unsigned long long>(visited.ScannedSlots), depth,
                static_cast<unsigned long long>(summary.maxDepth),
                static_cast<unsigned long long>(summary.budgetFailures),
                static_cast<unsigned long long>(summary.depthFailures),
                complete ? "none" : summary.failureReason);
        // Reflected traversal is diagnostic discovery, never a certificate of
        // custom/native serialization or opaque bytes absent from reflection.
        return complete;
    }
#endif

    bool TraverseShadowStructValue(
        const UStruct* descriptor,
        const void* storage,
        std::uint32_t depth,
        const UScriptStruct* persistentIdDescriptor,
        ShadowValueSummary& summary)
    {
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        if (!ChargeShadowWork(summary, depth))
            return false;
        if (descriptor != nullptr && descriptor == summary.OpaqueStructDescriptor)
        {
            summary.complete = false;
            ++summary.unsupported;
            ++summary.opaqueBoundaries;
            summary.failureReason = "recognized-opaque-struct";
            return false;
        }
#endif
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
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
            if (summary.ExactPidSink != nullptr)
                summary.ExactPidSink->Observe(value);
#endif
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
                    if (TakeShadowFailureLog(summary))
                    {
                        ShadowLog(summary,
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
                        if (summary.Diagnostics != nullptr && summary.Diagnostics->ParentFailure != nullptr)
                            summary.Diagnostics->ParentFailure(descriptor, property, depth);
                    }
#endif
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

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        // Map integers (including GUID components/indices) are observations,
        // not typed PID discoveries and not numeric PID candidates.
        if (summary.TypedMapDepth != 0)
            return true;
#endif

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
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        if (!ChargeShadowWork(summary, depth))
            return false;
#endif
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
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
                    ++summary.budgetFailures;
                    summary.failureReason = "cumulative-array-element-limit";
#endif
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
        if (HasCastFlag(property, EClassCastFlags::MapProperty))
            return TraverseShadowMapValue(
                property, valueStorage, depth, persistentIdDescriptor, summary);
#endif

#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        if (TakeShadowFailureLog(summary))
        {
            // Classification-only breadcrumb. Never read or reinterpret the
            // unknown payload, and retain the existing fail-closed behavior.
            ShadowLog(summary,
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
            // Report reflected type bits without interpreting unknown value storage.
            // The class pointer is separately validated before any metadata access.
            const auto* leafClass = property->ClassPrivate;
            if (leafClass != nullptr &&
                IsReadableRange(leafClass, sizeof(FFieldClass)))
            {
                ShadowLog(summary,
                    "PersistentIdFix: reflection unsupported leaf cast flags: "
                    "depth=%u offset=%d elementSize=%d castFlags=0x%llX",
                    depth,
                    property->Offset,
                    property->ElementSize,
                    static_cast<unsigned long long>(leafClass->CastFlags));
            }
            else
            {
                ShadowLog(summary,
                    "PersistentIdFix: reflection unsupported leaf cast flags: "
                    "depth=%u classUnreadable=1",
                    depth);
            }
        }
#endif
        summary.complete = false;
        ++summary.unsupported;
#if PERSISTENTIDFIX_REFLECTION_TEST_MODE
        summary.failureReason = "unsupported-property-kind-or-opaque-storage";
#endif
        return false;
    }
}

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace PersistentIdFixSourceMemory
{
    inline bool IsReadableRange(const void* pointer, std::size_t byteCount)
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
}

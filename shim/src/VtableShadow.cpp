/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

#include "VtableShadow.hpp"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

#include "D3D12Vtable.hpp"
#include "ShimLog.hpp"

namespace D3D12Sim
{

void** VtableShadowSet::GetOrBuildShadow_Locked(void** OrigVtbl)
{
    auto It = m_ShadowByOrig.find(OrigVtbl);
    if (It != m_ShadowByOrig.end())
        return It->second;

    // Copy at most up to the end of the memory region that holds the vtable
    SIZE_T                   NumSlots = kVtblCopySlots;
    MEMORY_BASIC_INFORMATION Mbi{};
    if (::VirtualQuery(OrigVtbl, &Mbi, sizeof(Mbi)) == sizeof(Mbi))
    {
        const auto*  Base = static_cast<const char*>(Mbi.BaseAddress);
        const auto*  Ptr  = reinterpret_cast<const char*>(OrigVtbl);
        const SIZE_T Room = (Base != nullptr && Ptr >= Base) ? (Mbi.RegionSize - static_cast<SIZE_T>(Ptr - Base)) / sizeof(void*) : 0;
        if (Room > 0 && Room < NumSlots)
            NumSlots = Room;
    }

    // [ original vtable | slot 0 | slot 1 | ... ]
    void** Block = new void*[kVtblCopySlots + 1];
    std::memset(Block, 0, sizeof(void*) * (kVtblCopySlots + 1));
    Block[0]     = OrigVtbl;
    void** Slots = Block + 1;
    std::memcpy(Slots, OrigVtbl, NumSlots * sizeof(void*));
    m_Patch(Slots, NumSlots);

    m_ShadowByOrig.emplace(OrigVtbl, Slots);
    m_Shadows.insert(Slots);
    LogInfo("%s: built shadow #%zu for vtable %p (%zu slots copied)", m_Name, m_ShadowByOrig.size(), OrigVtbl, static_cast<size_t>(NumSlots));
    return Slots;
}

bool VtableShadowSet::Wrap(void* pObject)
{
    if (pObject == nullptr)
        return false;
    std::lock_guard<std::mutex> Lock{m_Mutex};
    void** Current = *static_cast<void***>(pObject);
    if (Current == nullptr)
        return false;
    if (m_Shadows.count(Current) != 0)
        return true; // already wrapped
    *static_cast<void***>(pObject) = GetOrBuildShadow_Locked(Current);
    return true;
}

bool VtableShadowSet::IsWrapped(const void* pObject) const
{
    if (pObject == nullptr)
        return false;
    std::lock_guard<std::mutex> Lock{m_Mutex};
    return m_Shadows.count(*static_cast<void** const*>(pObject)) != 0;
}

} // namespace D3D12Sim

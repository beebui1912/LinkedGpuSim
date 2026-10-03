/*
 *  Copyright 2019-2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  See http://www.apache.org/licenses/LICENSE-2.0
 */

//  VtableShadow
//  ------------
//  Replaces methods of a COM object by swapping the object's vtable pointer
//  for a "shadow": a heap copy of the original vtable with some slots patched.
//  The original vtable is stored in the entry before slot 0, so a hook reaches
//  the real method with GetOrigVtbl(This)[Slot] (no lookup, no lock).
//
//  One shadow exists per original vtable (D3D12 and its debug layer use
//  different classes, so a process sees several).  Shadows are never freed:
//  an object may outlive any point at which freeing would be safe.  Whether an
//  object is already wrapped is decided from its current vtable pointer, so an
//  object allocated at the address of a released one is wrapped again.

#pragma once

#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace D3D12Sim
{

class VtableShadowSet
{
public:
    // Patches the slots of a new shadow. NumSlots is the number of entries
    // copied from the original vtable; slots at or above it must not be patched.
    using PatchFn = void (*)(void** Slots, size_t NumSlots);

    VtableShadowSet(const char* Name, PatchFn Patch) :
        m_Name{Name}, m_Patch{Patch} {}

    // Swaps the object's vtable for the shadow of its class. Returns false if
    // the object could not be wrapped; true if it is wrapped (now or before).
    bool Wrap(void* pObject);

    // Whether the object currently dispatches through one of this set's shadows.
    bool IsWrapped(const void* pObject) const;

    static void** GetOrigVtbl(const void* pObject)
    {
        void** Slots = *static_cast<void** const*>(pObject);
        return static_cast<void**>(Slots[-1]);
    }

private:
    void** GetOrBuildShadow_Locked(void** OrigVtbl);

    const char* const          m_Name;
    const PatchFn              m_Patch;
    mutable std::mutex         m_Mutex;
    std::unordered_map<void**, void**> m_ShadowByOrig; // original vtable -> shadow slot 0
    std::unordered_set<void**>         m_Shadows;      // shadow slot 0 pointers
};

} // namespace D3D12Sim

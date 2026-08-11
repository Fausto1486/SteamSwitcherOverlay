#pragma once
// ModKitInterop.h — lazy, optional access to a handful of ModKit.dll
// exports that OverlayContent.h wants (row click-to-configure, pool
// searching/clearing status) but that current ModKit.dll does not export
// yet (confirmed: absent from current ModKit.h as of this port — see
// OVERLAY-REDESIGN-RESULT.md's project history). Resolved via
// GetProcAddress, not a header include or static link — this DLL has no
// build-time dependency on ModKit.dll at all, consistent with
// SharedDataReader.h's own no-link philosophy.
//
// Every function here degrades gracefully to a safe default (false/no-op)
// if ModKit.dll isn't loaded, or is loaded but doesn't export the symbol
// yet. This means the overlay's mod rows render as non-clickable and the
// pool-searching/clearing indicator never shows until ModKit.cpp actually
// adds these four exports — a real, known limitation, not a bug. Adding
// those exports to ModKit.cpp is separate work, out of scope for this file.

#include <Windows.h>

namespace ModKitInterop {

    typedef bool (*HasButton_t)(const char*);
    typedef void (*ClickButton_t)(const char*);
    typedef bool (*IsPoolSearching_t)();
    typedef bool (*IsPoolClearing_t)();

    struct ResolvedFns {
        HasButton_t hasButton = nullptr;
        ClickButton_t clickButton = nullptr;
        IsPoolSearching_t isPoolSearching = nullptr;
        IsPoolClearing_t isPoolClearing = nullptr;
        bool attempted = false; // re-attempt resolution each call until ModKit.dll is found —
                                 // it may load into this process after this DLL already has
    };

    inline ResolvedFns& Fns() {
        static ResolvedFns fns;
        return fns;
    }

    // Re-resolves every call rather than caching "not found" permanently —
    // ModKit.dll's load order relative to this DLL isn't guaranteed, and a
    // GetProcAddress call is cheap enough to not need caching failure.
    inline void EnsureResolved() {
        ResolvedFns& fns = Fns();
        if (fns.hasButton && fns.clickButton && fns.isPoolSearching && fns.isPoolClearing) return;

        HMODULE hModKit = GetModuleHandleA("ModKit.dll");
        if (!hModKit) return;

        if (!fns.hasButton) fns.hasButton = reinterpret_cast<HasButton_t>(GetProcAddress(hModKit, "ModKit_HasButton"));
        if (!fns.clickButton) fns.clickButton = reinterpret_cast<ClickButton_t>(GetProcAddress(hModKit, "ModKit_ClickButton"));
        if (!fns.isPoolSearching) fns.isPoolSearching = reinterpret_cast<IsPoolSearching_t>(GetProcAddress(hModKit, "ModKit_IsPoolSearching"));
        if (!fns.isPoolClearing) fns.isPoolClearing = reinterpret_cast<IsPoolClearing_t>(GetProcAddress(hModKit, "ModKit_IsPoolClearing"));
    }

    inline bool HasButton(const char* modName) {
        EnsureResolved();
        return Fns().hasButton ? Fns().hasButton(modName) : false;
    }

    inline void ClickButton(const char* modName) {
        EnsureResolved();
        if (Fns().clickButton) Fns().clickButton(modName);
    }

    inline bool IsPoolSearching() {
        EnsureResolved();
        return Fns().isPoolSearching ? Fns().isPoolSearching() : false;
    }

    inline bool IsPoolClearing() {
        EnsureResolved();
        return Fns().isPoolClearing ? Fns().isPoolClearing() : false;
    }

} // namespace ModKitInterop

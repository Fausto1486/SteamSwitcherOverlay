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
#include <cstdint>

namespace ModKitInterop {

    // Mirrors ModKit.h's ModKitConfigRowType/ModKitConfigRowView exactly —
    // deliberately redefined here rather than #include "ModKit.h", per this
    // file's own no-header-include/no-static-link philosophy stated above.
    // Both are plain POD/enum, so a byte-for-byte copy is safe as long as
    // it's kept in sync with ModKit.h's definition.
    enum ModKitConfigRowType {
        MODKIT_ROW_TOGGLE = 0,
        MODKIT_ROW_FLOAT = 1,
        MODKIT_ROW_INT = 2,
        MODKIT_ROW_DROPDOWN = 3,
        MODKIT_ROW_STATUS = 4,
    };

    struct ModKitConfigRowView {
        int  type;
        char label[128];
        char valueText[64];
        float numMin, numMax;
        int  dropdownCount;
    };

    typedef bool (*HasButton_t)(const char*);
    typedef void (*ClickButton_t)(const char*);
    typedef void (*ClickButtonForOverlay_t)(const char*);
    typedef bool (*IsPoolSearching_t)();
    typedef bool (*IsPoolClearing_t)();

    // Config-window overlay bridge — see ModKit.h's own comment block
    // ("CONFIG WINDOW OVERLAY BRIDGE") for the full design. Struct layouts
    // (ModKitConfigRowView) come straight from ModKit.h, same header both
    // sides already share.

    typedef bool (*HasConfigWindow_t)(const char*);
    typedef bool (*GetConfigWindowTitle_t)(const char*, char*, int);
    typedef int  (*GetConfigWindowRowCount_t)(const char*);
    typedef bool (*GetConfigWindowRow_t)(const char*, int, ModKitConfigRowView*);
    typedef bool (*GetConfigWindowDropdownOption_t)(const char*, int, int, char*, int);
    typedef void (*SetConfigWindowToggle_t)(const char*, int);
    typedef bool (*SetConfigWindowFloat_t)(const char*, int, float);
    typedef bool (*SetConfigWindowInt_t)(const char*, int, int32_t);
    typedef void (*SetConfigWindowDropdown_t)(const char*, int, int32_t);
    typedef void (*CloseConfigWindowFromOverlay_t)(const char*);
    typedef void (*CloseAllConfigWindows_t)();

    // Stats-window overlay bridge — mirrors ModKit.h's "STATS WINDOW OVERLAY
    // BRIDGE" section. ModKitStatsRowView is a byte-for-byte copy of
    // ModKitStatsRowView in ModKit.h, same no-header-include philosophy as
    // ModKitConfigRowView above.
    enum ModKitStatsRowType {
        MODKIT_STATS_DIVIDER = 0,
        MODKIT_STATS_BAR = 1,
        MODKIT_STATS_TEXT = 2,
        MODKIT_STATS_EDIT = 3,
        MODKIT_STATS_BUTTON = 4,
        MODKIT_STATS_CHECKBOX = 5,
        MODKIT_STATS_DROPDOWN = 6,
        MODKIT_STATS_COLUMN_BREAK = 7,
    };

    struct ModKitStatsRowView {
        int  type;
        char label[128];
        char valueText[64];
        float barFrac;
        uint32_t barColor;
        bool valid;
        bool enabled;
        bool checked;
        int  dropdownCount;
        bool defaultCollapsed;   // Divider only — see ModKit.h's own copy of this field
    };

    typedef bool (*HasStatsWindow_t)(const char*);
    typedef bool (*GetStatsWindowTitle_t)(const char*, char*, int);
    typedef int  (*GetStatsWindowRowCount_t)(const char*);
    typedef bool (*GetStatsWindowRow_t)(const char*, int, ModKitStatsRowView*);
    typedef bool (*GetStatsWindowDropdownOption_t)(const char*, int, int, char*, int);
    typedef void (*ClickStatsWindowButton_t)(const char*, int);
    typedef void (*ToggleStatsWindowCheckbox_t)(const char*, int);
    typedef void (*ChangeStatsWindowEdit_t)(const char*, int, const char*);
    typedef void (*SelectStatsWindowDropdown_t)(const char*, int, int32_t);
    typedef void (*CloseStatsWindowFromOverlay_t)(const char*);
    typedef void (*CloseAllStatsWindows_t)();

    // Persistent counterpart to ClickButtonForOverlay's per-call flag - see
    // ModKit_IsOverlayModeActive's own comment in ModKit.h. Pushed every
    // frame from DrawOverlay, not just on change, so it self-heals within
    // one frame regardless of DLL load order.
    typedef void (*SetOverlayModeActive_t)(bool);

    struct ResolvedFns {
        HasButton_t hasButton = nullptr;
        ClickButton_t clickButton = nullptr;
        ClickButtonForOverlay_t clickButtonForOverlay = nullptr;
        IsPoolSearching_t isPoolSearching = nullptr;
        IsPoolClearing_t isPoolClearing = nullptr;
        HasConfigWindow_t hasConfigWindow = nullptr;
        GetConfigWindowTitle_t getConfigWindowTitle = nullptr;
        GetConfigWindowRowCount_t getConfigWindowRowCount = nullptr;
        GetConfigWindowRow_t getConfigWindowRow = nullptr;
        GetConfigWindowDropdownOption_t getConfigWindowDropdownOption = nullptr;
        SetConfigWindowToggle_t setConfigWindowToggle = nullptr;
        SetConfigWindowFloat_t setConfigWindowFloat = nullptr;
        SetConfigWindowInt_t setConfigWindowInt = nullptr;
        SetConfigWindowDropdown_t setConfigWindowDropdown = nullptr;
        CloseConfigWindowFromOverlay_t closeConfigWindowFromOverlay = nullptr;
        CloseAllConfigWindows_t closeAllConfigWindows = nullptr;
        HasStatsWindow_t hasStatsWindow = nullptr;
        GetStatsWindowTitle_t getStatsWindowTitle = nullptr;
        GetStatsWindowRowCount_t getStatsWindowRowCount = nullptr;
        GetStatsWindowRow_t getStatsWindowRow = nullptr;
        GetStatsWindowDropdownOption_t getStatsWindowDropdownOption = nullptr;
        ClickStatsWindowButton_t clickStatsWindowButton = nullptr;
        ToggleStatsWindowCheckbox_t toggleStatsWindowCheckbox = nullptr;
        ChangeStatsWindowEdit_t changeStatsWindowEdit = nullptr;
        SelectStatsWindowDropdown_t selectStatsWindowDropdown = nullptr;
        CloseStatsWindowFromOverlay_t closeStatsWindowFromOverlay = nullptr;
        CloseAllStatsWindows_t closeAllStatsWindows = nullptr;
        SetOverlayModeActive_t setOverlayModeActive = nullptr;
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
        if (fns.hasButton && fns.clickButton && fns.clickButtonForOverlay && fns.isPoolSearching && fns.isPoolClearing
            && fns.getConfigWindowRow) return;

        HMODULE hModKit = GetModuleHandleA("ModKit.dll");
        if (!hModKit) return;

        if (!fns.hasButton) fns.hasButton = reinterpret_cast<HasButton_t>(GetProcAddress(hModKit, "ModKit_HasButton"));
        if (!fns.clickButton) fns.clickButton = reinterpret_cast<ClickButton_t>(GetProcAddress(hModKit, "ModKit_ClickButton"));
        if (!fns.clickButtonForOverlay) fns.clickButtonForOverlay = reinterpret_cast<ClickButtonForOverlay_t>(GetProcAddress(hModKit, "ModKit_ClickButtonForOverlay"));
        if (!fns.isPoolSearching) fns.isPoolSearching = reinterpret_cast<IsPoolSearching_t>(GetProcAddress(hModKit, "ModKit_IsPoolSearching"));
        if (!fns.isPoolClearing) fns.isPoolClearing = reinterpret_cast<IsPoolClearing_t>(GetProcAddress(hModKit, "ModKit_IsPoolClearing"));
        if (!fns.hasConfigWindow) fns.hasConfigWindow = reinterpret_cast<HasConfigWindow_t>(GetProcAddress(hModKit, "ModKit_HasConfigWindow"));
        if (!fns.getConfigWindowTitle) fns.getConfigWindowTitle = reinterpret_cast<GetConfigWindowTitle_t>(GetProcAddress(hModKit, "ModKit_GetConfigWindowTitle"));
        if (!fns.getConfigWindowRowCount) fns.getConfigWindowRowCount = reinterpret_cast<GetConfigWindowRowCount_t>(GetProcAddress(hModKit, "ModKit_GetConfigWindowRowCount"));
        if (!fns.getConfigWindowRow) fns.getConfigWindowRow = reinterpret_cast<GetConfigWindowRow_t>(GetProcAddress(hModKit, "ModKit_GetConfigWindowRow"));
        if (!fns.getConfigWindowDropdownOption) fns.getConfigWindowDropdownOption = reinterpret_cast<GetConfigWindowDropdownOption_t>(GetProcAddress(hModKit, "ModKit_GetConfigWindowDropdownOption"));
        if (!fns.setConfigWindowToggle) fns.setConfigWindowToggle = reinterpret_cast<SetConfigWindowToggle_t>(GetProcAddress(hModKit, "ModKit_SetConfigWindowToggle"));
        if (!fns.setConfigWindowFloat) fns.setConfigWindowFloat = reinterpret_cast<SetConfigWindowFloat_t>(GetProcAddress(hModKit, "ModKit_SetConfigWindowFloat"));
        if (!fns.setConfigWindowInt) fns.setConfigWindowInt = reinterpret_cast<SetConfigWindowInt_t>(GetProcAddress(hModKit, "ModKit_SetConfigWindowInt"));
        if (!fns.setConfigWindowDropdown) fns.setConfigWindowDropdown = reinterpret_cast<SetConfigWindowDropdown_t>(GetProcAddress(hModKit, "ModKit_SetConfigWindowDropdown"));
        if (!fns.closeConfigWindowFromOverlay) fns.closeConfigWindowFromOverlay = reinterpret_cast<CloseConfigWindowFromOverlay_t>(GetProcAddress(hModKit, "ModKit_CloseConfigWindowFromOverlay"));
        if (!fns.closeAllConfigWindows) fns.closeAllConfigWindows = reinterpret_cast<CloseAllConfigWindows_t>(GetProcAddress(hModKit, "ModKit_CloseAllConfigWindows"));
        if (!fns.hasStatsWindow) fns.hasStatsWindow = reinterpret_cast<HasStatsWindow_t>(GetProcAddress(hModKit, "ModKit_HasStatsWindow"));
        if (!fns.getStatsWindowTitle) fns.getStatsWindowTitle = reinterpret_cast<GetStatsWindowTitle_t>(GetProcAddress(hModKit, "ModKit_GetStatsWindowTitle"));
        if (!fns.getStatsWindowRowCount) fns.getStatsWindowRowCount = reinterpret_cast<GetStatsWindowRowCount_t>(GetProcAddress(hModKit, "ModKit_GetStatsWindowRowCount"));
        if (!fns.getStatsWindowRow) fns.getStatsWindowRow = reinterpret_cast<GetStatsWindowRow_t>(GetProcAddress(hModKit, "ModKit_GetStatsWindowRow"));
        if (!fns.getStatsWindowDropdownOption) fns.getStatsWindowDropdownOption = reinterpret_cast<GetStatsWindowDropdownOption_t>(GetProcAddress(hModKit, "ModKit_GetStatsWindowDropdownOption"));
        if (!fns.clickStatsWindowButton) fns.clickStatsWindowButton = reinterpret_cast<ClickStatsWindowButton_t>(GetProcAddress(hModKit, "ModKit_ClickStatsWindowButton"));
        if (!fns.toggleStatsWindowCheckbox) fns.toggleStatsWindowCheckbox = reinterpret_cast<ToggleStatsWindowCheckbox_t>(GetProcAddress(hModKit, "ModKit_ToggleStatsWindowCheckbox"));
        if (!fns.changeStatsWindowEdit) fns.changeStatsWindowEdit = reinterpret_cast<ChangeStatsWindowEdit_t>(GetProcAddress(hModKit, "ModKit_ChangeStatsWindowEdit"));
        if (!fns.selectStatsWindowDropdown) fns.selectStatsWindowDropdown = reinterpret_cast<SelectStatsWindowDropdown_t>(GetProcAddress(hModKit, "ModKit_SelectStatsWindowDropdown"));
        if (!fns.closeStatsWindowFromOverlay) fns.closeStatsWindowFromOverlay = reinterpret_cast<CloseStatsWindowFromOverlay_t>(GetProcAddress(hModKit, "ModKit_CloseStatsWindowFromOverlay"));
        if (!fns.closeAllStatsWindows) fns.closeAllStatsWindows = reinterpret_cast<CloseAllStatsWindows_t>(GetProcAddress(hModKit, "ModKit_CloseAllStatsWindows"));
        if (!fns.setOverlayModeActive) fns.setOverlayModeActive = reinterpret_cast<SetOverlayModeActive_t>(GetProcAddress(hModKit, "ModKit_SetOverlayModeActive"));
    }

    inline bool HasButton(const char* modName) {
        EnsureResolved();
        return Fns().hasButton ? Fns().hasButton(modName) : false;
    }

    inline void ClickButton(const char* modName) {
        EnsureResolved();
        if (Fns().clickButton) Fns().clickButton(modName);
    }

    // Use this instead of ClickButton for a click that originates from the
    // in-game overlay row itself - see ModKit_ClickButtonForOverlay's own
    // comment in ModKit.h for why this needs to be a distinct entry point.
    inline void ClickButtonForOverlay(const char* modName) {
        EnsureResolved();
        if (Fns().clickButtonForOverlay) Fns().clickButtonForOverlay(modName);
    }

    inline bool IsPoolSearching() {
        EnsureResolved();
        return Fns().isPoolSearching ? Fns().isPoolSearching() : false;
    }

    inline bool IsPoolClearing() {
        EnsureResolved();
        return Fns().isPoolClearing ? Fns().isPoolClearing() : false;
    }

    inline bool HasConfigWindow(const char* modName) {
        EnsureResolved();
        return Fns().hasConfigWindow ? Fns().hasConfigWindow(modName) : false;
    }

    inline bool GetConfigWindowTitle(const char* modName, char* outTitle, int outTitleSize) {
        EnsureResolved();
        return Fns().getConfigWindowTitle ? Fns().getConfigWindowTitle(modName, outTitle, outTitleSize) : false;
    }

    inline int GetConfigWindowRowCount(const char* modName) {
        EnsureResolved();
        return Fns().getConfigWindowRowCount ? Fns().getConfigWindowRowCount(modName) : 0;
    }

    inline bool GetConfigWindowRow(const char* modName, int rowIndex, ModKitConfigRowView* out) {
        EnsureResolved();
        return Fns().getConfigWindowRow ? Fns().getConfigWindowRow(modName, rowIndex, out) : false;
    }

    inline bool GetConfigWindowDropdownOption(const char* modName, int rowIndex, int optionIndex, char* outText, int outTextSize) {
        EnsureResolved();
        return Fns().getConfigWindowDropdownOption ? Fns().getConfigWindowDropdownOption(modName, rowIndex, optionIndex, outText, outTextSize) : false;
    }

    inline void SetConfigWindowToggle(const char* modName, int rowIndex) {
        EnsureResolved();
        if (Fns().setConfigWindowToggle) Fns().setConfigWindowToggle(modName, rowIndex);
    }

    inline bool SetConfigWindowFloat(const char* modName, int rowIndex, float value) {
        EnsureResolved();
        return Fns().setConfigWindowFloat ? Fns().setConfigWindowFloat(modName, rowIndex, value) : false;
    }

    inline bool SetConfigWindowInt(const char* modName, int rowIndex, int32_t value) {
        EnsureResolved();
        return Fns().setConfigWindowInt ? Fns().setConfigWindowInt(modName, rowIndex, value) : false;
    }

    inline void SetConfigWindowDropdown(const char* modName, int rowIndex, int32_t optionIndex) {
        EnsureResolved();
        if (Fns().setConfigWindowDropdown) Fns().setConfigWindowDropdown(modName, rowIndex, optionIndex);
    }

    inline void CloseConfigWindowFromOverlay(const char* modName) {
        EnsureResolved();
        if (Fns().closeConfigWindowFromOverlay) Fns().closeConfigWindowFromOverlay(modName);
    }

    inline void CloseAllConfigWindows() {
        EnsureResolved();
        if (Fns().closeAllConfigWindows) Fns().closeAllConfigWindows();
    }

    inline bool HasStatsWindow(const char* modName) {
        EnsureResolved();
        return Fns().hasStatsWindow ? Fns().hasStatsWindow(modName) : false;
    }

    inline bool GetStatsWindowTitle(const char* modName, char* outTitle, int outTitleSize) {
        EnsureResolved();
        return Fns().getStatsWindowTitle ? Fns().getStatsWindowTitle(modName, outTitle, outTitleSize) : false;
    }

    inline int GetStatsWindowRowCount(const char* modName) {
        EnsureResolved();
        return Fns().getStatsWindowRowCount ? Fns().getStatsWindowRowCount(modName) : 0;
    }

    inline bool GetStatsWindowRow(const char* modName, int rowIndex, ModKitStatsRowView* out) {
        EnsureResolved();
        return Fns().getStatsWindowRow ? Fns().getStatsWindowRow(modName, rowIndex, out) : false;
    }

    inline bool GetStatsWindowDropdownOption(const char* modName, int rowIndex, int optionIndex, char* outText, int outTextSize) {
        EnsureResolved();
        return Fns().getStatsWindowDropdownOption ? Fns().getStatsWindowDropdownOption(modName, rowIndex, optionIndex, outText, outTextSize) : false;
    }

    inline void ClickStatsWindowButton(const char* modName, int rowIndex) {
        EnsureResolved();
        if (Fns().clickStatsWindowButton) Fns().clickStatsWindowButton(modName, rowIndex);
    }

    inline void ToggleStatsWindowCheckbox(const char* modName, int rowIndex) {
        EnsureResolved();
        if (Fns().toggleStatsWindowCheckbox) Fns().toggleStatsWindowCheckbox(modName, rowIndex);
    }

    inline void ChangeStatsWindowEdit(const char* modName, int rowIndex, const char* text) {
        EnsureResolved();
        if (Fns().changeStatsWindowEdit) Fns().changeStatsWindowEdit(modName, rowIndex, text);
    }

    inline void SelectStatsWindowDropdown(const char* modName, int rowIndex, int32_t optionIndex) {
        EnsureResolved();
        if (Fns().selectStatsWindowDropdown) Fns().selectStatsWindowDropdown(modName, rowIndex, optionIndex);
    }

    inline void CloseStatsWindowFromOverlay(const char* modName) {
        EnsureResolved();
        if (Fns().closeStatsWindowFromOverlay) Fns().closeStatsWindowFromOverlay(modName);
    }

    inline void CloseAllStatsWindows() {
        EnsureResolved();
        if (Fns().closeAllStatsWindows) Fns().closeAllStatsWindows();
    }

    inline void SetOverlayModeActive(bool active) {
        EnsureResolved();
        if (Fns().setOverlayModeActive) Fns().setOverlayModeActive(active);
    }

} // namespace ModKitInterop
#pragma once
// SharedDataReader.h — read-only access to ModKit's ModKitSharedData_v1
// named shared-memory block. Ported directly from current SteamSwitcher's
// Logic/Classes/ModSharedStatusReadercs.cs — same layout, same read
// philosophy (open fresh every read, dispose immediately, no locking, torn
// reads self-correct on next resync). No ModKit.dll dependency: this reads
// the shared-memory block ModKit.cpp creates, entirely independent of
// whether ModKit.dll itself is loaded, has exports, or is even present.
//
// Per OVERLAY-DLL-BLUEPRINT.md's own explicit warning: this layout was
// pulled from CURRENT SteamSwitcher source, not guessed at or reconstructed
// from memory. If ModKit.cpp's SharedBlock/SharedEntry layout ever changes,
// re-port from ModSharedStatusReadercs.cs again — don't hand-edit the
// constants below from assumption.

#include <Windows.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace SharedDataReader {

    constexpr const char* kMapName = "ModKitSharedData_v1";
    constexpr int kKeyLen = 64;
    constexpr int kValLen = 256;
    constexpr int kEntrySize = kKeyLen + kValLen + 1; // + trailing `bool used` byte
    constexpr int kMaxEntries = 64;
    constexpr long kHeaderSize = 4; // LONG lock
    constexpr long kTotalSize = kHeaderSize + static_cast<long>(kEntrySize) * kMaxEntries;

    struct ModStatus {
        bool hooked = false;
        bool outdated = false;
        bool busy = false;
        std::vector<std::string> activeSubFlags;      // #F — ModConfigWindow non-default rows
        std::vector<std::string> activeFeatureLabels; // #G — ordinary multi-flag features, display-only
        std::vector<std::string> pendingFeatureLabels; // #P — features currently resolving, see ModKit_NotifyFeatureBusy
    };

    inline std::string ReadNullTerminatedAscii(const char* buf, int len) {
        int end = 0;
        while (end < len && buf[end] != '\0') ++end;
        return std::string(buf, end);
    }

    inline std::vector<std::string> SplitNonEmpty(const std::string& s, char delim) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= s.size()) {
            size_t pos = s.find(delim, start);
            if (pos == std::string::npos) pos = s.size();
            if (pos > start) out.push_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return out;
    }

    // Returns false (empty map) if the mapping doesn't exist — i.e. no
    // ModKit-injected process is currently alive in THIS process. Since
    // this DLL only ever runs inside the game process itself, "doesn't
    // exist" specifically means no ModKit.dll instance in this same
    // process has created it yet (e.g. zero mods, or ModKit hasn't
    // finished its own startup) — expected, not an error.
    inline bool ReadRawEntries(std::unordered_map<std::string, std::string>& outEntries) {
        outEntries.clear();
        HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, kMapName);
        if (!hMap) return false;

        void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(kTotalSize));
        if (!view) { CloseHandle(hMap); return false; }

        const char* base = reinterpret_cast<const char*>(view);
        for (int i = 0; i < kMaxEntries; ++i) {
            long entryOffset = kHeaderSize + static_cast<long>(i) * kEntrySize;
            bool used = base[entryOffset + kKeyLen + kValLen] != 0;
            if (!used) continue;

            std::string key = ReadNullTerminatedAscii(base + entryOffset, kKeyLen);
            std::string value = ReadNullTerminatedAscii(base + entryOffset + kKeyLen, kValLen);
            if (!key.empty()) outEntries[key] = value;
        }

        UnmapViewOfFile(view);
        CloseHandle(hMap);
        return true;
    }

    // Mirrors ModSharedStatusReader.ReadAll() — every mod's mirrored status,
    // keyed by mod name. Empty map (not an error) if nothing is reporting yet.
    inline std::unordered_map<std::string, ModStatus> ReadAllModStatus() {
        std::unordered_map<std::string, ModStatus> result;
        std::unordered_map<std::string, std::string> raw;
        if (!ReadRawEntries(raw)) return result;

        auto dirIt = raw.find("MODS");
        if (dirIt == raw.end() || dirIt->second.empty()) return result;

        for (const auto& modName : SplitNonEmpty(dirIt->second, ',')) {
            ModStatus status;
            if (auto it = raw.find(modName + "#H"); it != raw.end()) status.hooked = it->second == "1";
            if (auto it = raw.find(modName + "#O"); it != raw.end()) status.outdated = it->second == "1";
            if (auto it = raw.find(modName + "#B"); it != raw.end()) status.busy = it->second == "1";
            if (auto it = raw.find(modName + "#F"); it != raw.end() && !it->second.empty())
                status.activeSubFlags = SplitNonEmpty(it->second, ';');
            if (auto it = raw.find(modName + "#G"); it != raw.end() && !it->second.empty())
                status.activeFeatureLabels = SplitNonEmpty(it->second, ';');
            if (auto it = raw.find(modName + "#P"); it != raw.end() && !it->second.empty())
                status.pendingFeatureLabels = SplitNonEmpty(it->second, ';');
            result[modName] = std::move(status);
        }
        return result;
    }

    // Mirrors ModSharedStatusReader.TryReadPoolInfo().
    inline bool TryReadPoolInfo(int& outMethod, long long& outSizeBytes) {
        outMethod = 0; outSizeBytes = 0;
        std::unordered_map<std::string, std::string> raw;
        if (!ReadRawEntries(raw)) return false;

        auto it = raw.find("__POOL__");
        if (it == raw.end()) return false;

        auto parts = SplitNonEmpty(it->second, '|');
        if (parts.size() < 2) return false;
        try {
            outMethod = std::stoi(parts[0]);
            outSizeBytes = std::stoll(parts[1]);
        }
        catch (...) {
            return false;
        }
        return true;
    }

    // MonoKit's own class-pool stats — same "__MONOPOOL__" key MonoKit.h's
    // PublishPoolStats() writes via ModKit_SetSharedData, mirroring
    // TryReadPoolInfo above exactly but for MonoKit's pool instead of
    // ModKit's shared PE-cave/VirtualAlloc one. Absent entirely (returns
    // false) for any mod that never calls MonoKit::BuildAt — not just
    // "zero", genuinely not present, so callers can tell "no Mono hooks in
    // this process" apart from "Mono hooks exist but haven't allocated yet".
    inline bool TryReadMonoPoolInfo(int& outChunks, long long& outTotalBytes, long long& outUsedBytes) {
        outChunks = 0; outTotalBytes = 0; outUsedBytes = 0;
        std::unordered_map<std::string, std::string> raw;
        if (!ReadRawEntries(raw)) return false;

        auto it = raw.find("__MONOPOOL__");
        if (it == raw.end()) return false;

        auto parts = SplitNonEmpty(it->second, '|');
        if (parts.size() < 3) return false;
        try {
            outChunks = std::stoi(parts[0]);
            outTotalBytes = std::stoll(parts[1]);
            outUsedBytes = std::stoll(parts[2]);
        }
        catch (...) {
            return false;
        }
        return true;
    }

    // True if ModKit.dll has loaded in this process. Checks the actual
    // loaded module directly (GetModuleHandleA — this DLL runs inside the
    // same process as ModKit.dll, so this is a trivial, reliable,
    // same-process check) rather than the shared-memory block's
    // existence. The block is NOT a reliable proxy for "ModKit is
    // loaded": ModKit.cpp only creates it lazily, inside ModKit_Startup(),
    // which each individual MOD calls when it registers — with zero mods
    // (Force Inject ModKit alone), nothing ever calls it, so the mapping
    // genuinely never exists even though ModKit.dll itself is loaded and
    // running. Checking the mapping's existence was permanently false in
    // exactly the scenario this needed to detect.
    inline bool IsModKitPresent() {
        return GetModuleHandleA("ModKit.dll") != nullptr;
    }

} // namespace SharedDataReader
#include "protocol.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace fd {
namespace {
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}
bool Starts(const std::wstring& text, const wchar_t* prefix) {
    return text.compare(0, std::wcslen(prefix), prefix) == 0;
}
bool Ends(const std::wstring& text, const wchar_t* suffix) {
    const std::size_t n = std::wcslen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}
}

bool IsLocalTorrentFile(const std::wstring& input) {
    if (!Ends(Lower(input), L".torrent")) return false;
    if (input.find(L"://") != std::wstring::npos) return false;
    const DWORD attributes = GetFileAttributesW(input.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

Protocol Detect(const std::wstring& raw) {
    const std::wstring input = Lower(raw);
    if (Starts(input, L"https://") || Starts(input, L"http://")) return Protocol::Http;
    if (Starts(input, L"ftps://") || Starts(input, L"ftp://")) return Protocol::Ftp;
    if (Starts(input, L"magnet:?")) return Protocol::Magnet;
    if (Starts(input, L"ed2k://")) return Protocol::Ed2k;
    if (IsLocalTorrentFile(raw)) return Protocol::Torrent;
    return Protocol::Unsupported;
}

std::wstring ProtocolName(const std::wstring& url) {
    const std::wstring lower = Lower(url);
    switch (Detect(url)) {
    case Protocol::Http: return Starts(lower, L"https://") ? L"HTTPS" : L"HTTP";
    case Protocol::Ftp: return Starts(lower, L"ftps://") ? L"FTPS" : L"FTP";
    case Protocol::Magnet: return L"Magnet";
    case Protocol::Ed2k: return L"ED2K";
    case Protocol::Torrent: return L"BitTorrent";
    default: return L"Unsupported";
    }
}

std::wstring CleanFilename(const std::wstring& raw) {
    std::wstring value;
    value.reserve(std::min<std::size_t>(raw.size(), 180));
    for (wchar_t c : raw) {
        if (value.size() >= 180) break;
        value += (c < 32 || std::wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'.')) value.pop_back();
    if (value.empty() || value == L"." || value == L"..") value = L"download.bin";
    const auto dot = value.find(L'.');
    const std::wstring stem = Lower(value.substr(0, dot));
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
        stem == L"com1" || stem == L"com2" || stem == L"com3" || stem == L"com4" ||
        stem == L"com5" || stem == L"com6" || stem == L"com7" || stem == L"com8" ||
        stem == L"com9" || stem == L"lpt1" || stem == L"lpt2" || stem == L"lpt3" ||
        stem == L"lpt4" || stem == L"lpt5" || stem == L"lpt6" || stem == L"lpt7" ||
        stem == L"lpt8" || stem == L"lpt9") value.insert(0, L"_");
    return value;
}

std::wstring FilenameFromUrl(const std::wstring& url) {
    if (IsLocalTorrentFile(url)) {
        const auto slash = url.find_last_of(L"\\/");
        return CleanFilename(url.substr(slash == std::wstring::npos ? 0 : slash + 1));
    }
    if (Detect(url) == Protocol::Magnet) return L"Magnet transfer";
    if (Detect(url) == Protocol::Ed2k) return L"ED2K transfer";
    auto end = url.find_first_of(L"?#");
    std::wstring path = url.substr(0, end);
    auto slash = path.find_last_of(L"/\\");
    if (slash == std::wstring::npos || slash + 1 >= path.size()) return L"download.bin";
    return CleanFilename(path.substr(slash + 1));
}

bool IsExecutableFilename(const std::wstring& name) {
    const std::wstring lower = Lower(name);
    const wchar_t* risky[] = {
        L".exe", L".com", L".scr", L".pif", L".bat", L".cmd", L".ps1", L".vbs", L".js", L".msi"
    };
    for (auto suffix : risky) if (Ends(lower, suffix)) return true;
    return false;
}

// These helpers do not imply an internal FTP or ED2K transfer engine.
bool IsHttpUrl(const std::wstring& value) { return Detect(value) == Protocol::Http; }
bool IsTorrentSource(const std::wstring& value) { const Protocol kind = Detect(value); return kind == Protocol::Magnet || kind == Protocol::Torrent; }
bool NeedsExternalHandler(const std::wstring& value) { const Protocol kind = Detect(value); return kind == Protocol::Ftp || kind == Protocol::Ed2k; }
bool IsSupportedSource(const std::wstring& value) { return Detect(value) != Protocol::Unsupported; }
bool IsSecureHttpUrl(const std::wstring& value) { return Starts(Lower(value), L"https://"); }
bool IsMagnetUrl(const std::wstring& value) { return Detect(value) == Protocol::Magnet; }
bool IsEd2kUrl(const std::wstring& value) { return Detect(value) == Protocol::Ed2k; }
bool IsFtpUrl(const std::wstring& value) { return Detect(value) == Protocol::Ftp; }
bool IsFtpsUrl(const std::wstring& value) { return Starts(Lower(value), L"ftps://"); }

bool LaunchExternal(const std::wstring& input, std::wstring& reason) {
    const Protocol protocol = Detect(input);
    // BitTorrent and Magnet are handled inside FASTDOWN by libtorrent.
    if (protocol != Protocol::Ftp && protocol != Protocol::Ed2k) {
        reason = L"This protocol is not eligible for external handoff.";
        return false;
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", input.c_str(),
                                           nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) return true;
    reason = L"No compatible external application is registered.";
    return false;
}
}

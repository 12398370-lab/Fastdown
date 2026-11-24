#pragma once
#include <string>

namespace fd {

enum class Protocol {
    Http, Ftp, Magnet, Ed2k, Torrent, Unsupported
};

Protocol Detect(const std::wstring& raw);
std::wstring ProtocolName(const std::wstring& raw);
std::wstring FilenameFromUrl(const std::wstring& raw);
std::wstring CleanFilename(const std::wstring& raw);
bool IsExecutableFilename(const std::wstring& name);
bool LaunchExternal(const std::wstring& input, std::wstring& reason);
bool IsLocalTorrentFile(const std::wstring& input);
// Protocol classification helpers for UI filters, validation and future adapters.
bool IsHttpUrl(const std::wstring& value);
bool IsTorrentSource(const std::wstring& value);
bool NeedsExternalHandler(const std::wstring& value);
bool IsSupportedSource(const std::wstring& value);
bool IsSecureHttpUrl(const std::wstring& value);
bool IsMagnetUrl(const std::wstring& value);
bool IsEd2kUrl(const std::wstring& value);
bool IsFtpUrl(const std::wstring& value);
bool IsFtpsUrl(const std::wstring& value);

}

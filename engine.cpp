#include "engine.h"
#include "protocol.h"
#include "checkpoint.h"
#include "performance.h"
#include <curl/curl.h>
#include <winhttp.h>
#include <wininet.h>
#include <array>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/error_code.hpp>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "libcurl.lib")

namespace fd {
namespace lt = libtorrent;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::int64_t MiB = 1024LL * 1024LL;
constexpr DWORD ReadBuffer = 1024 * 1024;
constexpr int MaxAttempts = 3;

struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET handle = nullptr) : value(handle) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    explicit operator bool() const { return value != nullptr; }
};
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~FileHandle() { Close(); }
    void Close() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};
struct HeaderData {
    DWORD status = 0;
    std::int64_t length = -1;
    std::int64_t total = -1;
    std::int64_t first = -1;
    std::int64_t last = -1;
    std::wstring validator; // ETag or Last-Modified for cross-request consistency.
};

bool Terminal(State state) {
    return state == State::Complete || state == State::Failed ||
           state == State::Cancelled || state == State::HandedOff;
}

void SetState(Job& job, State next, const std::wstring& detail) {
    { std::lock_guard<std::mutex> lock(job.detailMutex); job.detail = detail; }
    job.state.store(next);
}

std::wstring WinError(const wchar_t* step, DWORD number = GetLastError()) {
    return std::wstring(step) + L" (Win32 " + std::to_wstring(number) + L")";
}

bool ParseUrl(const std::wstring& raw, UrlParts& parts) {
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof(c);
    wchar_t hostname[2048]{};
    wchar_t pathname[8192]{};
    wchar_t extra[8192]{};
    c.lpszHostName = hostname;
    c.dwHostNameLength = static_cast<DWORD>(_countof(hostname));
    c.lpszUrlPath = pathname;
    c.dwUrlPathLength = static_cast<DWORD>(_countof(pathname));
    c.lpszExtraInfo = extra;
    c.dwExtraInfoLength = static_cast<DWORD>(_countof(extra));
    if (!WinHttpCrackUrl(raw.c_str(), static_cast<DWORD>(raw.size()), 0, &c)) return false;
    if (c.nScheme != INTERNET_SCHEME_HTTP && c.nScheme != INTERNET_SCHEME_HTTPS) return false;
    if (!c.dwHostNameLength) return false;
    parts.host.assign(hostname, c.dwHostNameLength);
    parts.path.assign(pathname, c.dwUrlPathLength);
    parts.path.append(extra, c.dwExtraInfoLength);
    if (parts.path.empty()) parts.path = L"/";
    parts.port = c.nPort;
    parts.secure = (c.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

std::int64_t NumericHeader(HINTERNET request, DWORD query) {
    wchar_t text[128]{};
    DWORD size = sizeof(text);
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             text, &size, WINHTTP_NO_HEADER_INDEX)) return -1;
    wchar_t* end = nullptr;
    const auto number = _wcstoi64(text, &end, 10);
    return end != text && *end == L'\0' && number >= 0 ? number : -1;
}

bool ReadContentRange(HINTERNET request, HeaderData& result) {
    wchar_t text[256]{};
    DWORD size = sizeof(text);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Content-Range",
                             text, &size, WINHTTP_NO_HEADER_INDEX)) return false;
    if (std::wcsncmp(text, L"bytes ", 6) != 0) return false;
    wchar_t* next = nullptr;
    std::int64_t first = _wcstoi64(text + 6, &next, 10);
    if (next == text + 6 || *next != L'-') return false;
    wchar_t* after = nullptr;
    std::int64_t last = _wcstoi64(next + 1, &after, 10);
    if (after == next + 1 || *after != L'/') return false;
    if (after[1] == L'*') return false;
    wchar_t* end = nullptr;
    std::int64_t total = _wcstoi64(after + 1, &end, 10);
    if (end == after + 1 || *end != L'\0' || total <= 0 || first < 0 || last < first || last >= total) return false;
    result.first = first;
    result.last = last;
    result.total = total;
    return true;
}

// Each request owns its connection and request handles; a WinHTTP session can be shared.
// A6: small independently testable transfer-safety policies.
bool A6ValidatorMatches(const std::wstring& expected, const std::wstring& observed);
bool A6ValidResponseSize(std::int64_t expected, std::int64_t actual);
bool A6SafeFileOffset(std::int64_t offset, DWORD length);
bool A6TransferInterrupted(bool stopped, bool paused);
bool A6FtpWriteAll(HANDLE file, const unsigned char* bytes, DWORD size);
bool A6HttpRangeComplete(std::int64_t first, std::int64_t last, std::int64_t received);
DWORD A6NetworkTimeout(Mode mode);
std::size_t A6FtpBufferSize(Mode mode);
bool IsSafeRangeValidator(const std::wstring& value);
bool HasStrongValidator(const std::wstring& value);
bool IsHttpSuccessStatus(DWORD status);
bool IsCompleteRange(const HeaderData&, std::int64_t, std::int64_t);
using Sink = std::function<bool(const unsigned char*, DWORD, std::int64_t)>;
bool Fetch(const UrlParts& parts, HINTERNET session,
           std::int64_t first, std::int64_t last,
           bool validateRange, const Sink& onData, const std::atomic<bool>& stop,
           HeaderData& result, std::wstring& reason,
           HINTERNET reusableConnection = nullptr,
           const std::wstring* ifRange = nullptr) {
    // A worker owns one WinHTTP connection handle and creates a fresh request
    // per range. This permits connection reuse without sharing request handles.
    HttpHandle ownedConnection;
    if (!reusableConnection) {
        ownedConnection.value = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
        if (!ownedConnection) { reason = WinError(L"WinHttpConnect"); return false; }
    }
    const HINTERNET connection = reusableConnection ? reusableConnection : ownedConnection.value;
    const DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle request(WinHttpOpenRequest(connection, L"GET", parts.path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) { reason = WinError(L"WinHttpOpenRequest"); return false; }

    // Byte offsets must address the original representation, not gzip-compressed data.
    static constexpr wchar_t identityHeader[] = L"Accept-Encoding: identity\r\n";
    if (!WinHttpAddRequestHeaders(request.value, identityHeader, static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
        reason = WinError(L"WinHttpAddRequestHeaders(identity)");
        return false;
    }
    if (first >= 0) {
        wchar_t header[160]{};
        swprintf_s(header, L"Range: bytes=%lld-%lld", first, last);
        if (!WinHttpAddRequestHeaders(request.value, header, static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD)) {
            reason = WinError(L"WinHttpAddRequestHeaders");
            return false;
        }
    }
    if (ifRange && HasStrongValidator(*ifRange) && first >= 0) {
        // Only values read from the response header are used here. WinHTTP
        // rejects malformed header strings rather than sending them verbatim.
        const std::wstring validatorHeader = L"If-Range: " + *ifRange + L"\r\n";
        if (!WinHttpAddRequestHeaders(request.value, validatorHeader.c_str(),
                                     static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
            reason = WinError(L"WinHttpAddRequestHeaders(If-Range)");
            return false;
        }
    }
    if (stop) { reason = L"Cancelled"; return false; }
    if (!WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        reason = WinError(L"WinHttpSendRequest");
        return false;
    }
    if (!WinHttpReceiveResponse(request.value, nullptr)) {
        reason = WinError(L"WinHttpReceiveResponse");
        return false;
    }
    DWORD bytes = sizeof(result.status);
    if (!WinHttpQueryHeaders(request.value,
         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
         WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &bytes,
         WINHTTP_NO_HEADER_INDEX)) {
        reason = L"Missing HTTP status";
        return false;
    }
    wchar_t encoding[128]{};
    DWORD encodingBytes = sizeof(encoding);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_ENCODING,
                            WINHTTP_HEADER_NAME_BY_INDEX, encoding, &encodingBytes,
                            WINHTTP_NO_HEADER_INDEX) &&
        _wcsicmp(encoding, L"identity") != 0) {
        reason = L"Server sent compressed bytes despite identity request";
        return false;
    }
    result.length = NumericHeader(request.value, WINHTTP_QUERY_CONTENT_LENGTH);
    ReadContentRange(request.value, result);
    // Prefer a strong ETag. A weak ETag is not valid for If-Range.
    wchar_t validatorBuffer[512]{};
    DWORD validatorSize = sizeof(validatorBuffer);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_ETAG,
                            WINHTTP_HEADER_NAME_BY_INDEX, validatorBuffer,
                            &validatorSize, WINHTTP_NO_HEADER_INDEX) &&
        validatorBuffer[0] && std::wcsncmp(validatorBuffer, L"W/", 2) != 0) {
        result.validator = validatorBuffer;
    } else {
        validatorSize = sizeof(validatorBuffer);
        if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LAST_MODIFIED,
                                WINHTTP_HEADER_NAME_BY_INDEX, validatorBuffer,
                                &validatorSize, WINHTTP_NO_HEADER_INDEX)) {
            result.validator = validatorBuffer;
        }
    }
    // If-Range may be ignored by servers. Reject a mismatching validator
    // even if a server responds 206 to avoid silently mixing file versions.
    if (validateRange && ifRange && HasStrongValidator(*ifRange) &&
        !A6ValidatorMatches(*ifRange, result.validator)) {
        reason = L"Resource validator changed during segmented transfer";
        return false;
    }
    if (!IsHttpSuccessStatus(result.status)) {
        reason = L"HTTP " + std::to_wstring(result.status);
        return false;
    }
    if (validateRange) {
        if (!IsCompleteRange(result, first, last)) {
            reason = L"Server did not return the requested byte range";
            return false;
        }
    }
    if (!onData) return true; // Header-only probe. No unneeded body transfer.
    thread_local std::vector<unsigned char> buffer(ReadBuffer);
    // One reusable read buffer per worker avoids per-segment heap allocations.
    std::int64_t position = first >= 0 ? first : 0;
    for (;;) {
        if (stop) { reason = L"Cancelled"; return false; }
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &read)) {
            reason = WinError(L"WinHttpReadData");
            return false;
        }
        if (!read) break;
        if (validateRange && (position > last ||
                              static_cast<std::int64_t>(read) > last - position + 1)) {
            reason = L"Server overran its Content-Range";
            return false;
        }
        if (!onData(buffer.data(), read, position)) {
            reason = L"File write failed or transfer stopped";
            return false;
        }
        position += read;
    }
    if (validateRange && !A6HttpRangeComplete(first, last, position - first)) {
        reason = L"Short HTTP range response";
        return false;
    }
    if (first < 0 && result.length >= 0 && !A6ValidResponseSize(result.length, position)) {
        reason = L"Short HTTP response";
        return false;
    }
    return true;
}

bool WriteAt(HANDLE file, const unsigned char* source, DWORD count, std::int64_t offset) {
    if (!A6SafeFileOffset(offset, count)) return false;
    OVERLAPPED ov{};
    const auto position = static_cast<std::uint64_t>(offset);
    ov.Offset = static_cast<DWORD>(position & 0xffffffffULL);
    ov.OffsetHigh = static_cast<DWORD>(position >> 32);
    DWORD written = 0;
    BOOL success = WriteFile(file, source, count, &written, &ov);
    if (!success && GetLastError() == ERROR_IO_PENDING)
        success = GetOverlappedResult(file, &ov, &written, TRUE);
    return success && written == count;
}

bool WaitUntilRunning(Job& job) {
    if (!job.paused) return !job.stopping;
    std::unique_lock<std::mutex> lock(job.pauseMutex);
    job.pauseCondition.wait(lock, [&job] { return !job.paused || job.stopping; });
    return !job.stopping;
}

// A17: pause-aware retry backoff; condition-variable wake on Pause/Resume/Cancel.
// Only full, uninterrupted slices consume retry budget. The residual <=50 ms is safe.
bool RetryDelay(Job& job, int milliseconds) {
    unsigned elapsed = 0;
    const unsigned delay = static_cast<unsigned>((std::max)(milliseconds, 0));
    while (elapsed < delay) {
        if (!WaitUntilRunning(job)) return false;
        const unsigned step = performance::BackoffSleepSliceMs(delay - elapsed);
        std::unique_lock<std::mutex> lock(job.pauseMutex);
        const bool interrupted = job.pauseCondition.wait_for(lock,
            std::chrono::milliseconds(step), [&job] {
                return job.stopping.load() || job.paused.load();
            });
        if (job.stopping.load()) return false;
        if (!interrupted && performance::BackoffClockShouldAdvance(
                job.paused.load(), job.stopping.load())) elapsed += step;
    }
    return !job.stopping.load();
}

// Eight small, testable transfer-policy helpers keep decisions centralized.
bool IsRangeResponse(const HeaderData& h) {
    return h.status == 206 && h.total > 0 && h.first == 0 && h.last == 0;
}
bool IsCompleteTransfer(std::int64_t expected, std::int64_t actual) {
    return expected <= 0 || expected == actual;
}
std::int64_t CeilingDiv(std::int64_t n, std::int64_t d) {
    return n / d + (n % d != 0 ? 1 : 0);
}
int ClampWorkerCount(int desired, std::int64_t segments) {
    return static_cast<int>(std::min<std::int64_t>(desired, std::max<std::int64_t>(1, segments)));
}
bool ShouldUseRanges(const HeaderData& h) { return IsRangeResponse(h); }
int RetryBackoffMs(int attempt) { return std::min(2000, 300 * (attempt + 1)); }
std::int64_t SegmentEnd(std::int64_t start, std::int64_t chunk, std::int64_t total) {
    return start + std::min(chunk, total - start) - 1;
}
bool IsSafeRangeValidator(const std::wstring& value) {
    return !value.empty() && value.size() < 500 &&
        value.find(L'\r') == std::wstring::npos &&
        value.find(L'\n') == std::wstring::npos;
}

// A4 transfer policy helpers. They are deliberately side-effect free so their
// invariants can be exercised independently of WinHTTP and libtorrent.
bool IsHttpSuccessStatus(DWORD status) { return status == 200 || status == 206; }
bool IsCompleteRange(const HeaderData& h, std::int64_t first, std::int64_t last) {
    return h.status == 206 && h.first == first && h.last == last && h.total > last;
}
bool IsValidLength(std::int64_t length) { return length >= 0; }
bool ShouldRetryAttempt(int attempt, int limit, bool stopped) {
    return !stopped && attempt + 1 < limit;
}
int BoundedHttpWorkers(int requested, std::int64_t segments) {
    return std::clamp(ClampWorkerCount(requested, segments), 1, 24);
}
std::int64_t RemainingSegmentBytes(std::int64_t total, std::int64_t start) {
    return start >= total ? 0 : total - start;
}
bool HasStrongValidator(const std::wstring& value) {
    return IsSafeRangeValidator(value) && value.compare(0, 2, L"W/") != 0;
}
// A6 helpers: enforce byte integrity without forcing additional allocations.
bool A6ValidatorMatches(const std::wstring& expected, const std::wstring& observed) {
    // Some servers omit the validator on 206 responses. If they provide one,
    // it must match the original probe; If-Range supplies the other safeguard.
    return observed.empty() || expected == observed;
}
bool A6ValidResponseSize(std::int64_t expected, std::int64_t actual) {
    return expected >= 0 && actual == expected;
}
bool A6SafeFileOffset(std::int64_t offset, DWORD length) {
    return offset >= 0 && length > 0 &&
        offset <= (std::numeric_limits<std::int64_t>::max)() - length;
}
bool A6TransferInterrupted(bool stopped, bool paused) {
    return stopped || paused;
}
bool A6FtpWriteAll(HANDLE file, const unsigned char* bytes, DWORD size) {
    DWORD offset = 0;
    while (offset < size) {
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, size - offset, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}
bool A6HttpRangeComplete(std::int64_t first, std::int64_t last, std::int64_t received) {
    return first >= 0 && last >= first && received == last - first + 1;
}
DWORD A6NetworkTimeout(Mode mode) {
    return mode == Mode::Eco ? 45000u : mode == Mode::Balanced ? 35000u : 25000u;
}
std::size_t A6FtpBufferSize(Mode mode) {
    return mode == Mode::Eco ? 64u * 1024u :
           mode == Mode::Balanced ? 256u * 1024u : 1024u * 1024u;
}

bool ShouldPublishTransfer(bool stopped, bool failed, std::int64_t expected, std::int64_t actual) {
    return !stopped && !failed && IsCompleteTransfer(expected, actual);
}

// Limit requests on small objects, while allowing independent I/O on large objects.
std::int64_t SegmentBytes(Mode mode, std::int64_t total) {
    if (total <= 32 * MiB) return 4 * MiB;
    if (mode == Mode::Eco) return 16 * MiB;
    if (mode == Mode::Balanced) return 8 * MiB;
    return total >= 512 * MiB ? 16 * MiB : 8 * MiB;
}

int MaxActiveJobs(Mode mode) {
    switch (mode) {
    case Mode::Eco: return 1;
    case Mode::Balanced: return 3;
    case Mode::Turbo: return 6;
    case Mode::Maximum: return 8;
    }
    return 2;
}

int MaxSegments(Mode mode, std::int64_t total) {
    if (total <= 4 * MiB) return 1;
    int limit = 1;
    switch (mode) {
    case Mode::Eco: limit = 2; break;
    case Mode::Balanced: limit = 8; break;
    case Mode::Turbo: limit = 16; break;
    case Mode::Maximum: limit = 24; break;
    }
    // Network transfers are I/O-bound; do not artificially limit them to CPU count.
    const auto chunk = SegmentBytes(mode, total);
    const int sizeLimit = static_cast<int>(std::min<std::int64_t>(32,
        total / chunk + (total % chunk != 0 ? 1 : 0)));
    return std::max(1, std::min(limit, sizeLimit));
}

std::wstring PartName(const Job& job) {
    return job.destination + L".fastdown." + std::to_wstring(job.id) + L".part";
}

bool IsFilePresent(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
}

// BitTorrent mode policies: bounds prevent excessive peers and scheduler overhead.
namespace {
int TorrentPeerLimit(Mode mode) {
    switch (mode) {
    case Mode::Eco: return 70;
    case Mode::Balanced: return 160;
    case Mode::Turbo: return 300;
    case Mode::Maximum: return 450;
    }
    return 160;
}
int TorrentUnchokeSlots(Mode mode) {
    switch (mode) {
    case Mode::Eco: return 4;
    case Mode::Balanced: return 8;
    case Mode::Turbo: return 16;
    case Mode::Maximum: return 24;
    }
    return 8;
}
int TorrentActiveDownloads(Mode mode) {
    switch (mode) {
    case Mode::Eco: return 1;
    case Mode::Balanced: return 3;
    case Mode::Turbo: return 6;
    case Mode::Maximum: return 8;
    }
    return 3;
}
lt::settings_pack TorrentModeSettings(Mode mode) {
    lt::settings_pack settings;
    settings.set_int(lt::settings_pack::connections_limit, TorrentPeerLimit(mode));
    settings.set_int(lt::settings_pack::unchoke_slots_limit, TorrentUnchokeSlots(mode));
    settings.set_int(lt::settings_pack::active_downloads, TorrentActiveDownloads(mode));
    settings.set_int(lt::settings_pack::active_limit, TorrentActiveDownloads(mode) + 4);
    return settings;
}
// Transfer-size-aware HTTP bounds: no pointless 24-way concurrency for tiny files.
int EffectiveHttpWorkers(Mode mode, std::int64_t bytes) {
    if (bytes <= 0) return 1;
    return ClampWorkerCount(MaxSegments(mode, bytes), CeilingDiv(bytes, SegmentBytes(mode, bytes)));
}
// Exponential capped retry backoff with deterministic job-specific jitter.
int TransferBackoffMs(int attempt, unsigned jobId) {
    const int capped = std::min(4, std::max(0, attempt));
    return std::min(3000, (250 << capped) + static_cast<int>(jobId % 113));
}
// A stalled or cancelled job must never occupy a queue slot indefinitely.
bool IsJobInterrupted(const Job& job) { return job.stopping.load(); }
// The torrent engine is shared, so only per-session settings are modified.
bool HasTorrentSession(const std::unique_ptr<lt::session>& session) { return static_cast<bool>(session); }
}

struct Manager::TorrentRecord {
    unsigned id = 0;
    lt::torrent_handle handle;
};

Manager::Manager() {
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:0");
    settings.set_int(lt::settings_pack::connections_limit, TorrentPeerLimit(mode_.load()));
    settings.set_int(lt::settings_pack::unchoke_slots_limit, TorrentUnchokeSlots(mode_.load()));
    settings.set_int(lt::settings_pack::active_downloads, TorrentActiveDownloads(mode_.load()));
    settings.set_int(lt::settings_pack::active_limit, TorrentActiveDownloads(mode_.load()) + 4);
    torrentSession_ = std::make_unique<lt::session>(settings);
}

Job::Job(unsigned number, std::wstring source, std::wstring target)
    : id(number), url(std::move(source)), destination(std::move(target)) {}

Job::~Job() {
    stopping = true;
    paused = false;
    pauseCondition.notify_all();
    if (worker.joinable()) worker.join();
}

Manager::~Manager() {
    CancelAll();
    std::vector<std::shared_ptr<Job>> copy;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        copy.swap(jobs_);
    }
    for (const auto& job : copy)
        if (job->worker.joinable()) job->worker.join();
    copy.clear(); // Join every worker before releasing its last shared_ptr.
    {
        std::lock_guard<std::mutex> lock(torrentMutex_);
        for (auto& item : torrents_) {
            if (item && item->handle.is_valid() && torrentSession_)
                torrentSession_->remove_torrent(item->handle);
        }
        torrents_.clear();
    }
    torrentSession_.reset();
}

std::shared_ptr<Job> Manager::Find(unsigned id) const {
    std::lock_guard<std::mutex> lock(jobsMutex_);
    for (const auto& job : jobs_) if (job->id == id) return job;
    return {};
}

bool Manager::HasActiveDuplicate(const std::wstring& url, const std::wstring& path) const {
    std::lock_guard<std::mutex> lock(jobsMutex_);
    for (const auto& job : jobs_)
        if (!Terminal(job->state) && job->url == url && job->destination == path)
            return true;
    return false;
}

unsigned Manager::Add(const std::wstring& url, const std::wstring& destination) {
    if (url.empty()) return 0;
    const Protocol protocol = Detect(url);
    if (protocol == Protocol::Unsupported) return 0;
    if (protocol == Protocol::Http && destination.empty()) return 0;
    if ((protocol == Protocol::Magnet || protocol == Protocol::Torrent) && destination.empty()) return 0;
    if (protocol == Protocol::Torrent && !IsLocalTorrentFile(url)) return 0;
    if (protocol == Protocol::Magnet && url.rfind(L"magnet:?", 0) != 0) return 0;

    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        for (const auto& existing : jobs_)
            if (!Terminal(existing->state) && existing->url == url &&
                existing->destination == destination) return 0;
        job = std::make_shared<Job>(nextID_.fetch_add(1), url, destination);
        jobs_.push_back(job);
    }
    try {
        job->worker = std::thread([this, job] { Run(job); });
    } catch (...) {
        SetState(*job, State::Failed, L"Unable to create background transfer thread");
    }
    return job->id;
}

bool Manager::AcquireSlot(const std::shared_ptr<Job>& job) {
    std::unique_lock<std::mutex> lock(slotMutex_);
    slotCondition_.wait(lock, [&] {
        return job->stopping || (!job->paused && runningTransfers_ < MaxActiveJobs(mode_));
    });
    if (job->stopping) return false;
    ++runningTransfers_;
    return true;
}

void Manager::ReleaseSlot() {
    {
        std::lock_guard<std::mutex> lock(slotMutex_);
        if (runningTransfers_ > 0) --runningTransfers_;
    }
    slotCondition_.notify_all();
}

void Manager::Run(const std::shared_ptr<Job>& job) {
    const Protocol protocol = Detect(job->url);
    if (protocol == Protocol::Magnet || protocol == Protocol::Torrent) {
        if (!AcquireSlot(job)) {
            SetState(*job, State::Cancelled, L"Cancelled before BitTorrent started");
            return;
        }
        try {
            RunTorrent(job);
        } catch (const std::exception&) {
            RemoveTorrent(job->id);
            SetState(*job, State::Failed, L"BitTorrent engine exception");
        } catch (...) {
            RemoveTorrent(job->id);
            SetState(*job, State::Failed, L"Unknown BitTorrent engine error");
        }
        ReleaseSlot();
        return;
    }
    if (protocol == Protocol::Ftp && IsFtpsUrl(job->url)) {
        if (!AcquireSlot(job)) {
            SetState(*job, State::Cancelled, L"Cancelled before FTPS started");
            return;
        }
        try { RunFtps(job); }
        catch (...) { SetState(*job, State::Failed, L"FTPS transfer engine exception"); }
        ReleaseSlot();
        return;
    }
    if (protocol == Protocol::Ftp && !IsFtpsUrl(job->url)) {
        if (!AcquireSlot(job)) { SetState(*job, State::Cancelled, L"Cancelled before FTP started"); return; }
        try { RunFtp(job); }
        catch (...) { SetState(*job, State::Failed, L"FTP transfer exception"); }
        ReleaseSlot();
        return;
    }
    if (protocol == Protocol::Ftp || protocol == Protocol::Ed2k) {
        if (IsJobInterrupted(*job)) { SetState(*job, State::Cancelled, L"Cancelled"); return; }
        std::wstring reason;
        if (LaunchExternal(job->url, reason))
            SetState(*job, State::HandedOff, L"Opened in an external protocol client");
        else
            SetState(*job, State::Failed, reason);
        return;
    }
    if (protocol != Protocol::Http) {
        SetState(*job, State::Failed, L"Unsupported URL scheme");
        return;
    }
    if (!AcquireSlot(job)) {
        SetState(*job, State::Cancelled, L"Cancelled before transfer started");
        return;
    }
    try {
        RunHttp(job);
    } catch (const std::exception&) {
        DeleteFileW(PartName(*job).c_str());
        SetState(*job, State::Failed, L"Unhandled download engine exception");
    } catch (...) {
        DeleteFileW(PartName(*job).c_str());
        SetState(*job, State::Failed, L"Unknown download engine error");
    }
    ReleaseSlot();
}

// A5: native FTP adapter. Explicit FTPS remains delegated to an external client:
// WinINet FTP APIs do not provide an equivalent authenticated FTPS engine.
namespace {
struct InternetHandle {
    HINTERNET value = nullptr;
    explicit InternetHandle(HINTERNET h = nullptr) : value(h) {}
    ~InternetHandle() { if (value) InternetCloseHandle(value); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    explicit operator bool() const { return value != nullptr; }
};
bool ParseFtpAddress(const std::wstring& input, std::wstring& host,
                     std::wstring& path, INTERNET_PORT& port) {
    if (input.size() > 8192 || !IsFtpUrl(input) || IsFtpsUrl(input)) return false;
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    wchar_t hostBuffer[2048]{};
    wchar_t pathBuffer[8192]{};
    wchar_t extraBuffer[1024]{};
    components.lpszHostName = hostBuffer;
    components.dwHostNameLength = static_cast<DWORD>(std::size(hostBuffer));
    components.lpszUrlPath = pathBuffer;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuffer));
    components.lpszExtraInfo = extraBuffer;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extraBuffer));
    if (!InternetCrackUrlW(input.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_FTP ||
        components.dwHostNameLength == 0 || components.dwUrlPathLength == 0 ||
        components.dwUserNameLength != 0 || components.dwPasswordLength != 0 ||
        components.dwExtraInfoLength != 0) return false;
    host.assign(hostBuffer, components.dwHostNameLength);
    path.assign(pathBuffer, components.dwUrlPathLength);
    port = components.nPort ? components.nPort : INTERNET_DEFAULT_FTP_PORT;
    return path.size() > 1 && path.back() != L'/';
}
std::size_t FtpBufferBytes(Mode mode) {
    switch (mode) {
    case Mode::Eco: return 64 * 1024;
    case Mode::Balanced: return 256 * 1024;
    case Mode::Turbo: return 512 * 1024;
    case Mode::Maximum: return 1024 * 1024;
    }
    return 256 * 1024;
}
bool FtpMayPublish(bool cancelled, bool complete, std::int64_t expected,
                   std::int64_t actual) {
    return !cancelled && complete && (expected < 0 || expected == actual);
}
} // namespace

void Manager::RunFtp(const std::shared_ptr<Job>& job) {
    if (IsExecutableFilename(FilenameFromUrl(job->url))) {
        SetState(*job, State::Failed, L"Executable and script downloads are blocked by default");
        return;
    }
    std::wstring host, remote;
    INTERNET_PORT port = 0;
    if (!ParseFtpAddress(job->url, host, remote, port)) {
        SetState(*job, State::Failed, L"Invalid FTP URL (anonymous FTP only; no URL credentials)");
        return;
    }
    if (IsFilePresent(job->destination)) {
        SetState(*job, State::Failed, L"Destination exists; overwrite prevented");
        return;
    }
    const std::wstring temporary = PartName(*job);
    FileHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        SetState(*job, State::Failed, WinError(L"Cannot create FTP temporary file"));
        return;
    }
    SetState(*job, State::Probing, L"Connecting to FTP server");
    InternetHandle internet(InternetOpenW(L"FASTDOWN1.0 FTP", INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0));
    if (!internet) {
        file.Close(); DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"InternetOpenW failed");
        return;
    }
    DWORD timeout = A6NetworkTimeout(mode_.load());
    InternetSetOptionW(internet.value, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(internet.value, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(internet.value, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetHandle connection(InternetConnectW(internet.value, host.c_str(), port,
        L"anonymous", L"fastdown@example.invalid", INTERNET_SERVICE_FTP,
        INTERNET_FLAG_PASSIVE, 0));
    if (!connection) {
        file.Close(); DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"FTP server connection failed");
        return;
    }
    InternetHandle remoteFile(FtpOpenFileW(connection.value, remote.c_str(), GENERIC_READ,
        FTP_TRANSFER_TYPE_BINARY | INTERNET_FLAG_RELOAD, 0));
    if (!remoteFile) {
        file.Close(); DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"FTP remote file could not be opened");
        return;
    }
    DWORD high = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD low = FtpGetFileSize(remoteFile.value, &high);
    const DWORD sizeError = GetLastError();
    const std::int64_t expected = (low == INVALID_FILE_SIZE && sizeError != ERROR_SUCCESS)
        ? -1 : static_cast<std::int64_t>((static_cast<std::uint64_t>(high) << 32) | low);
    if (expected >= 0) job->totalBytes = expected;
    SetState(*job, job->paused ? State::Paused : State::Downloading,
             L"FTP binary stream (anonymous, passive mode)");
    std::vector<unsigned char> buffer(A6FtpBufferSize(mode_.load()));
    bool complete = false;
    DWORD lastError = ERROR_SUCCESS;
    while (!job->stopping) {
        if (A6TransferInterrupted(job->stopping.load(), job->paused.load()) &&
            !WaitUntilRunning(*job)) break;
        if (!WaitUntilRunning(*job)) break;
        DWORD read = 0;
        if (!InternetReadFile(remoteFile.value, buffer.data(),
                              static_cast<DWORD>(buffer.size()), &read)) {
            lastError = GetLastError();
            break;
        }
        if (read == 0) { complete = true; break; }
        if (!A6FtpWriteAll(file.value, buffer.data(), read)) {
            lastError = GetLastError();
            break;
        }
        job->completeBytes.fetch_add(read);
    }
    if (remoteFile.value) InternetCloseHandle(remoteFile.value);
    remoteFile.value = nullptr;
    const std::int64_t received = job->completeBytes.load();
    if (expected < 0 && complete) job->totalBytes = received;
    const bool valid = FtpMayPublish(job->stopping.load(), complete, expected, received);
    const bool flushed = valid && FlushFileBuffers(file.value) != FALSE;
    file.Close();
    if (!flushed) {
        DeleteFileW(temporary.c_str());
        if (job->stopping) SetState(*job, State::Cancelled, L"FTP transfer cancelled");
        else SetState(*job, State::Failed, complete
            ? L"FTP file-size verification or disk flush failed"
            : L"FTP read/write failed (Win32 " + std::to_wstring(lastError) + L")");
        return;
    }
    if (!MoveFileExW(temporary.c_str(), job->destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"FTP final file publication failed");
        return;
    }
    SetState(*job, State::Complete, L"FTP download completed and flushed");
}

// A7: native encrypted FTPS using libcurl + the Windows Schannel/OpenSSL
// backend provided by vcpkg. This is not ED2K and does not bypass TLS checks.
namespace {
std::string ToUtf8Url(const std::wstring& wide) {
    if (wide.empty() || wide.size() > 32768) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr) != size)
        return {};
    return result;
}
struct CurlFileSink {
    Job& job;
    HANDLE file;
    std::int64_t received = 0;
    bool diskFailed = false;
};
size_t CurlFtpWrite(char* source, size_t size, size_t count, void* context) {
    if (!size || !count) return 0;
    if (count > (std::numeric_limits<size_t>::max)() / size) return 0;
    const size_t bytes = size * count;
    auto& sink = *static_cast<CurlFileSink*>(context);
    if (!WaitUntilRunning(sink.job)) return 0;
    size_t consumed = 0;
    while (consumed < bytes) {
        if (sink.job.stopping) return 0;
        const DWORD step = static_cast<DWORD>((std::min)(bytes - consumed,
            static_cast<size_t>(512u * 1024u)));
        if (!A6FtpWriteAll(sink.file,
            reinterpret_cast<const unsigned char*>(source + consumed), step)) {
            sink.diskFailed = true;
            return 0;
        }
        consumed += step;
    }
    if (!performance::SafeTransferIncrement(sink.received, bytes)) {
        sink.diskFailed = true; // Reject counter overflow rather than publishing a corrupt size.
        return 0;
    }
    sink.received = performance::TransferIncrement(sink.received, bytes);
    sink.job.completeBytes.store(sink.received);
    return bytes;
}
int CurlFtpProgress(void* context, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) {
    auto& sink = *static_cast<CurlFileSink*>(context);
    if (total > 0 && total <= (std::numeric_limits<std::int64_t>::max)())
        sink.job.totalBytes.store(static_cast<std::int64_t>(total));
    // libcurl progress callback also fires while the server is idle.
    // Honor Pause without spinning, and unblock immediately on Cancel.
    return WaitUntilRunning(sink.job) ? 0 : 1;
}
}

void Manager::RunFtps(const std::shared_ptr<Job>& job) {
    if (IsExecutableFilename(FilenameFromUrl(job->url))) {
        SetState(*job, State::Failed, L"Executable and script downloads are blocked by default");
        return;
    }
    if (IsFilePresent(job->destination)) {
        SetState(*job, State::Failed, L"Destination exists; overwrite prevented");
        return;
    }
    const std::string source = ToUtf8Url(job->url);
    if (source.empty() || !IsFtpsUrl(job->url)) {
        SetState(*job, State::Failed, L"Invalid FTPS source URL");
        return;
    }
    static const CURLcode globalStatus = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (globalStatus != CURLE_OK) {
        SetState(*job, State::Failed, L"libcurl TLS backend initialization failed");
        return;
    }
    const std::wstring temporary = PartName(*job);
    FileHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        SetState(*job, State::Failed, WinError(L"Cannot create FTPS temporary file"));
        return;
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        file.Close(); DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"libcurl could not create FTPS transfer");
        return;
    }
    CurlFileSink sink{*job, file.value};
    const long timeoutSeconds = static_cast<long>(performance::ConnectTimeoutSeconds(static_cast<unsigned>(mode_.load())));
    curl_easy_setopt(curl.get(), CURLOPT_URL, source.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "ftps");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    // FTPS transfers run on dedicated worker threads. Keepalives help detect stale sockets.
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FTP_SKIP_PASV_IP, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, static_cast<long>(performance::FtpLowSpeedSeconds(static_cast<unsigned>(mode_.load()))));
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, static_cast<long>(performance::FtpLowSpeedBytes(static_cast<unsigned>(mode_.load()))));
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl.get(), CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl.get(), CURLOPT_FTP_USE_EPSV, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FTP_RESPONSE_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &CurlFtpWrite);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &CurlFtpProgress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "FASTDOWN1.0/17");
    curl_easy_setopt(curl.get(), CURLOPT_BUFFERSIZE, static_cast<long>(performance::CurlReadBufferBytes(static_cast<unsigned>(mode_.load()))));
    SetState(*job, job->paused ? State::Paused : State::Downloading,
        L"FTPS encrypted file transfer (TLS certificate verified)");
    // Retry only transient network failures. Never retry TLS verification or
    // disk-write errors. Restart the temporary file from byte zero to avoid
    // concatenating a partial first attempt with a complete second attempt.
    CURLcode result = CURLE_OK;
    for (unsigned attempt = 0;; ++attempt) {
        if (attempt != 0) {
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(file.value, zero, nullptr, FILE_BEGIN) ||
                !SetEndOfFile(file.value)) {
                sink.diskFailed = true;
                break;
            }
            sink.received = 0;
            job->completeBytes.store(0);
            curl_easy_setopt(curl.get(), CURLOPT_FRESH_CONNECT, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_FORBID_REUSE, 1L);
        }
        result = curl_easy_perform(curl.get());
        long ftpReply = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &ftpReply);
        const unsigned mode = static_cast<unsigned>(mode_.load());
        const bool canRetry = performance::RetryableFtpsResponse(
            static_cast<int>(result), ftpReply, attempt, mode,
            job->stopping.load(), sink.diskFailed);
        if (!canRetry) break;
        const unsigned delay = performance::EffectiveRetryDelay(attempt, mode, job->id);
        unsigned elapsed = 0;
        while (elapsed < delay && !job->stopping.load()) {
            // Do not consume retry backoff while paused; cancellation stays responsive.
            if (!WaitUntilRunning(*job)) break;
            const unsigned step = performance::BoundedRetrySleep(delay - elapsed, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            elapsed = performance::AdvanceRetryClock(elapsed, step, job->paused.load(), job->stopping.load());
        }
        if (job->stopping.load()) break;
    }
    long finalFtpReply = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &finalFtpReply);
    curl_off_t remoteSize = -1;
    curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &remoteSize);
    const std::int64_t received = sink.received;
    const bool complete = performance::PublishAllowed(
        performance::FtpsReplyAcceptable(static_cast<int>(result), finalFtpReply), !sink.diskFailed,
        performance::FtpsFileSizeMatches(received, static_cast<std::int64_t>(remoteSize)), job->stopping.load());
    if (remoteSize >= 0) job->totalBytes.store(static_cast<std::int64_t>(remoteSize));
    else if (complete) job->totalBytes.store(received);
    const bool flushed = complete && FlushFileBuffers(file.value) != FALSE;
    file.Close();
    if (!flushed) {
        DeleteFileW(temporary.c_str());
        if (job->stopping) SetState(*job, State::Cancelled, L"FTPS transfer cancelled");
        else SetState(*job, State::Failed, sink.diskFailed ? L"FTPS disk write failed" :
            L"FTPS transfer or file verification failed (libcurl " +
            std::to_wstring(static_cast<int>(result)) + L")");
        return;
    }
    if (!MoveFileExW(temporary.c_str(), job->destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"Cannot publish downloaded FTPS file");
        return;
    }
    SetState(*job, State::Complete, L"FTPS transfer completed with TLS verification");
}

void Manager::RunTorrent(const std::shared_ptr<Job>& job) {
    if (!torrentSession_) {
        SetState(*job, State::Failed, L"Internal BitTorrent session is unavailable");
        return;
    }
    std::error_code fsError;
    std::filesystem::create_directories(std::filesystem::path(job->destination), fsError);
    if (fsError) {
        SetState(*job, State::Failed, L"Cannot create BitTorrent destination directory");
        return;
    }
    auto toUtf8 = [](const std::wstring& value) -> std::string {
        if (value.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<std::size_t>(n), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), out.data(), n, nullptr, nullptr) != n) return {};
        return out;
    };
    const std::string savePath = toUtf8(job->destination);
    if (savePath.empty()) {
        SetState(*job, State::Failed, L"Invalid destination path encoding");
        return;
    }
    lt::error_code error;
    lt::add_torrent_params params;
    const Protocol protocol = Detect(job->url);
    if (protocol == Protocol::Magnet) {
        const std::string magnet = toUtf8(job->url);
        if (magnet.empty()) {
            SetState(*job, State::Failed, L"Invalid Magnet URI encoding");
            return;
        }
        params = lt::parse_magnet_uri(magnet, error);
        if (error) {
            SetState(*job, State::Failed, L"Magnet parsing failed");
            return;
        }
    } else if (protocol == Protocol::Torrent) {
        if (!IsLocalTorrentFile(job->url)) {
            SetState(*job, State::Failed, L"Torrent file no longer exists");
            return;
        }
        const std::string torrentPath = toUtf8(job->url);
        if (torrentPath.empty()) {
            SetState(*job, State::Failed, L"Invalid torrent path encoding");
            return;
        }
        params.ti = std::make_shared<lt::torrent_info>(torrentPath, error);
        if (error || !params.ti) {
            SetState(*job, State::Failed, L"Torrent metadata could not be parsed");
            return;
        }
    } else {
        SetState(*job, State::Failed, L"Unsupported BitTorrent source");
        return;
    }
    params.save_path = savePath;
    if (job->stopping) {
        SetState(*job, State::Cancelled, L"Cancelled before adding torrent");
        return;
    }
    lt::torrent_handle handle = torrentSession_->add_torrent(std::move(params), error);
    if (error || !handle.is_valid()) {
        SetState(*job, State::Failed, L"Could not add torrent to internal session");
        return;
    }
    auto record = std::make_shared<TorrentRecord>();
    record->id = job->id;
    record->handle = handle;
    {
        std::lock_guard<std::mutex> lock(torrentMutex_);
        torrents_.push_back(record);
    }
    if (job->paused) handle.pause();
    SetState(*job, job->paused ? State::Paused : State::Probing,
             L"Torrent added to FASTDOWN internal BitTorrent session");
    bool completed = false;
    bool handlePaused = job->paused.load();
    while (!job->stopping) {
        if (!handle.is_valid()) break;
        if (job->paused) {
            if (!handlePaused) { handle.pause(); handlePaused = true; }
            job->state = State::Paused;
            std::unique_lock<std::mutex> lock(job->pauseMutex);
            job->pauseCondition.wait_for(lock, std::chrono::milliseconds(300), [&] {
                return !job->paused.load() || job->stopping.load();
            });
            continue;
        }
        if (handlePaused) { handle.resume(); handlePaused = false; }
        const lt::torrent_status status = handle.status();
        job->completeBytes = std::max<std::int64_t>(0, status.total_wanted_done);
        job->liveBytes = 0;
        job->totalBytes = std::max<std::int64_t>(0, status.total_wanted);
        job->downloadRate = static_cast<std::uint64_t>(std::max(0, status.download_rate));
        job->uploadRate = static_cast<std::uint64_t>(std::max(0, status.upload_rate));
        job->seeds = std::max(0, status.num_seeds);
        job->peers = std::max(0, status.num_peers);
        if (status.state == lt::torrent_status::downloading_metadata ||
            status.state == lt::torrent_status::checking_files ||
            status.state == lt::torrent_status::checking_resume_data)
            job->state = State::Probing;
        else
            job->state = State::Downloading;
        {
            std::lock_guard<std::mutex> lock(job->detailMutex);
            job->detail = L"Seeds: " + std::to_wstring(job->seeds.load()) +
                L" | Peers: " + std::to_wstring(job->peers.load());
        }
        if (status.state == lt::torrent_status::finished ||
            status.state == lt::torrent_status::seeding) {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (job->stopping) {
        RemoveTorrent(job->id);
        SetState(*job, State::Cancelled, L"BitTorrent transfer cancelled");
    } else if (completed) {
        RemoveTorrent(job->id); // Stop seeding; keep downloaded data.
        SetState(*job, State::Complete, L"BitTorrent download completed");
    } else {
        RemoveTorrent(job->id);
        SetState(*job, State::Failed, L"BitTorrent handle became invalid");
    }
}

void Manager::RemoveTorrent(unsigned id) {
    std::shared_ptr<TorrentRecord> record;
    {
        std::lock_guard<std::mutex> lock(torrentMutex_);
        auto it = std::find_if(torrents_.begin(), torrents_.end(), [id](const auto& item) {
            return item && item->id == id;
        });
        if (it == torrents_.end()) return;
        record = *it;
        torrents_.erase(it);
    }
    if (torrentSession_ && record->handle.is_valid())
        torrentSession_->remove_torrent(record->handle);
}

void Manager::RunHttp(const std::shared_ptr<Job>& job) {
    if (IsExecutableFilename(FilenameFromUrl(job->url))) {
        SetState(*job, State::Failed, L"Executable and script downloads are blocked by default");
        return;
    }
    SetState(*job, State::Probing, L"Checking server range support");
    UrlParts url;
    if (!ParseUrl(job->url, url)) {
        SetState(*job, State::Failed, L"Invalid HTTP or HTTPS URL");
        return;
    }
    HttpHandle session(WinHttpOpen(L"FASTDOWN/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        SetState(*job, State::Failed, WinError(L"WinHttpOpen"));
        return;
    }
    WinHttpSetTimeouts(session.value, 12000, 15000, 30000, 30000);
    // Request enough HTTP/1.1 sockets for the selected mode. WinHTTP may enforce
    // its own connection limits; this option is best-effort, not a speed guarantee.
    const DWORD connectionLimit = static_cast<DWORD>(
        std::max(2, MaxSegments(mode_.load(), 512 * MiB)));
    WinHttpSetOption(session.value, WINHTTP_OPTION_MAX_CONNS_PER_SERVER,
                     const_cast<DWORD*>(&connectionLimit), sizeof(connectionLimit));
    DWORD redirectLimit = 8;
    WinHttpSetOption(session.value, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                     &redirectLimit, sizeof(redirectLimit));

    HeaderData probe;
    std::wstring reason;
    const bool probed = Fetch(url, session.value, 0, 0, false, {},
                               job->stopping, probe, reason);
    if (!probed) {
        SetState(*job, job->stopping ? State::Cancelled : State::Failed,
                 job->stopping ? L"Cancelled" : reason);
        return;
    }
    const bool ranged = ShouldUseRanges(probe);
    const std::int64_t total = ranged ? probe.total :
        (probe.status == 200 ? probe.length : -1);
    if (!ranged && probe.status != 200) {
        SetState(*job, State::Failed, L"Unexpected HTTP response to range probe");
        return;
    }
    job->totalBytes = std::max<std::int64_t>(0, total);
    if (job->stopping) {
        SetState(*job, State::Cancelled, L"Cancelled");
        return;
    }
    if (IsFilePresent(job->destination)) {
        SetState(*job, State::Failed, L"Destination already exists: overwrite prevented");
        return;
    }
    // A7: restart-safe checkpoints only for known-size ranged responses with
    // a strong ETag. Otherwise use the established non-resumable temporary file.
    const std::int64_t resumeChunk = ranged && total > 0
        ? SegmentBytes(mode_.load(), total) : 0;
    const std::int64_t resumeCount = resumeChunk > 0 ? CeilingDiv(total, resumeChunk) : 0;
    const bool persistentResume = ranged && total > 0 && resumeCount <= 1000000 &&
        ResumeJournal::StrongETag(probe.validator);
    const std::wstring temporary = persistentResume
        ? ResumeJournal::PartialPath(job->destination, job->url) : PartName(*job);
    const std::wstring indexPath = temporary + L".idx";
    const bool partExists = persistentResume && IsFilePresent(temporary);
    const bool indexExists = persistentResume && IsFilePresent(indexPath);
    if (persistentResume && partExists != indexExists) {
        SetState(*job, State::Failed,
            L"HTTP resume sidecars are incomplete. Move both .part and .idx files to restart.");
        return;
    }
    const bool resumeExisting = persistentResume && partExists;
    FileHandle file(CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, resumeExisting ? OPEN_EXISTING : CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
    if (!file) {
        SetState(*job, State::Failed, WinError(L"Cannot open temporary download file"));
        return;
    }
    LARGE_INTEGER currentSize{};
    if (resumeExisting && (!GetFileSizeEx(file.value, &currentSize) || currentSize.QuadPart != total)) {
        SetState(*job, State::Failed, L"HTTP resume partial file has unexpected size");
        return;
    }
    if (total > 0 && !resumeExisting) {
        LARGE_INTEGER target{};
        target.QuadPart = total;
        if (!SetFilePointerEx(file.value, target, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(file.value)) {
            file.Close();
            DeleteFileW(temporary.c_str());
            SetState(*job, State::Failed, L"Could not reserve destination size");
            return;
        }
    }
    std::unique_ptr<ResumeJournal> journal;
    if (persistentResume) {
        journal = std::make_unique<ResumeJournal>();
        std::wstring error;
        if (!journal->Open(indexPath, job->url, probe.validator, total, resumeChunk,
                           resumeExisting, error)) {
            if (!resumeExisting) { file.Close(); DeleteFileW(temporary.c_str()); }
            SetState(*job, State::Failed, error);
            return;
        }
        job->completeBytes.store(journal->CompletedBytes());
    }
    job->state = job->paused ? State::Paused : State::Downloading;
    std::atomic<bool> failed{false};
    std::mutex failureMutex;
    std::wstring firstFailure;
    auto fail = [&](const std::wstring& message) {
        failed = true;
        std::lock_guard<std::mutex> lock(failureMutex);
        if (firstFailure.empty()) firstFailure = message;
    };

    if (ranged && total > 0) {
        const std::int64_t chunk = persistentResume ? resumeChunk : SegmentBytes(mode_.load(), total);
        const std::int64_t count = CeilingDiv(total, chunk);
        const int threads = BoundedHttpWorkers(
            EffectiveHttpWorkers(mode_.load(), total), count);
        std::atomic<std::int64_t> next{0};
        // A14: admission is based on active in-flight claims, not worker indices.
        // A worker that finishes early cannot strand higher-index workers behind
        // a reduced target. All remaining workers can compete for a permit.
        const unsigned maximumWorkers = static_cast<unsigned>(threads);
        std::atomic<unsigned> activeWorkers{performance::InitialConcurrencyForMode(maximumWorkers, static_cast<unsigned>(mode_.load()))};
        std::atomic<unsigned> inFlightClaims{0};
        std::mutex admissionMutex;
        std::condition_variable admissionChanged;
        std::atomic<bool> samplingDone{false};
        std::thread sampler([&] {
            auto lastTime = Clock::now();
            // Include bytes currently being written, not only completed segments.
            // A failed segment can roll back liveBytes; reset the baseline in that case.
            auto lastBytes = performance::SampledTransferBytes(job->completeBytes.load(), job->liveBytes.load());
            std::uint64_t priorRate = 0;
            unsigned stableSamples = 0;
            unsigned adaptationCooldown = 0;
            while (!samplingDone.load() && !failed.load() && !job->stopping.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(performance::SamplerSleepMs(250)));
                if (!performance::ShouldSampleThroughput(job->paused.load(), job->stopping.load(), failed.load())) {
                    lastTime = Clock::now();
                    lastBytes = performance::SampledTransferBytes(job->completeBytes.load(), job->liveBytes.load());
                    priorRate = 0;
                    stableSamples = 0;
                    adaptationCooldown = 0;
                    continue;
                }
                const auto now = Clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
                if (!performance::SampleWindowReady(static_cast<std::uint64_t>(elapsed), 2000)) continue;
                const auto sampled = performance::SampledTransferBytes(job->completeBytes.load(), job->liveBytes.load());
                const bool valid = !performance::ThroughputCounterRolledBack(lastBytes, sampled);
                const auto delta = performance::ThroughputWindowDelta(lastBytes, sampled);
                const auto rate = performance::BytesPerSecond(delta, static_cast<std::uint64_t>(elapsed));
                stableSamples = performance::NextStableSample(stableSamples, valid && priorRate != 0, 4);
                if (!valid) adaptationCooldown = 0;
                const unsigned before = activeWorkers.load();
                if (performance::CanRetuneTarget(stableSamples, adaptationCooldown)) {
                    const unsigned proposed = performance::AdaptiveTargetAfterSample(
                        before, maximumWorkers, priorRate, rate, stableSamples, valid);
                    activeWorkers.store(proposed);
                    if (proposed != before) admissionChanged.notify_all();
                    adaptationCooldown = performance::AdaptiveCooldownAfterChange(
                        adaptationCooldown, proposed != before);
                } else {
                    adaptationCooldown = performance::AdaptiveCooldownAfterChange(adaptationCooldown, false);
                }
                priorRate = valid ? rate : 0;
                lastBytes = sampled;
                lastTime = now;
            }
        });
        std::vector<std::thread> pool;
        pool.reserve(threads);
        try {
            for (int n = 0; n < threads; ++n) {
                pool.emplace_back([&] {
                HttpHandle connection(WinHttpConnect(session.value, url.host.c_str(), url.port, 0));
                if (!connection) {
                    fail(WinError(L"WinHttpConnect(worker)"));
                    return;
                }
                while (!failed && !job->stopping) {
                    // Do not claim additional work during Pause.
                    if (!WaitUntilRunning(*job)) break;
                    if (next.load() >= count) break;
                    unsigned observed = inFlightClaims.load();
                    if (!performance::CanAdmitClaim(observed, activeWorkers.load()) ||
                        !inFlightClaims.compare_exchange_weak(observed, observed + 1)) {
                        // A17: workers sleep on a condition variable instead of polling
                        // every 25 ms. Timed wake provides a cancellation fallback.
                        std::unique_lock<std::mutex> lock(admissionMutex);
                        admissionChanged.wait_for(lock,
                            std::chrono::milliseconds(performance::HttpPermitWaitMilliseconds(job->paused.load())),
                            [&] {
                                return failed.load() ||
                                    performance::HttpPermitWakeRequired(
                                        inFlightClaims.load(), activeWorkers.load(),
                                        next.load() >= count, job->stopping.load());
                            });
                        continue;
                    }
                    const std::int64_t index = next.fetch_add(1);
                    if (index >= count) {
                        inFlightClaims.fetch_sub(1);
                        admissionChanged.notify_one();
                        break;
                    }
                    if (journal && journal->IsComplete(static_cast<std::uint64_t>(index))) {
                        inFlightClaims.fetch_sub(1);
                        admissionChanged.notify_one();
                        continue;
                    }
                    const std::int64_t start = index * chunk;
                    const std::int64_t end = SegmentEnd(start, chunk, total);
                    bool success = false;
                    for (int attempt = 0; attempt < MaxAttempts && !job->stopping; ++attempt) {
                        std::int64_t current = 0;
                        HeaderData response;
                        std::wstring localError;
                        const bool ok = Fetch(url, session.value, start, end, true,
                            [&](const unsigned char* data, DWORD amount, std::int64_t offset) {
                                if (!WaitUntilRunning(*job)) return false;
                                if (!performance::HttpSegmentWriteAllowed(current, amount, end - start + 1)) return false;
                                if (!WriteAt(file.value, data, amount, offset)) return false;
                                current += amount;
                                job->liveBytes.fetch_add(amount);
                                return true;
                            }, job->stopping, response, localError,
                            connection.value, &probe.validator);
                        job->liveBytes.fetch_sub(current);
                        if (ok) {
                            if (journal && !journal->Commit(static_cast<std::uint64_t>(index),
                                                             file.value, localError)) {
                                fail(localError);
                                break;
                            }
                            job->completeBytes.fetch_add(current);
                            success = true;
                            break;
                        }
                        // A17: do not retry permanent HTTP responses or a changed ETag.
                        const bool validatorChanged =
                            localError.find(L"validator changed") != std::wstring::npos;
                        if (performance::RetryHttpSegment(response.status,
                                static_cast<unsigned>(attempt),
                                static_cast<unsigned>(MaxAttempts),
                                job->stopping.load(), validatorChanged)) {
                            if (!RetryDelay(*job, TransferBackoffMs(attempt, job->id))) break;
                        } else if (!job->stopping) {
                            fail(localError.empty() ? L"Range transfer failed" : localError);
                        }
                    }
                    inFlightClaims.fetch_sub(1);
                    admissionChanged.notify_one();
                    if (!success && !job->stopping) break;
                }
                });
            }
        } catch (...) {
            failed = true;
            admissionChanged.notify_all();
            samplingDone = true;
            if (sampler.joinable()) sampler.join();
            for (auto& thread : pool) if (thread.joinable()) thread.join();
            throw;
        }
        for (auto& thread : pool) if (thread.joinable()) thread.join();
        samplingDone = true;
        if (sampler.joinable()) sampler.join();
    } else {
        // Unknown-size or non-range downloads use a single streaming request.
        // Retain the connection across retries to avoid unnecessary setup work.
        HttpHandle streamingConnection(WinHttpConnect(session.value, url.host.c_str(), url.port, 0));
        if (!streamingConnection) {
            fail(WinError(L"WinHttpConnect(stream)"));
        }
        bool success = false;
        for (int attempt = 0; attempt < MaxAttempts && !job->stopping && !failed; ++attempt) {
            job->completeBytes = 0;
            HeaderData response;
            std::wstring localError;
            const bool ok = Fetch(url, session.value, -1, -1, false,
                [&](const unsigned char* data, DWORD amount, std::int64_t offset) {
                    if (!WaitUntilRunning(*job)) return false;
                    if (!WriteAt(file.value, data, amount, offset)) return false;
                    job->completeBytes.fetch_add(amount);
                    return true;
                }, job->stopping, response, localError, streamingConnection.value);
            if (ok) {
                success = true;
                if (job->totalBytes == 0) job->totalBytes = job->completeBytes.load();
                break;
            }
            const bool validatorChanged =
                localError.find(L"validator changed") != std::wstring::npos;
            // A17: HTTP 200 is valid for a single-stream transfer. If its body
            // was interrupted or shorter than Content-Length, permit a retry.
            // HTTP 200 is never valid for a *segmented* request (handled above).
            const bool streamRetryable = response.status == 200u
                ? ShouldRetryAttempt(attempt, MaxAttempts, job->stopping.load())
                : performance::RetryHttpSegment(response.status,
                    static_cast<unsigned>(attempt),
                    static_cast<unsigned>(MaxAttempts),
                    job->stopping.load(), validatorChanged);
            if (streamRetryable) {
                if (!RetryDelay(*job, TransferBackoffMs(attempt, job->id))) break;
            } else if (!job->stopping) fail(localError);
        }
        if (!success && !job->stopping && !failed) fail(L"Single-stream request failed");
    }
    // No partially written file is ever published under its final filename.
    if (!ShouldPublishTransfer(job->stopping.load(), failed.load(),
                               total, job->completeBytes.load())) {
        file.Close();
        if (!persistentResume) DeleteFileW(temporary.c_str());
        if (job->stopping) SetState(*job, State::Cancelled,
            persistentResume ? L"Cancelled; verified HTTP segments retained for next launch" : L"Cancelled");
        else SetState(*job, State::Failed,
            failed ? (firstFailure.empty() ? L"Transfer failed" : firstFailure)
                   : L"Verified byte count does not match the response");
        return;
    }
    // Unknown-length transfers can shrink on a successful retry.
    const std::int64_t finalSize = job->completeBytes.load();
    LARGE_INTEGER size{};
    size.QuadPart = finalSize;
    if (!SetFilePointerEx(file.value, size, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(file.value) || !FlushFileBuffers(file.value)) {
        file.Close();
        if (!persistentResume) DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"Final size or disk flush failed");
        return;
    }
    file.Close();
    if (job->stopping) {
        if (!persistentResume) DeleteFileW(temporary.c_str());
        SetState(*job, State::Cancelled, L"Cancelled; verified ranges retained when resumable");
        return;
    }
    if (journal) journal->Close();
    if (!MoveFileExW(temporary.c_str(), job->destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (!persistentResume) DeleteFileW(temporary.c_str());
        SetState(*job, State::Failed, L"Could not publish completed file; destination may already exist");
        return;
    }
    if (persistentResume) DeleteFileW(indexPath.c_str());
    SetState(*job, State::Complete, persistentResume
        ? L"Completed; checkpoint journal verified and cleared" : L"Completed and flushed to disk");
}

bool Manager::Pause(unsigned id) {
    auto job = Find(id);
    if (!job) return false;
    const State state = job->state;
    if (state != State::Downloading && state != State::Queued && state != State::Probing) return false;
    job->paused = true;
    job->state = State::Paused;
    {
        std::lock_guard<std::mutex> lock(torrentMutex_);
        for (const auto& record : torrents_)
            if (record->id == id && record->handle.is_valid()) record->handle.pause();
    }
    job->pauseCondition.notify_all();
    slotCondition_.notify_all();
    return true;
}
bool Manager::Resume(unsigned id) {
    auto job = Find(id);
    if (!job || job->state != State::Paused) return false;
    job->paused = false;
    job->state = State::Downloading;
    {
        std::lock_guard<std::mutex> lock(torrentMutex_);
        for (const auto& record : torrents_)
            if (record->id == id && record->handle.is_valid()) record->handle.resume();
    }
    job->pauseCondition.notify_all();
    slotCondition_.notify_all();
    return true;
}
bool Manager::Cancel(unsigned id) {
    auto job = Find(id);
    if (!job || Terminal(job->state)) return false;
    job->stopping = true;
    job->paused = false;
    {
        std::lock_guard<std::mutex> lock(torrentMutex_);
        for (const auto& record : torrents_)
            if (record->id == id && record->handle.is_valid()) record->handle.pause();
    }
    job->pauseCondition.notify_all();
    slotCondition_.notify_all();
    return true;
}
void Manager::PauseAll() {
    for (const auto& job : List()) Pause(job.id);
}
void Manager::ResumeAll() {
    for (const auto& job : List()) Resume(job.id);
}
void Manager::CancelAll() {
    std::lock_guard<std::mutex> lock(jobsMutex_);
    for (auto& job : jobs_) {
        if (Terminal(job->state)) continue;
        job->stopping = true;
        job->paused = false;
        job->pauseCondition.notify_all();
    }
    slotCondition_.notify_all();
}
unsigned Manager::Retry(unsigned id) {
    const auto job = Find(id);
    if (!job || (job->state != State::Failed && job->state != State::Cancelled)) return 0;
    return Add(job->url, job->destination);
}
std::size_t Manager::RetryFailed() {
    std::vector<std::pair<std::wstring, std::wstring>> pending;
    for (const auto& job : List())
        if (job.state == State::Failed || job.state == State::Cancelled)
            pending.emplace_back(job.url, job.destination);
    std::size_t created = 0;
    for (const auto& item : pending) if (Add(item.first, item.second)) ++created;
    return created;
}
std::size_t Manager::RemoveFinished() {
    std::vector<std::shared_ptr<Job>> removed;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        auto it = std::remove_if(jobs_.begin(), jobs_.end(), [&](const auto& job) {
            if (!Terminal(job->state)) return false;
            removed.push_back(job);
            return true;
        });
        jobs_.erase(it, jobs_.end());
    }
    for (const auto& job : removed) {
        if (job->worker.joinable()) job->worker.join();
        RemoveTorrent(job->id);
    }
    return removed.size(); // Destruction and joins happen outside the mutex.
}
bool Manager::RemoveTerminal(unsigned id) {
    std::shared_ptr<Job> removed;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        const auto it = std::find_if(jobs_.begin(), jobs_.end(),
            [id](const auto& job) { return job->id == id && Terminal(job->state); });
        if (it == jobs_.end()) return false;
        removed = *it;
        jobs_.erase(it);
    }
    if (removed->worker.joinable()) removed->worker.join();
    RemoveTorrent(id);
    return true;
}
std::vector<Snapshot> Manager::List() const {
    std::vector<std::shared_ptr<Job>> copy;
    { std::lock_guard<std::mutex> lock(jobsMutex_); copy = jobs_; }
    std::vector<Snapshot> result;
    result.reserve(copy.size());
    for (const auto& job : copy) {
        Snapshot s;
        s.id = job->id;
        s.url = job->url;
        s.destination = job->destination;
        s.state = job->state.load();
        s.done = performance::ClampedSnapshotBytes(job->completeBytes.load(), job->liveBytes.load());
        s.total = job->totalBytes.load();
        s.downloadRate = job->downloadRate.load();
        s.uploadRate = job->uploadRate.load();
        s.seeds = job->seeds.load();
        s.peers = job->peers.load();
        { std::lock_guard<std::mutex> lock(job->detailMutex); s.message = job->detail; }
        result.push_back(std::move(s));
    }
    return result;
}
void Manager::SetMode(Mode value) {
    mode_ = value;
    // Keep libtorrent's shared session in sync with the selected performance mode.
    // Changing peer limits is not a promise of higher throughput: excessive peers
    // can increase CPU load and congestion on weak Wi-Fi networks.
    if (HasTorrentSession(torrentSession_)) {
        torrentSession_->apply_settings(TorrentModeSettings(value));
    }
    slotCondition_.notify_all();
}

std::wstring StateName(State state) {
    switch (state) {
    case State::Queued: return L"Queued";
    case State::Probing: return L"Connecting";
    case State::Downloading: return L"Downloading";
    case State::Paused: return L"Paused";
    case State::Complete: return L"Complete";
    case State::Failed: return L"Failed";
    case State::Cancelled: return L"Cancelled";
    case State::HandedOff: return L"External client";
    }
    return L"Unknown";
}
std::wstring ModeName(Mode mode) {
    switch (mode) {
    case Mode::Eco: return L"Eco";
    case Mode::Balanced: return L"Balanced";
    case Mode::Turbo: return L"Turbo";
    case Mode::Maximum: return L"Maximum";
    }
    return L"Balanced";
}
std::wstring HumanBytes(std::int64_t bytes) {
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    int index = 0;
    double number = static_cast<double>(std::max<std::int64_t>(0, bytes));
    while (number >= 1024.0 && index < 4) { number /= 1024.0; ++index; }
    wchar_t text[64]{};
    swprintf_s(text, L"%.1f %s", number, units[index]);
    return text;
}
std::wstring HumanRate(double bytesPerSecond) {
    if (!std::isfinite(bytesPerSecond)) bytesPerSecond = 0;
    return HumanBytes(static_cast<std::int64_t>(std::max(0.0, bytesPerSecond))) + L"/s";
}
}

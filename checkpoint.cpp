#include "checkpoint.h"
#include <algorithm>
#include <limits>

namespace fd {
namespace {
bool ExactWrite(HANDLE file, const void* buffer, DWORD bytes) {
    const auto* data = static_cast<const unsigned char*>(buffer);
    DWORD done = 0;
    while (done < bytes) {
        DWORD written = 0;
        if (!WriteFile(file, data + done, bytes - done, &written, nullptr) || written == 0)
            return false;
        done += written;
    }
    return true;
}
bool ExactRead(HANDLE file, void* buffer, DWORD bytes) {
    auto* data = static_cast<unsigned char*>(buffer);
    DWORD done = 0;
    while (done < bytes) {
        DWORD received = 0;
        if (!ReadFile(file, data + done, bytes - done, &received, nullptr) || received == 0)
            return false;
        done += received;
    }
    return true;
}
bool SeekTo(HANDLE file, std::int64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
}
} // namespace

ResumeJournal::~ResumeJournal() { Close(); }
void ResumeJournal::Close() noexcept {
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

bool ResumeJournal::StrongETag(const std::wstring& text) {
    // Strong RFC 9110 entity tag only. A weak ETag or Last-Modified header
    // does not permit automatic restart reuse of bytes from a prior session.
    if (text.size() < 2 || text.front() != L'"' || text.back() != L'"') return false;
    for (wchar_t c : text)
        if (c < 32 || c == 127 || c == L'\r' || c == L'\n') return false;
    return true;
}

std::uint64_t ResumeJournal::StableHash(const std::wstring& value) noexcept {
    // Deterministic FNV-1a over UTF-16 code units on Windows.
    std::uint64_t hash = 14695981039346656037ULL;
    for (wchar_t code : value) {
        const unsigned unit = static_cast<unsigned>(code);
        hash ^= unit & 0xffu;
        hash *= 1099511628211ULL;
        hash ^= (unit >> 8) & 0xffu;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring ResumeJournal::PartialPath(const std::wstring& destination,
                                        const std::wstring& url) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::uint64_t value = StableHash(url);
    wchar_t hex[17]{};
    for (int i = 15; i >= 0; --i) { hex[i] = digits[value & 15u]; value >>= 4u; }
    return destination + L".fastdown." + hex + L".part";
}

bool ResumeJournal::Open(const std::wstring& path, const std::wstring& source,
                         const std::wstring& etag, std::int64_t total,
                         std::int64_t chunk, bool existing, std::wstring& error) {
    Close();
    if (!StrongETag(etag) || total <= 0 || chunk <= 0) {
        error = L"Persistent HTTP resume requires a strong ETag and known size";
        return false;
    }
    const std::uint64_t count = static_cast<std::uint64_t>(total / chunk) + (total % chunk != 0);
    if (count == 0 || count > 1000000) {
        error = L"Too many segments for checkpoint journal";
        return false;
    }
    path_ = path;
    header_ = {Magic, 1, StableHash(source), StableHash(etag), total, chunk, count};
    complete_.assign(static_cast<std::size_t>(count), 0);
    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ, nullptr, existing ? OPEN_EXISTING : CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        error = L"Cannot open HTTP resume journal (Win32 " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    if (!existing) {
        if (!ExactWrite(file_, &header_, sizeof(header_)) || !FlushFileBuffers(file_)) {
            error = L"Cannot initialize HTTP checkpoint journal";
            Close();
            DeleteFileW(path.c_str());
            return false;
        }
        return true;
    }
    Header stored{};
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file_, &length) || length.QuadPart < sizeof(Header) ||
        !SeekTo(file_, 0) || !ExactRead(file_, &stored, sizeof(stored)) ||
        stored.magic != header_.magic || stored.version != header_.version ||
        stored.urlHash != header_.urlHash || stored.etagHash != header_.etagHash ||
        stored.total != header_.total || stored.chunk != header_.chunk ||
        stored.count != header_.count) {
        error = L"HTTP partial file has an incompatible checkpoint. Move or remove both sidecars.";
        Close();
        return false;
    }
    const std::int64_t bodyBytes = length.QuadPart - sizeof(Header);
    const std::int64_t records = bodyBytes / sizeof(Record);
    if (records > 1000000) {
        error = L"HTTP resume journal contains too many records";
        Close();
        return false;
    }
    for (std::int64_t i = 0; i < records; ++i) {
        Record record{};
        if (!ExactRead(file_, &record, sizeof(record)) || record.index >= count ||
            record.check != (record.index ^ header_.urlHash ^ RecordSalt)) {
            error = L"HTTP resume journal is corrupt. Move or remove both sidecars.";
            Close();
            return false;
        }
        complete_[static_cast<std::size_t>(record.index)] = 1;
    }
    // Drop a torn trailing record from a previous crash before appending.
    const std::int64_t end = sizeof(Header) + records * sizeof(Record);
    if (!SeekTo(file_, end) || !SetEndOfFile(file_)) {
        error = L"Cannot repair partial checkpoint record";
        Close();
        return false;
    }
    return true;
}

bool ResumeJournal::IsComplete(std::uint64_t index) const noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    return index < complete_.size() && complete_[static_cast<std::size_t>(index)] != 0;
}
std::int64_t ResumeJournal::CompletedBytes() const noexcept {
    std::lock_guard<std::mutex> guard(lock_);
    std::int64_t bytes = 0;
    for (std::size_t i = 0; i < complete_.size(); ++i)
        if (complete_[i]) bytes += (std::min)(header_.chunk, header_.total -
                                              static_cast<std::int64_t>(i) * header_.chunk);
    return bytes;
}

bool ResumeJournal::Commit(std::uint64_t index, HANDLE payload, std::wstring& error) {
    std::lock_guard<std::mutex> guard(lock_);
    if (index >= complete_.size() || file_ == INVALID_HANDLE_VALUE) {
        error = L"Invalid HTTP checkpoint index";
        return false;
    }
    if (complete_[static_cast<std::size_t>(index)]) return true;
    // Durability ordering: bytes -> flush payload -> append record -> flush journal.
    // Never report a range as checkpointed before its payload reaches disk.
    const Record record{index, index ^ header_.urlHash ^ RecordSalt};
    if (!FlushFileBuffers(payload) || !ExactWrite(file_, &record, sizeof(record)) ||
        !FlushFileBuffers(file_)) {
        error = L"HTTP checkpoint disk flush/write failed";
        return false;
    }
    complete_[static_cast<std::size_t>(index)] = 1;
    return true;
}
} // namespace fd

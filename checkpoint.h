#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace fd {
// Durable, append-only checkpoint journal for completed HTTP byte ranges.
// A range is recorded ONLY after its bytes have been flushed to disk.
// Resume requires a matching strong ETag, URL hash, length and segment size.
class ResumeJournal final {
public:
    ResumeJournal() = default;
    ~ResumeJournal();
    ResumeJournal(const ResumeJournal&) = delete;
    ResumeJournal& operator=(const ResumeJournal&) = delete;

    static bool StrongETag(const std::wstring& value);
    static std::uint64_t StableHash(const std::wstring& value) noexcept;
    static std::wstring PartialPath(const std::wstring& destination, const std::wstring& url);

    bool Open(const std::wstring& path, const std::wstring& source,
              const std::wstring& etag, std::int64_t total, std::int64_t chunk,
              bool existing, std::wstring& error);
    bool IsComplete(std::uint64_t index) const noexcept;
    std::int64_t CompletedBytes() const noexcept;
    bool Commit(std::uint64_t index, HANDLE payload, std::wstring& error);
    void Close() noexcept;
    const std::wstring& Path() const noexcept { return path_; }

private:
    struct Header {
        std::uint64_t magic;
        std::uint64_t version;
        std::uint64_t urlHash;
        std::uint64_t etagHash;
        std::int64_t total;
        std::int64_t chunk;
        std::uint64_t count;
    };
    struct Record { std::uint64_t index, check; };
    static_assert(sizeof(Header) == 56, "Unexpected checkpoint header layout");
    static_assert(sizeof(Record) == 16, "Unexpected checkpoint record layout");
    static constexpr std::uint64_t Magic = 0x314e574f44445346ULL; // FASTDOWN1 journal
    static constexpr std::uint64_t RecordSalt = 0x9E3779B97F4A7C15ULL;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    Header header_{};
    std::vector<unsigned char> complete_;
    mutable std::mutex lock_;
};
} // namespace fd

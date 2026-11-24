#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace libtorrent { class session; }

namespace fd {

enum class State : unsigned char {
    Queued, Probing, Downloading, Paused, Complete, Failed, Cancelled, HandedOff
};

enum class Mode : unsigned char { Eco, Balanced, Turbo, Maximum };

struct Snapshot {
    unsigned id = 0;
    std::wstring url;
    std::wstring destination;
    std::wstring message;
    State state = State::Queued;
    std::int64_t done = 0;
    std::int64_t total = 0;
    std::uint64_t downloadRate = 0;
    std::uint64_t uploadRate = 0;
    int seeds = 0;
    int peers = 0;
};

struct Job {
    const unsigned id;
    const std::wstring url;
    const std::wstring destination;
    std::atomic<State> state{State::Queued};
    std::atomic<std::int64_t> completeBytes{0};
    std::atomic<std::int64_t> liveBytes{0};
    std::atomic<std::int64_t> totalBytes{0};
    std::atomic<std::uint64_t> downloadRate{0};
    std::atomic<std::uint64_t> uploadRate{0};
    std::atomic<int> seeds{0};
    std::atomic<int> peers{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> paused{false};
    std::mutex detailMutex;
    std::mutex pauseMutex;
    std::condition_variable pauseCondition;
    std::wstring detail;
    std::thread worker;

    Job(unsigned number, std::wstring source, std::wstring target);
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
};

class Manager {
public:
    Manager();
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // Returns zero for a duplicate active task or invalid arguments.
    unsigned Add(const std::wstring& url, const std::wstring& destination);
    unsigned Retry(unsigned id);
    std::size_t RetryFailed();
    bool Pause(unsigned id);
    bool Resume(unsigned id);
    bool Cancel(unsigned id);
    void PauseAll();
    void ResumeAll();
    void CancelAll();
    std::size_t RemoveFinished();
    bool RemoveTerminal(unsigned id);
    bool HasActiveDuplicate(const std::wstring& url, const std::wstring& path) const;
    std::vector<Snapshot> List() const;

    void SetMode(Mode value);
    Mode GetMode() const noexcept { return mode_.load(); }

private:
    std::shared_ptr<Job> Find(unsigned id) const;
    bool AcquireSlot(const std::shared_ptr<Job>& job);
    void ReleaseSlot();
    void Run(const std::shared_ptr<Job>& job);
    void RunHttp(const std::shared_ptr<Job>& job);
    void RunFtp(const std::shared_ptr<Job>& job);
    void RunFtps(const std::shared_ptr<Job>& job);
    void RunTorrent(const std::shared_ptr<Job>& job);
    void RemoveTorrent(unsigned id);
    struct TorrentRecord;
    std::unique_ptr<libtorrent::session> torrentSession_;
    mutable std::mutex torrentMutex_;
    std::vector<std::shared_ptr<TorrentRecord>> torrents_;

    mutable std::mutex jobsMutex_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::atomic<unsigned> nextID_{1};
    std::atomic<Mode> mode_{Mode::Balanced};
    std::mutex slotMutex_;
    std::condition_variable slotCondition_;
    int runningTransfers_ = 0;
};

std::wstring StateName(State state);
std::wstring ModeName(Mode mode);
std::wstring HumanBytes(std::int64_t bytes);
std::wstring HumanRate(double bytesPerSecond);
}

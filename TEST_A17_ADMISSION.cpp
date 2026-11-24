#include "performance.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    constexpr unsigned workers=16, segments=600;
    std::atomic<unsigned> next{0}, inFlight{0}, target{4}, done{0};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::thread> pool;
    for(unsigned index=0;index<workers;++index) {
        pool.emplace_back([&] {
            for(;;) {
                if(next.load()>=segments) break;
                unsigned observed=inFlight.load();
                if(!fd::performance::CanAdmitClaim(observed,target.load()) ||
                   !inFlight.compare_exchange_weak(observed,observed+1)) {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait_for(lock,std::chrono::milliseconds(20),[&] {
                        return fd::performance::HttpPermitWakeRequired(
                            inFlight.load(),target.load(),next.load()>=segments,false);
                    });
                    continue;
                }
                const unsigned assigned=next.fetch_add(1);
                if(assigned>=segments) {
                    inFlight.fetch_sub(1);
                    changed.notify_one();
                    break;
                }
                if(assigned==37 || assigned==230) { target.store(1);changed.notify_all(); }
                if(assigned==128 || assigned==340) { target.store(8);changed.notify_all(); }
                std::this_thread::yield();
                done.fetch_add(1);
                inFlight.fetch_sub(1);
                changed.notify_one();
            }
        });
    }
    for(auto& t:pool) t.join();
    assert(done==segments && inFlight==0 && next>=segments);
}

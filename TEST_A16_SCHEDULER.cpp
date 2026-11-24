#include "performance.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

int main() {
    constexpr unsigned workers=12;
    constexpr unsigned segments=256;
    std::atomic<unsigned> next{0}, inFlight{0}, target{4}, finished{0}, peak{0};
    std::vector<std::thread> threads;
    for(unsigned t=0;t<workers;++t) {
        threads.emplace_back([&] {
            for(;;) {
                if(next.load()>=segments) break;
                unsigned observed=inFlight.load();
                if(!fd::performance::CanAdmitClaim(observed,target.load()) ||
                   !inFlight.compare_exchange_weak(observed,observed+1)) {
                    std::this_thread::yield();
                    continue;
                }
                unsigned index=next.fetch_add(1);
                if(index>=segments) { inFlight.fetch_sub(1); break; }
                unsigned running=inFlight.load();
                unsigned previous=peak.load();
                while(running>previous && !peak.compare_exchange_weak(previous,running)) {}
                // The target may decrease during a running claim; never revoke that claim.
                if(index==28) target.store(1);
                if(index==60) target.store(6);
                std::this_thread::yield();
                finished.fetch_add(1);
                inFlight.fetch_sub(1);
            }
        });
    }
    for(auto& thread:threads) thread.join();
    assert(finished==segments);
    assert(next>=segments);
    assert(inFlight==0);
    assert(peak>0 && peak<=workers);
    unsigned cooldown=0;
    cooldown=fd::performance::AdaptiveCooldownAfterChange(cooldown,true);
    assert(!fd::performance::CanRetuneTarget(5,cooldown));
    for(unsigned n=0;n<3;++n) cooldown=fd::performance::AdaptiveCooldownAfterChange(cooldown,false);
    assert(fd::performance::CanRetuneTarget(5,cooldown));
}

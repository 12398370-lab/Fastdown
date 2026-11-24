#include "performance.h"
#include <algorithm>
#include <limits>
namespace fd::performance {
unsigned RetryLimit(unsigned mode) noexcept { return mode >= 2 ? 4u : 3u; }
unsigned RetryDelayMs(unsigned attempt, unsigned mode) noexcept {
    const unsigned base = mode >= 2 ? 300u : 500u;
    const unsigned shift = (std::min)(attempt, 4u);
    return (std::min)(base << shift, 6000u);
}
std::size_t NetworkBufferBytes(unsigned mode) noexcept {
    return mode >= 3 ? 1024u*1024u : mode == 2 ? 512u*1024u : 256u*1024u;
}
unsigned ConnectTimeoutSeconds(unsigned mode) noexcept { return mode >= 2 ? 15u : 30u; }
bool TransientCurlError(int code) noexcept {
    // libcurl numeric CURLcode constants: resolve, connect, partial, timeout,
    // send, receive and HTTP/2 stream framing. TLS validation is never retried.
    switch (code) { case 5: case 6: case 7: case 18: case 28:
                    case 55: case 56: case 92: return true; default: return false; }
}
bool ShouldRetry(int code, unsigned attempt, unsigned mode, bool cancelled, bool diskFailed) noexcept {
    return !cancelled && !diskFailed && attempt < RetryLimit(mode) && TransientCurlError(code);
}
bool ExactTransferSize(std::int64_t received, std::int64_t expected) noexcept {
    return received >= 0 && (expected < 0 || received == expected);
}
unsigned ClampParallelism(unsigned requested, unsigned segments) noexcept {
    return segments == 0 ? 0u : (std::min)((std::max)(requested, 1u), (std::min)(segments, 32u));
}
std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
    const auto limit = (std::numeric_limits<std::uint64_t>::max)();
    return a > limit - b ? limit : a + b;
}
std::uint64_t BytesPerSecond(std::uint64_t bytes, std::uint64_t elapsedMs) noexcept {
    if (!elapsedMs) return 0;
    const auto quotient = bytes / elapsedMs;
    const auto remainder = bytes % elapsedMs;
    const auto limit = (std::numeric_limits<std::uint64_t>::max)();
    if (quotient > limit / 1000u) return limit;
    const auto fractional = static_cast<std::uint64_t>((static_cast<long double>(remainder) * 1000.0L) / elapsedMs);
    return SaturatingAdd(quotient * 1000u, fractional);
}
std::uint64_t SmoothRate(std::uint64_t previous, std::uint64_t current, unsigned weightPercent) noexcept {
    const unsigned w = (std::min)(weightPercent, 100u);
    return (previous / 100u) * (100u-w) + (current / 100u) * w +
        ((previous % 100u) * (100u-w) + (current % 100u) * w) / 100u;
}
unsigned AdaptiveWorkerTarget(unsigned current, unsigned maximum, std::uint64_t oldRate, std::uint64_t newRate) noexcept {
    if (!maximum) return 0;
    current = (std::max)(1u, (std::min)(current, maximum));
    if (!oldRate) return current;
    if (newRate > oldRate + oldRate / 8u && current < maximum) return current + 1;
    if (newRate < oldRate - oldRate / 5u && current > 1u) return current - 1;
    return current;
}
unsigned RetryJitterMs(unsigned attempt, unsigned jobId) noexcept {
    return (attempt * 37u + jobId * 53u) % 197u;
}
std::uint64_t EstimatedRemainingSeconds(std::uint64_t remainingBytes, std::uint64_t rate) noexcept {
    return !rate ? 0 : remainingBytes / rate + (remainingBytes % rate != 0u);
}
std::size_t SafeReadBuffer(std::size_t requested, std::size_t maximum) noexcept {
    return maximum == 0 ? 0 : (std::min)((std::max)(requested, std::size_t{4096}), maximum);
}
bool ValidCheckpointProgress(std::uint64_t completed, std::uint64_t total) noexcept {
    return completed <= total;
}
bool SampleWindowReady(std::uint64_t elapsedMs, std::uint64_t minimumMs) noexcept { return minimumMs != 0 && elapsedMs >= minimumMs; }
std::uint64_t RateDelta(std::uint64_t previousBytes, std::uint64_t currentBytes) noexcept { return currentBytes >= previousBytes ? currentBytes - previousBytes : 0; }
unsigned InitialWorkerTarget(unsigned maximum) noexcept { return maximum == 0 ? 0u : (std::min)(maximum, 4u); }
bool StableSampleCount(unsigned samples, unsigned threshold) noexcept { return threshold != 0 && samples >= threshold; }
unsigned AdjustWorkerTarget(unsigned current, unsigned maximum, std::uint64_t previousRate, std::uint64_t newRate, unsigned stableSamples) noexcept {
    if (!maximum) return 0;
    current = (std::max)(1u, (std::min)(current, maximum));
    if (!StableSampleCount(stableSamples, 2) || !previousRate) return current;
    return AdaptiveWorkerTarget(current, maximum, previousRate, newRate);
}
bool WorkerMayClaim(unsigned workerIndex, unsigned activeWorkers) noexcept { return workerIndex < activeWorkers; }
std::uint64_t SafeRemainingBytes(std::uint64_t total, std::uint64_t completed) noexcept { return completed >= total ? 0u : total - completed; }
unsigned BackoffCapMs(unsigned baseMs, unsigned maximumMs) noexcept { return (std::min)(baseMs, maximumMs); }

// A11: reply-code retry policy applies only to temporary 4xx FTP responses.
unsigned FtpRetryBudget(unsigned mode) noexcept { return RetryLimit(mode); }
unsigned FtpRetryWaitMs(unsigned attempt, unsigned mode, unsigned jobId) noexcept {
    const unsigned base = RetryDelayMs(attempt, mode);
    return (std::min)(base + RetryJitterMs(attempt, jobId), 8000u);
}
bool IsFtpTransientReply(long reply) noexcept { return reply == 421 || reply == 425 || reply == 426 || reply == 450 || reply == 451 || reply == 452; }
bool RetryFtpReply(long reply, unsigned attempt, unsigned mode, bool cancelled, bool diskFailed) noexcept {
    return !cancelled && !diskFailed && attempt < FtpRetryBudget(mode) && IsFtpTransientReply(reply);
}
unsigned FtpLowSpeedSeconds(unsigned mode) noexcept { return mode >= 2 ? 30u : 45u; }
unsigned FtpLowSpeedBytes(unsigned mode) noexcept { return mode >= 2 ? 128u : 64u; }
std::size_t CurlReadBufferBytes(unsigned mode) noexcept { return (std::min)(NetworkBufferBytes(mode), std::size_t{512u*1024u}); }
bool IsTransferCancelled(bool stopping, bool diskFailed) noexcept { return stopping || diskFailed; }
// A12: portable, directly exercised FTPS safety policies.
bool SafeTransferIncrement(std::int64_t current, std::size_t incoming) noexcept {
    return current >= 0 && incoming <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)() - current);
}
std::int64_t TransferIncrement(std::int64_t current, std::size_t incoming) noexcept {
    return SafeTransferIncrement(current, incoming) ? current + static_cast<std::int64_t>(incoming) : -1;
}
bool RetryBudgetAvailable(unsigned attempt, unsigned mode) noexcept { return attempt < FtpRetryBudget(mode); }
unsigned RetrySleepSliceMs(unsigned remainingMs) noexcept { return (std::min)(remainingMs, 50u); }
bool RetryWaitShouldAdvance(bool paused, bool stopping) noexcept { return !paused && !stopping; }
bool PublishAllowed(bool transferOk, bool diskOk, bool sizeOk, bool cancelled) noexcept {
    return transferOk && diskOk && sizeOk && !cancelled;
}
unsigned RetryAttemptNext(unsigned attempt) noexcept { return attempt == (std::numeric_limits<unsigned>::max)() ? attempt : attempt + 1u; }
bool ValidFtpReply(long reply) noexcept { return reply >= 100 && reply <= 599; }

// A13: pure, portable policies. FTP response codes are protocol status values.
bool RetryableFtpStatus(long status) noexcept {
    return status == 421 || status == 425 || status == 426 || status == 450 || status == 451 || status == 452;
}
bool RetryableFtpFailure(int curlResult, long status, unsigned attempt, unsigned mode, bool cancelled, bool diskFailed) noexcept {
    // A transient FTP reply may accompany several different libcurl failure codes.
    // Never retry success, disk failures, cancellation, or a permanent FTP response.
    return curlResult != 0 && !cancelled && !diskFailed && attempt < FtpRetryBudget(mode) && RetryableFtpStatus(status);
}
unsigned BoundedRetrySleep(unsigned remainingMs, unsigned maximumSliceMs) noexcept {
    return remainingMs < maximumSliceMs ? remainingMs : maximumSliceMs;
}
unsigned AdvanceRetryClock(unsigned elapsedMs, unsigned sliceMs, bool paused, bool cancelled) noexcept {
    if (paused || cancelled) return elapsedMs;
    return elapsedMs > UINT32_MAX - sliceMs ? UINT32_MAX : elapsedMs + sliceMs;
}
unsigned EffectiveRetryDelay(unsigned attempt, unsigned mode, unsigned jobId) noexcept {
    return FtpRetryWaitMs(attempt, mode, jobId);
}
bool FtpsFileSizeMatches(std::int64_t received, std::int64_t remoteSize) noexcept {
    return ExactTransferSize(received, remoteSize);
}
unsigned MaxFtpsRetries(unsigned mode) noexcept { return FtpRetryBudget(mode); }
std::uint64_t ClampTransferRate(std::uint64_t measured, std::uint64_t ceiling) noexcept {
    return ceiling && measured > ceiling ? ceiling : measured;
}
// A14: portable scheduling policies used to prevent admission deadlocks.
bool CanAdmitClaim(unsigned inFlight, unsigned target) noexcept { return target > 0 && inFlight < target; }
unsigned ClampWorkerTarget(unsigned requested, unsigned maximum) noexcept {
    return maximum == 0 ? 0u : (std::max)(1u, (std::min)(requested, maximum));
}
unsigned ScaleWorkerTarget(unsigned current, unsigned maximum, bool increase) noexcept {
    if (maximum == 0) return 0;
    current = ClampWorkerTarget(current, maximum);
    return increase ? (std::min)(maximum, current + (current < maximum ? 1u : 0u)) :
                      (std::max)(1u, current - (current > 1 ? 1u : 0u));
}
std::uint64_t SaturatingRateBytes(std::uint64_t complete, std::uint64_t live) noexcept {
    return SaturatingAdd(complete, live);
}
bool ValidThroughputSample(std::uint64_t previous, std::uint64_t current) noexcept {
    return current >= previous;
}
unsigned SamplerSleepMs(unsigned requested) noexcept { return (std::max)(25u, (std::min)(requested, 1000u)); }
bool AllSegmentsClaimed(std::int64_t next, std::int64_t count) noexcept {
    return count >= 0 && next >= count;
}
bool RetryableHttpStatus(unsigned status) noexcept {
    return status == 408 || status == 425 || status == 429 || (status >= 500 && status <= 504);
}

// A15: telemetry includes in-progress bytes, so large segments do not appear idle.
std::uint64_t NonnegativeTransferBytes(std::int64_t bytes) noexcept {
    return bytes > 0 ? static_cast<std::uint64_t>(bytes) : 0u;
}
std::uint64_t SampledTransferBytes(std::int64_t completed, std::int64_t inProgress) noexcept {
    return SaturatingAdd(NonnegativeTransferBytes(completed), NonnegativeTransferBytes(inProgress));
}
bool ThroughputCounterRolledBack(std::uint64_t previous, std::uint64_t current) noexcept {
    return current < previous;
}
std::uint64_t ThroughputWindowDelta(std::uint64_t previous, std::uint64_t current) noexcept {
    return ThroughputCounterRolledBack(previous, current) ? 0u : current - previous;
}
unsigned NextStableSample(unsigned current, bool valid, unsigned maximum) noexcept {
    if (!valid) return 0;
    return current >= maximum ? maximum : current + 1u;
}
unsigned AdaptiveTargetAfterSample(unsigned current, unsigned maximum, std::uint64_t oldRate,
                                   std::uint64_t newRate, unsigned stableSamples, bool valid) noexcept {
    return valid ? AdjustWorkerTarget(current, maximum, oldRate, newRate, stableSamples)
                 : ClampWorkerTarget(current, maximum);
}
bool ShouldSampleThroughput(bool paused, bool stopping, bool failed) noexcept {
    return !paused && !stopping && !failed;
}
unsigned PermitWaitMilliseconds(unsigned target, unsigned inFlight) noexcept {
    return CanAdmitClaim(inFlight, target) ? 0u : 40u;
}


// A16: quick initial concurrency for aggressive modes, but never above configured limit.
unsigned InitialConcurrencyForMode(unsigned maximum, unsigned mode) noexcept {
    if (!maximum) return 0;
    const unsigned preferred = mode >= 3 ? 8u : mode == 2 ? 6u : mode == 1 ? 4u : 2u;
    return (std::min)(maximum, preferred);
}
// An adjustment holds the concurrency target for three valid sample windows;
// avoids rapid thread oscillations on bursty Wi-Fi and proxy connections.
unsigned AdaptiveCooldownAfterChange(unsigned current, bool changed) noexcept {
    return changed ? 3u : current > 0u ? current - 1u : 0u;
}
bool CanRetuneTarget(unsigned stableSamples, unsigned cooldown) noexcept {
    return cooldown == 0u && stableSamples >= 2u;
}
bool TerminalFtpReply(long reply) noexcept {
    // 5xx replies are permanent errors. Invalid replies are NOT permanent.
    return reply >= 500 && reply <= 599;
}
bool RetryableFtpsResponse(int curlCode, long ftpReply, unsigned attempt, unsigned mode,
                           bool stopping, bool diskFailed) noexcept {
    if (curlCode == 0 || stopping || diskFailed || attempt >= FtpRetryBudget(mode)) return false;
    // Permanent reply and certificate/SSH-like local TLS validation failures are non-retryable.
    if (TerminalFtpReply(ftpReply)) return false;
    switch (curlCode) {
        case 51: case 58: case 60: case 77: case 83: case 90: return false;
        default: break;
    }
    return TransientCurlError(curlCode) || RetryableFtpStatus(ftpReply);
}
std::int64_t ClampedSnapshotBytes(std::int64_t completed, std::int64_t live) noexcept {
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    const auto safeCompleted = (std::max)(std::int64_t{0}, completed);
    const auto safeLive = (std::max)(std::int64_t{0}, live);
    return safeLive > maximum - safeCompleted ? maximum : safeCompleted + safeLive;
}
unsigned CompletionBasisPoints(std::int64_t completed, std::int64_t total) noexcept {
    if (total <= 0 || completed <= 0) return 0;
    if (completed >= total) return 10000;
    // Avoid completed*10000 overflow; computed with quotient and remainder.
    const std::uint64_t part = static_cast<std::uint64_t>(completed);
    const std::uint64_t whole = static_cast<std::uint64_t>(total);
    const std::uint64_t q = part / whole;
    const std::uint64_t r = part % whole;
    // r<whole, safe for long double intermediate (no integer overflow).
    return static_cast<unsigned>(q * 10000u + static_cast<std::uint64_t>(
        static_cast<long double>(r) * 10000.0L / static_cast<long double>(whole)));
}
unsigned IdleWorkerSleepMs(bool paused, unsigned inFlight, unsigned target) noexcept {
    if (paused) return 120u;
    return CanAdmitClaim(inFlight, target) ? 0u : 25u;
}


// A17: permanent HTTP failures should not consume all segment retry attempts.
bool HttpFailureIsPermanent(unsigned status) noexcept {
    if (status == 200u || status == 301u || status == 302u || status == 303u ||
        status == 307u || status == 308u) return true; // invalid range response/redirect
    return status >= 400u && status <= 499u &&
           status != 408u && status != 425u && status != 429u;
}
bool RetryHttpSegment(unsigned status, unsigned attempt, unsigned maximumAttempts,
                      bool cancelled, bool validatorChanged) noexcept {
    if (cancelled || validatorChanged || maximumAttempts == 0u ||
        attempt >= maximumAttempts - 1u || HttpFailureIsPermanent(status)) return false;
    // 0 means an I/O failure before a status could be read; 206 may fail mid-body.
    return status == 0u || status == 206u || RetryableHttpStatus(status) ||
           (status >= 500u && status <= 599u);
}
unsigned BackoffSleepSliceMs(unsigned remainingMs) noexcept {
    return (std::min)(remainingMs, 50u);
}
bool BackoffClockShouldAdvance(bool paused, bool stopping) noexcept {
    return !paused && !stopping;
}
bool HttpSegmentWriteAllowed(std::int64_t alreadyWritten, unsigned incoming,
                             std::int64_t expectedSegmentBytes) noexcept {
    return alreadyWritten >= 0 && expectedSegmentBytes >= 0 &&
           alreadyWritten <= expectedSegmentBytes &&
           static_cast<std::uint64_t>(incoming) <=
           static_cast<std::uint64_t>(expectedSegmentBytes - alreadyWritten);
}
bool HttpPermitWakeRequired(unsigned inFlight, unsigned target,
                            bool allClaimed, bool stopping) noexcept {
    return stopping || allClaimed || CanAdmitClaim(inFlight, target);
}
unsigned HttpPermitWaitMilliseconds(bool paused) noexcept {
    // Bounded wait ensures cancellation responsiveness even without notify.
    return paused ? 150u : 120u;
}
bool FtpsReplyAcceptable(int curlResult, long ftpReply) noexcept {
    // A successful libcurl completion may not expose a final FTP reply.
    return curlResult == 0 && (ftpReply == 0 ||
        (ftpReply >= 200 && ftpReply < 300));
}

} // namespace fd::performance

#pragma once
#include <cstdint>
#include <cstddef>
namespace fd::performance {
// Pure transfer policies; testable without Win32 or network dependencies.
unsigned RetryLimit(unsigned mode) noexcept;
unsigned RetryDelayMs(unsigned attempt, unsigned mode) noexcept;
std::size_t NetworkBufferBytes(unsigned mode) noexcept;
unsigned ConnectTimeoutSeconds(unsigned mode) noexcept;
bool TransientCurlError(int code) noexcept;
bool ShouldRetry(int code, unsigned attempt, unsigned mode, bool cancelled, bool diskFailed) noexcept;
bool ExactTransferSize(std::int64_t received, std::int64_t expected) noexcept;
unsigned ClampParallelism(unsigned requested, unsigned segments) noexcept;
// A9: portable throughput and transfer scheduling policies.
std::uint64_t SaturatingAdd(std::uint64_t a, std::uint64_t b) noexcept;
std::uint64_t BytesPerSecond(std::uint64_t bytes, std::uint64_t elapsedMs) noexcept;
std::uint64_t SmoothRate(std::uint64_t previous, std::uint64_t current, unsigned weightPercent) noexcept;
unsigned AdaptiveWorkerTarget(unsigned current, unsigned maximum, std::uint64_t oldRate, std::uint64_t newRate) noexcept;
unsigned RetryJitterMs(unsigned attempt, unsigned jobId) noexcept;
std::uint64_t EstimatedRemainingSeconds(std::uint64_t remainingBytes, std::uint64_t bytesPerSecond) noexcept;
std::size_t SafeReadBuffer(std::size_t requested, std::size_t maximum) noexcept;
bool ValidCheckpointProgress(std::uint64_t completed, std::uint64_t total) noexcept;
// A10: adaptive scheduling, safe sample windows and operational telemetry.
bool SampleWindowReady(std::uint64_t elapsedMs, std::uint64_t minimumMs) noexcept;
std::uint64_t RateDelta(std::uint64_t previousBytes, std::uint64_t currentBytes) noexcept;
unsigned InitialWorkerTarget(unsigned maximum) noexcept;
unsigned AdjustWorkerTarget(unsigned current, unsigned maximum, std::uint64_t previousRate, std::uint64_t newRate, unsigned stableSamples) noexcept;
bool WorkerMayClaim(unsigned workerIndex, unsigned activeWorkers) noexcept;
std::uint64_t SafeRemainingBytes(std::uint64_t total, std::uint64_t completed) noexcept;
unsigned BackoffCapMs(unsigned baseMs, unsigned maximumMs) noexcept;
bool StableSampleCount(unsigned samples, unsigned threshold) noexcept;
// A11: FTPS reliability and transfer-safety policies.
unsigned FtpRetryBudget(unsigned mode) noexcept;
unsigned FtpRetryWaitMs(unsigned attempt, unsigned mode, unsigned jobId) noexcept;
bool IsFtpTransientReply(long ftpReply) noexcept;
bool RetryFtpReply(long ftpReply, unsigned attempt, unsigned mode, bool cancelled, bool diskFailed) noexcept;
unsigned FtpLowSpeedSeconds(unsigned mode) noexcept;
unsigned FtpLowSpeedBytes(unsigned mode) noexcept;
std::size_t CurlReadBufferBytes(unsigned mode) noexcept;
bool IsTransferCancelled(bool stopping, bool diskFailed) noexcept;
// A12: overflow-safe FTPS writes and bounded retry state transitions.
bool SafeTransferIncrement(std::int64_t current, std::size_t incoming) noexcept;
std::int64_t TransferIncrement(std::int64_t current, std::size_t incoming) noexcept;
bool RetryBudgetAvailable(unsigned attempt, unsigned mode) noexcept;
unsigned RetrySleepSliceMs(unsigned remainingMs) noexcept;
bool RetryWaitShouldAdvance(bool paused, bool stopping) noexcept;
bool PublishAllowed(bool transferOk, bool diskOk, bool sizeOk, bool cancelled) noexcept;
unsigned RetryAttemptNext(unsigned attempt) noexcept;
bool ValidFtpReply(long reply) noexcept;
// A13: FTPS retry classification, time accounting and bounded transfer policies.
bool RetryableFtpStatus(long status) noexcept;
bool RetryableFtpFailure(int curlResult, long status, unsigned attempt, unsigned mode, bool cancelled, bool diskFailed) noexcept;
unsigned BoundedRetrySleep(unsigned remainingMs, unsigned maximumSliceMs) noexcept;
unsigned AdvanceRetryClock(unsigned elapsedMs, unsigned sliceMs, bool paused, bool cancelled) noexcept;
unsigned EffectiveRetryDelay(unsigned attempt, unsigned mode, unsigned jobId) noexcept;
bool FtpsFileSizeMatches(std::int64_t received, std::int64_t remoteSize) noexcept;
unsigned MaxFtpsRetries(unsigned mode) noexcept;
std::uint64_t ClampTransferRate(std::uint64_t measured, std::uint64_t ceiling) noexcept;
// A14: bounded permit admission, overflow-safe byte arithmetic and stable sampling.
bool CanAdmitClaim(unsigned inFlight, unsigned target) noexcept;
unsigned ClampWorkerTarget(unsigned requested, unsigned maximum) noexcept;
unsigned ScaleWorkerTarget(unsigned current, unsigned maximum, bool increase) noexcept;
std::uint64_t SaturatingRateBytes(std::uint64_t complete, std::uint64_t live) noexcept;
bool ValidThroughputSample(std::uint64_t previous, std::uint64_t current) noexcept;
unsigned SamplerSleepMs(unsigned requested) noexcept;
bool AllSegmentsClaimed(std::int64_t next, std::int64_t count) noexcept;
bool RetryableHttpStatus(unsigned status) noexcept;
// A15: throughput sampling, retry rollback and scheduling guards.
std::uint64_t NonnegativeTransferBytes(std::int64_t bytes) noexcept;
std::uint64_t SampledTransferBytes(std::int64_t completed, std::int64_t inProgress) noexcept;
bool ThroughputCounterRolledBack(std::uint64_t previous, std::uint64_t current) noexcept;
std::uint64_t ThroughputWindowDelta(std::uint64_t previous, std::uint64_t current) noexcept;
unsigned NextStableSample(unsigned current, bool valid, unsigned maximum) noexcept;
unsigned AdaptiveTargetAfterSample(unsigned current, unsigned maximum, std::uint64_t oldRate, std::uint64_t newRate, unsigned stableSamples, bool valid) noexcept;
bool ShouldSampleThroughput(bool paused, bool stopping, bool failed) noexcept;
unsigned PermitWaitMilliseconds(unsigned target, unsigned inFlight) noexcept;
// A16: cooldown-controlled adaptive concurrency, FTPS retry safety, UI-safe progress.
unsigned InitialConcurrencyForMode(unsigned maximum, unsigned mode) noexcept;
unsigned AdaptiveCooldownAfterChange(unsigned current, bool changed) noexcept;
bool CanRetuneTarget(unsigned stableSamples, unsigned cooldown) noexcept;
bool TerminalFtpReply(long reply) noexcept;
bool RetryableFtpsResponse(int curlCode, long ftpReply, unsigned attempt, unsigned mode, bool stopping, bool diskFailed) noexcept;
std::int64_t ClampedSnapshotBytes(std::int64_t completed, std::int64_t live) noexcept;
unsigned CompletionBasisPoints(std::int64_t completed, std::int64_t total) noexcept;
unsigned IdleWorkerSleepMs(bool paused, unsigned inFlight, unsigned target) noexcept;
// A17: classified HTTP retries, pause-aware backoff, and event-driven worker admission.
bool HttpFailureIsPermanent(unsigned status) noexcept;
bool RetryHttpSegment(unsigned status, unsigned attempt, unsigned maximumAttempts,
                      bool cancelled, bool validatorChanged) noexcept;
unsigned BackoffSleepSliceMs(unsigned remainingMs) noexcept;
bool BackoffClockShouldAdvance(bool paused, bool stopping) noexcept;
bool HttpSegmentWriteAllowed(std::int64_t alreadyWritten, unsigned incoming,
                             std::int64_t expectedSegmentBytes) noexcept;
bool HttpPermitWakeRequired(unsigned inFlight, unsigned target,
                            bool allClaimed, bool stopping) noexcept;
unsigned HttpPermitWaitMilliseconds(bool paused) noexcept;
bool FtpsReplyAcceptable(int curlResult, long ftpReply) noexcept;
}

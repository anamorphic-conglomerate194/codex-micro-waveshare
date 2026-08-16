// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

namespace stopwatch_usb_mic {

// Immediate disposition of a nonblocking local completion-chime request.
// The USB descriptor remains microphone-only; this API drives only the
// StopWatch's local speaker when the host is not requesting UAC microphone
// streaming.
enum class ChimeRequestResult : uint8_t {
  Queued,
  SkippedStreaming,
  Busy,
  Unavailable,
  Unsupported,
};

// Latest accepted request's terminal state. `Pending` covers both the idle
// guard and local playback; consult ChimeStatus::playing to distinguish them.
enum class ChimeResult : uint8_t {
  NeverRequested,
  Pending,
  Played,
  SkippedStreaming,
  AbortedStreaming,
  Unavailable,
  Unsupported,
  SpeakerStartFailed,
  PlaybackFailed,
  MicrophoneRestoreFailed,
};

struct ChimeStatus {
  uint32_t sequence;
  ChimeResult result;
  bool pending;
  bool playing;
  bool stream_requested;
  // False during intentional Mic -> Speaker ownership transfer as well as on
  // a terminal capture/restore failure.
  bool microphone_ready;
};

// Monotonic diagnostics for the microphone pipeline. `discontinuities` counts
// known breaks in otherwise-active source audio (capture gaps, queue eviction,
// source timeout, or an in-flight block invalidated by a stream transition).
// Short and zero USB writes are not discontinuities because their unwritten
// bytes are retained.
struct Stats {
  uint64_t captured_blocks;
  uint64_t captured_samples;
  uint64_t queued_blocks;
  uint64_t queue_drops;
  uint64_t write_zero;
  uint64_t write_short;
  uint64_t written_bytes;
  uint64_t startup_prefills;
  uint64_t startup_prefill_calls;
  uint64_t startup_prefill_bytes;
  uint64_t startup_prefill_shortfall_bytes;
  uint64_t startup_prefill_limit_hits;
  uint64_t startup_topups;
  uint64_t startup_topup_calls;
  uint64_t startup_topup_bytes;
  uint64_t queue_underruns;
  uint64_t underrun_silence_calls;
  uint64_t underrun_silence_bytes;
  uint64_t inactive_blocks;
  uint64_t stale_blocks;
  uint64_t capture_gaps;
  uint64_t transition_cleared_blocks;
  uint64_t abandoned_blocks;
  uint64_t abandoned_bytes;
  uint64_t discontinuities;
  uint32_t interface_transitions;
  uint32_t stream_transitions;
  uint32_t queue_high_water;
  uint32_t chime_requests;
  uint32_t chime_queued;
  uint32_t chime_played;
  uint32_t chime_skipped_streaming;
  uint32_t chime_aborted_streaming;
  uint32_t chime_skipped_busy;
  uint32_t chime_unavailable;
  uint32_t chime_unsupported;
  uint32_t chime_failures;
  uint32_t chime_capture_pauses;
  bool active;
};

// Starts a microphone-only USB Audio Class device backed by the StopWatch's
// built-in ES8311/MEMS microphone path. Returns false if either the codec,
// TinyUSB audio interface, or feeder task could not start.
//
// Defining CODEX_STOPWATCH_USB_MIC_SYNTHETIC=1 at compile time substitutes a
// deterministic 1 kHz tone for physical capture. The production path is
// unchanged when the flag is absent or zero.
bool begin();

// Requests the ~270 ms local completion sound without blocking the caller.
// A request is skipped immediately when the host has selected the microphone's
// streaming alternate setting. Otherwise capture continues through a short
// idle guard; any stream request during the guard cancels the sound. If a
// stream request arrives during playback, playback is aborted and microphone
// capture is restored first.
ChimeRequestResult requestCompletionChime();

// Returns an internally consistent snapshot of the latest accepted chime.
ChimeStatus snapshotChimeStatus();

// Returns an internally consistent snapshot of the monotonic diagnostics.
Stats snapshotStats();

}  // namespace stopwatch_usb_mic

#include <cstdint>
#include <type_traits>
#include <fan/utils/utility.h>
#include <fan/event/types.h>
#include <fan/types/dme.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <exception>
#include <coroutine>
#include <string>
#include <array>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <future>
#include <queue>
#include <deque>
#include <memory>
#include <cmath>
#include <fstream>
#include <sstream>
#include <ctime>
#include <filesystem>
#include <random>
#include <cstdio>
#include <cctype>

#include <WITCH/WITCH.h>

extern "C" {
  #include <libavutil/pixfmt.h>
}

extern "C" {
  #include <libavformat/avformat.h>
  #include <libavcodec/avcodec.h>
}

#include <vector>
#include <stdexcept>

#if __has_include("cuda.h")
#include <cuda.h>
#endif

import fan;
import fan.fmt;

import fan.graphics.video.screen;
using namespace fan::graphics;

#include <fan/graphics/types.h>
#include "midi/midi_synth.h"
#include "midi/midi_input.h"
#include "midi/midi_worker.h"
#include "midi/midi_sac_bank.h"

#if defined(_WIN32)
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <dlfcn.h>
#endif

// The graphics card encoders this computer can run, as "NVIDIA (h264_nvenc)": the encoder is in the bundled FFmpeg
// and the driver library it needs loads. Opening a real encoder for each would take long at start
static std::vector<std::string> find_gpu_encoders(const std::vector<std::string>& compiled) {
  std::vector<std::string> usable;
  auto has = [&](const char* name) { return std::find(compiled.begin(), compiled.end(), name) != compiled.end(); };
#if defined(_WIN32)
  auto loads = [](const wchar_t* dll) {
    HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module) {
      FreeLibrary(module);
    }
    return module != nullptr;
  };
  if (has("h264_nvenc") && loads(L"nvEncodeAPI64.dll")) {
    usable.push_back("NVIDIA (h264_nvenc)");
  }
  if (has("h264_amf") && loads(L"amfrt64.dll")) {
    usable.push_back("AMD (h264_amf)");
  }
#else
  auto loads = [](const char* library) {
    void* handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
    if (handle) {
      dlclose(handle);
    }
    return handle != nullptr;
  };
  if (has("h264_nvenc") && loads("libnvidia-encode.so.1")) {
    usable.push_back("NVIDIA (h264_nvenc)");
  }
  std::error_code error;
  if (has("h264_vaapi") && std::filesystem::exists("/dev/dri/renderD128", error)) {
    usable.push_back("VAAPI (h264_vaapi)");
  }
#endif
  return usable;
}


// frames wait here for the decoder thread, decoding on the network thread would
// leave incoming packets sitting in the socket while a frame is being decoded
struct decode_queue_t {
  static constexpr size_t max_size = 4;

  bool push(std::vector<uint8_t> frame) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (frames.size() >= max_size) {
        frames.clear();
        return false;
      }
      frames.push_back(std::move(frame));
    }
    cv.notify_one();
    return true;
  }
  bool pop(std::vector<uint8_t>& frame) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return !frames.empty(); });
    if (frames.empty()) {
      return false;
    }
    frame = std::move(frames.front());
    frames.pop_front();
    return true;
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::vector<uint8_t>> frames;
}decode_queue;

// the game comes first when the cpu is full
static void lower_thread_priority() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

// the sender runs on a 1 ms event loop timer, which windows runs at 15.6 ms by default.
// behind a fullscreen game windows would also throttle the process, so opt out of that
static void request_precise_timers() {
#if defined(_WIN32)
  timeBeginPeriod(1);
#if defined(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)
  PROCESS_POWER_THROTTLING_STATE state {};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#endif
#endif
}

// only the newest decoded frame is worth showing. frames swap in and out so the
// plane buffers get reused, a new 1080p frame would page fault 3 mb of memory
struct latest_frame_t {
  using frame_t = fan::graphics::screen_decode_t::decode_data_t;

  void publish(frame_t& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    std::swap(latest, frame);
    fresh = true;
  }
  bool take(frame_t& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!fresh) {
      return false;
    }
    std::swap(latest, frame);
    fresh = false;
    return true;
  }

  std::mutex mutex;
  frame_t latest;
  bool fresh = false;
};

// ms per frame, smoothed. the stream stats show these to tell what limits the frame rate
struct stage_time_t {
  void add(std::uint64_t ns) {
    ms.store(ms.load(std::memory_order_relaxed) * 0.95f + (f32_t)ns / 1e6f * 0.05f, std::memory_order_relaxed);
  }
  std::atomic<f32_t> ms = 0.f;
};
struct stage_scope_t {
  ~stage_scope_t() {
    stage.add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  }
  stage_time_t& stage;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
struct stage_times_t {
  stage_time_t capture, encode, decode, upload;
}stage_times;

// how often the window is drawn, measured over each second by the render loop: a stream decoded at 60 still
// looks like 15 when the window only draws 15 times a second
struct draw_rate_t {
  std::atomic<f32_t> fps{0.f};
  std::atomic<f32_t> slowest_ms{0.f};  // the longest single frame in that second
}draw_rate;

struct render_thread_t;
// 0 prints nothing extra. 1 adds the share/view status lines (what the capture, encoder and network are
// doing, every few seconds). Higher levels print per packet. Set it with -Decps_debug_prints=1
#ifndef ecps_debug_prints
  #define ecps_debug_prints 0
#endif

// fan::print_throttled_format is currently a no-op in fan, so throttle here:
// at most one line per call site every 250 ms
#define ecps_print_throttled(...) do { \
    static std::chrono::steady_clock::time_point ecps_last_print_{}; \
    auto ecps_now_ = std::chrono::steady_clock::now(); \
    if (ecps_now_ - ecps_last_print_ > std::chrono::milliseconds(250)) { \
      ecps_last_print_ = ecps_now_; \
      fan::print(fan::format(__VA_ARGS__)); \
    } \
  } while (0)

std::atomic<render_thread_t*> render_thread_ptr {nullptr};

render_thread_t* get_render_thread() {
  return render_thread_ptr.load(std::memory_order_acquire);
}

#define render_thread get_render_thread()

std::atomic<bool> render_thread_ready {false};

// the encoder and decoder threads are detached and use the render thread's state and the globals.
// They must be gone before either is destroyed, so everything that ends the program stops them first
std::atomic<bool> app_stopping {false};
std::atomic<int> worker_threads_active {0};

struct worker_thread_scope_t {
  ~worker_thread_scope_t() {
    worker_threads_active.fetch_sub(1);
  }
};

// lets go of the screen capture, the next read opens it again (on Wayland: asks which screen or window).
// A fan older than close_screen() still builds, it just keeps the old capture until restart. The generic lambda
// makes the requires check dependent, so a missing member is not an error
#if defined(_WIN32)
// the files Explorer put on the clipboard (Ctrl+C on files), for pasting them into the chat
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
static std::vector<std::string> clipboard_file_paths() {
  std::vector<std::string> paths;
  if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(nullptr)) {
    return paths;
  }
  if (HANDLE data = GetClipboardData(CF_HDROP)) {
    if (auto drop = (HDROP)GlobalLock(data)) {
      UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        paths.push_back(std::filesystem::path(path).string());
      }
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return paths;
}
#endif

static void close_capture(auto& encoder) {
  if constexpr (requires { encoder.close_screen(); }) {
    encoder.close_screen();
  }
}

static void stop_worker_threads() {
  app_stopping.store(true);
  // a worker stuck in a driver call must not keep the program from closing
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (worker_threads_active.load() > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

#include "backend.h"

fan::window::input_injector_t host_input;

struct audio_share_t {
  using header_t = ecps_backend_t::ScreenShare_AudioHeader_t;

  using capture_options_t = fan::audio::loopback_capture_t::options_t;

  void update(bool share, bool play, f32_t volume, const capture_options_t& options) {
    bool want_capture = share && ecps_backend.is_streaming_to_any_channel();
    bool source_changed = options.device_id != capture_options.device_id || options.exclude_discord != capture_options.exclude_discord;
    if (capture.is_running() && (source_changed || discord_changed(options))) {
      capture.stop();
      capture_failed = false;
    }
    capture_options = options;
    capture.set_gain(options.gain);
    if (want_capture && !capture.is_running() && !capture_failed) {
      capture_failed = !encoder.open() || !capture.start([this](const f32_t* pcm, std::uint32_t) { encode(pcm); }, options);
      exclude_failed = options.exclude_discord && !capture_failed && !capture.get_error().empty();
      if (capture_failed || exclude_failed) {
        fan::print("audio sharing:", capture.get_error());
      }
    }
    else if (!want_capture && capture.is_running()) {
      capture.stop();
    }
    capture_failed = capture_failed && share;

    bool want_play = play && ecps_backend.is_viewing_any_channel();
    play_enabled.store(want_play, std::memory_order_relaxed);
    if (want_play && !player.is_running() && !player_failed) {
      player_failed = !player.start();
      if (player_failed) {
        fan::print("audio playback failed to start");
      }
    }
    else if (!want_play && player.is_running()) {
      player.stop();
      receiving = false;
    }
    player_failed = player_failed && play;
    player.set_volume(volume);
  }

  // discord started or restarted since the capture began, so the excluded tree is stale
  bool discord_changed(const capture_options_t& options) {
    if (!options.exclude_discord || exclude_failed || !fan::audio::loopback_capture_t::can_exclude_processes()) {
      return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_discord_check < std::chrono::seconds(3)) {
      return false;
    }
    last_discord_check = now;
    std::uint32_t pid = fan::audio::loopback_capture_t::find_discord_pid();
    return pid != 0 && pid != capture.get_excluded_pid();
  }

  void encode(const f32_t* pcm) {
    std::vector<std::uint8_t> packet(sizeof(header_t) + fan::audio::stream_max_packet);
    ((header_t*)packet.data())->Sequence = send_sequence++;
    std::int32_t size = encoder.encode(pcm, packet.data() + sizeof(header_t), fan::audio::stream_max_packet);
    if (size <= 0) {
      return;
    }
    packet.resize(sizeof(header_t) + size);
    std::lock_guard<std::mutex> lock(outgoing_mutex);
    if (outgoing.size() < max_outgoing) {
      outgoing.push_back(std::move(packet));
    }
  }

  fan::event::task_t send_pending() {
    std::vector<std::vector<std::uint8_t>> packets;
    {
      std::lock_guard<std::mutex> lock(outgoing_mutex);
      packets.swap(outgoing);
    }
    for (const auto& packet : packets) {
      co_await ecps_backend.write_audio(packet);
    }
  }

  void on_packet(const std::uint8_t* data, std::uintptr_t size) {
    if (!play_enabled.load(std::memory_order_relaxed) || (!decoder.state && !decoder.open())) {
      return;
    }
    std::uint16_t sequence = ((const header_t*)data)->Sequence;
    std::int16_t gap = (std::int16_t)(sequence - expected_sequence);
    if (receiving && gap < 0 && gap > -max_late) {
      return;
    }
    std::array<f32_t, fan::audio::stream_frame_size * fan::audio::stream_channels> pcm;
    for (std::int16_t i = 0; receiving && i < std::min<std::int16_t>(gap, max_concealed); ++i) {
      if (decoder.conceal(pcm.data()) > 0) {
        player.push(pcm.data(), fan::audio::stream_frame_size);
      }
    }
    receiving = true;
    expected_sequence = sequence + 1;
    std::int32_t frames = decoder.decode(data + sizeof(header_t), (std::int32_t)(size - sizeof(header_t)), pcm.data());
    if (frames > 0) {
      player.push(pcm.data(), frames);
    }
  }

  static constexpr std::size_t max_outgoing = 25;
  static constexpr std::int16_t max_concealed = 3;
  static constexpr std::int16_t max_late = 50;

  fan::audio::loopback_capture_t capture;
  fan::audio::opus_encoder_t encoder;
  fan::audio::opus_decoder_t decoder;
  fan::audio::live_player_t player;
  std::mutex outgoing_mutex;
  std::vector<std::vector<std::uint8_t>> outgoing;
  std::uint16_t send_sequence = 0;
  std::uint16_t expected_sequence = 0;
  bool receiving = false;
  bool capture_failed = false;
  bool player_failed = false;
  std::atomic<bool> play_enabled = false;
  capture_options_t capture_options;
  bool exclude_failed = false;
  std::chrono::steady_clock::time_point last_discord_check;
}audio_share;

struct voice_chat_t {
  using header_t = ecps_backend_t::ScreenShare_AudioHeader_t;
  using channel_id_t = ecps_backend_t::Protocol_ChannelID_t;

  struct settings_t {
    std::string device;
    f32_t gain = 1.f;
    f32_t threshold = 0.02f;
    f32_t volume = 1.f;
    std::string output_device;
    bool echo_cancel = false;
    bool noise_suppress = true;
  };
  struct speaker_t {
    fan::audio::opus_decoder_t decoder;
    std::uint16_t expected_sequence = 0;
    bool receiving = false;
    std::int64_t heard_at_ms = 0;
  };

  static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void join(channel_id_t channel_id) {
    wanted_channel.store(channel_id.i);
    capture_failed = false;
  }
  void leave() {
    wanted_channel.store(-1);
  }
  bool is_wanted(channel_id_t channel_id) const {
    return wanted_channel.load() == (int)channel_id.i;
  }
  bool is_active() const {
    return active_channel.load() != -1;
  }

  void update(const settings_t& settings) {
    int wanted = wanted_channel.load();
    if (wanted != -1 && !ecps_backend.is_channel_joined(channel_id_t((uint16_t)wanted))) {
      leave();
    }
    bool want_capture = is_active() && !muted.load();
    if (settings.device != device) {
      capture_failed = false;
    }
    if (capture.is_running() && (!want_capture || settings.device != device)) {
      capture.stop();
    }
    device = settings.device;
    capture.set_gain(settings.gain);
    threshold.store(settings.threshold, std::memory_order_relaxed);
    if (want_capture && !capture.is_running() && !capture_failed) {
      capture_failed = !encoder.open(voice_bitrate, true) ||
        !capture.start([this](const f32_t* pcm, std::uint32_t) { encode(pcm); }, device);
      if (capture_failed) {
        fan::print("voice chat:", capture.get_error().empty() ? std::string("the encoder failed to start") : capture.get_error());
      }
    }

    if (!capture.is_running()) {
      level.store(0.f, std::memory_order_relaxed);
    }
    echo_cancel.store(settings.echo_cancel, std::memory_order_relaxed);
    noise_suppress.store(settings.noise_suppress, std::memory_order_relaxed);

    bool listening = is_active() && !deafened.load();
    if (mixer_opening.valid()) {
      if (mixer_opening.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        mixer_open = mixer_opening.get();
        mixer_failed = !mixer_open;
        if (mixer_failed) {
          fan::print("voice chat: playback failed to start");
        }
      }
    }
    else if (mixer_open && (!listening || settings.output_device != output_device)) {
      mixer.stop();
      mixer_open = false;
    }
    else if (listening && !mixer_open && (!mixer_failed || settings.output_device != output_device)) {
      output_device = settings.output_device;
      mixer.played = [this](const f32_t* pcm, std::uint32_t frame_count) {
        if (echo_cancel.load(std::memory_order_relaxed)) {
          echo.push_reference(pcm, frame_count);
        }
      };
      mixer_opening = std::async(std::launch::async, [this] { return mixer.start(output_device); });
    }
    mixer_failed = mixer_failed && listening;
    mixer.set_volume(settings.volume);

    std::lock_guard<std::mutex> lock(speakers_mutex);
    auto members = listening ? ecps_backend.voice_members_of(channel_id_t((uint16_t)active_channel.load())) : std::vector<std::uint32_t>();
    std::erase_if(speakers, [&](const auto& speaker) {
      bool gone = std::find(members.begin(), members.end(), speaker.first) == members.end();
      if (gone) {
        mixer.remove(speaker.first);
      }
      return gone;
    });
  }

  bool has_work() const {
    return active_channel.load() != -1 || wanted_channel.load() != -1 ||
      std::chrono::steady_clock::now() - ecps_backend.voice_probed_at >= std::chrono::seconds(2);
  }

  void encode(const f32_t* input) {
    std::array<f32_t, fan::audio::stream_frame_size * fan::audio::stream_channels> pcm;
    std::copy(input, input + pcm.size(), pcm.begin());
    bool cancel = echo_cancel.load(std::memory_order_relaxed);
    if (cancel && !echo_running) {
      echo.reset();
    }
    echo_running = cancel;
    if (cancel) {
      echo.process(pcm.data(), fan::audio::stream_frame_size);
    }
    bool suppress = noise_suppress.load(std::memory_order_relaxed);
    if (suppress && !noise_running) {
      noise.reset();
    }
    noise_running = suppress;
    if (suppress) {
      noise.process(pcm.data(), fan::audio::stream_frame_size);
    }
    f32_t peak = 0.f;
    for (f32_t sample : pcm) {
      peak = std::max(peak, std::abs(sample));
    }
    level.store(peak, std::memory_order_relaxed);
    std::int64_t now = now_ms();
    if (peak >= threshold.load(std::memory_order_relaxed)) {
      loud_at_ms.store(now, std::memory_order_relaxed);
    }
    if (now - loud_at_ms.load(std::memory_order_relaxed) > hold_ms) {
      return;
    }
    std::vector<std::uint8_t> packet(sizeof(header_t) + fan::audio::stream_max_packet);
    ((header_t*)packet.data())->Sequence = send_sequence++;
    std::int32_t size = encoder.encode(pcm.data(), packet.data() + sizeof(header_t), fan::audio::stream_max_packet);
    if (size <= 0) {
      return;
    }
    packet.resize(sizeof(header_t) + size);
    std::lock_guard<std::mutex> lock(outgoing_mutex);
    if (outgoing.size() < max_outgoing) {
      outgoing.push_back(std::move(packet));
    }
  }

  fan::event::task_t sync() {
    int wanted = wanted_channel.load();
    int active = active_channel.load();
    try {
      if (active != -1 && !ecps_backend.voice_known(channel_id_t((uint16_t)active))) {
        active_channel.store(active = -1);
      }
      if (active != -1 && active != wanted) {
        active_channel.store(-1);
        co_await ecps_backend.send_voice_join(channel_id_t((uint16_t)active), false);
        active = -1;
      }
      if (wanted != -1 && active == -1 && ecps_backend.voice_known(channel_id_t((uint16_t)wanted))) {
        co_await ecps_backend.send_voice_join(channel_id_t((uint16_t)wanted), true);
        active_channel.store(active = wanted);
      }
      co_await ecps_backend.probe_voice();
    }
    catch (...) {}

    std::vector<std::vector<std::uint8_t>> packets;
    {
      std::lock_guard<std::mutex> lock(outgoing_mutex);
      packets.swap(outgoing);
    }
    for (const auto& packet : packets) {
      if (active != -1) {
        co_await ecps_backend.write_voice(channel_id_t((uint16_t)active), packet.data(), packet.size());
      }
    }
  }

  void on_packet(channel_id_t channel_id, std::uint32_t session, const std::uint8_t* data, std::uintptr_t size) {
    if (active_channel.load() != (int)channel_id.i || deafened.load()) {
      return;
    }
    std::lock_guard<std::mutex> lock(speakers_mutex);
    auto& speaker = speakers[session];
    if (!speaker) {
      speaker = std::make_unique<speaker_t>();
      speaker->decoder.open();
    }
    if (!speaker->decoder.state) {
      return;
    }
    std::uint16_t sequence = ((const header_t*)data)->Sequence;
    std::int16_t gap = (std::int16_t)(sequence - speaker->expected_sequence);
    if (speaker->receiving && gap < 0 && gap > -max_late) {
      return;
    }
    std::array<f32_t, fan::audio::stream_frame_size * fan::audio::stream_channels> pcm;
    for (std::int16_t i = 0; speaker->receiving && i < std::min<std::int16_t>(gap, max_concealed); ++i) {
      if (speaker->decoder.conceal(pcm.data()) > 0) {
        mixer.push(session, pcm.data(), fan::audio::stream_frame_size);
      }
    }
    speaker->receiving = true;
    speaker->expected_sequence = sequence + 1;
    std::int32_t frames = speaker->decoder.decode(data + sizeof(header_t), (std::int32_t)(size - sizeof(header_t)), pcm.data());
    if (frames > 0) {
      mixer.push(session, pcm.data(), frames);
      speaker->heard_at_ms = now_ms();
    }
  }

  bool is_talking(std::uint32_t session) {
    std::int64_t now = now_ms();
    if (session == (std::uint32_t)ecps_backend.session_id.i) {
      return capture.is_running() && now - loud_at_ms.load(std::memory_order_relaxed) <= hold_ms;
    }
    std::lock_guard<std::mutex> lock(speakers_mutex);
    auto it = speakers.find(session);
    return it != speakers.end() && now - it->second->heard_at_ms <= hold_ms;
  }

  static constexpr std::int32_t voice_bitrate = 40000;
  static constexpr std::int64_t hold_ms = 400;
  static constexpr std::size_t max_outgoing = 25;
  static constexpr std::int16_t max_concealed = 3;
  static constexpr std::int16_t max_late = 50;

  fan::audio::microphone_capture_t capture;
  fan::audio::opus_encoder_t encoder;
  fan::audio::echo_canceller_t echo;
  fan::audio::noise_suppressor_t noise;
  fan::audio::live_mixer_t mixer;
  std::future<bool> mixer_opening;
  bool mixer_open = false;
  bool mixer_failed = false;
  std::string output_device;
  std::atomic<bool> echo_cancel = false;
  bool echo_running = false;
  std::atomic<bool> noise_suppress = true;
  bool noise_running = false;
  std::atomic<f32_t> level = 0.f;
  std::mutex outgoing_mutex;
  std::vector<std::vector<std::uint8_t>> outgoing;
  std::uint16_t send_sequence = 0;
  std::mutex speakers_mutex;
  std::unordered_map<std::uint32_t, std::unique_ptr<speaker_t>> speakers;
  std::atomic<int> wanted_channel = -1;
  std::atomic<int> active_channel = -1;
  std::atomic<bool> muted = false;
  std::atomic<bool> deafened = false;
  std::atomic<f32_t> threshold = 0.02f;
  std::atomic<std::int64_t> loud_at_ms = 0;
  std::string device;
  bool capture_failed = false;
}voice_chat;

// MIDI a viewer plays is synthesized here, so the host needs no MIDI device or loopback driver.
// The pump thread only runs while there is sound, a note-off that never arrives dies out by itself.
struct midi_host_t {
  ~midi_host_t() {
    stop();
  }

  void play(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    if (!start()) {
      return;
    }
    synth.midi(status, data1, data2);
  }

  void silence() {
    if (running.load()) {
      synth.all_notes_off();
    }
  }

  bool start() {
    if (running.load()) {
      return true;
    }
    if (failed) {
      return false;
    }
    player.target_frames = block_frames * 2;
    player.max_frames = block_frames * 12;
    if (!player.start()) {
      failed = true;
      fan::print("midi: no audio output to play the viewer's notes on");
      return false;
    }
    running = true;
    // the steinway recordings take a few seconds to decode, until then notes use the plain tone
    loader = std::thread([this] {
      auto directory = fan::audio::midi_sac_bank_t::find_directory();
      if (directory.empty()) {
        fan::print("midi: audio/steinway keys not found (set ECPS_PIANO_DIR), using the built-in tone");
        return;
      }
      std::size_t loaded = fan::audio::midi_sac_bank_t::load_all(directory, synth);
      fan::print("midi: loaded", loaded, "steinway keys");
    });
    pump = std::thread([this] {
      std::array<f32_t, block_frames * fan::audio::stream_channels> block;
      while (running.load()) {
        bool sounding = synth.active_voices() != 0;
        if (sounding || player.get_buffered_frames() != 0) {
          while (running.load() && player.get_buffered_frames() < block_frames * 3) {
            block.fill(0.f);
            synth.render(block.data(), block_frames);
            player.push(block.data(), block_frames);
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
      }
    });
    return true;
  }

  void stop() {
    if (!running.exchange(false)) {
      return;
    }
    if (pump.joinable()) {
      pump.join();
    }
    if (loader.joinable()) {
      loader.join();
    }
    player.stop();
  }

  static constexpr std::uint32_t block_frames = 240; // 5 ms
  fan::audio::midi_synth_t synth;
  fan::audio::live_player_t player;
  std::thread pump;
  std::thread loader;
  std::atomic<bool> running = false;
  bool failed = false;
}midi_host;

#include "remote_logs.h"
remote_logs_t remote_logs;

#include "updater.h"
updater_t updater;

struct dynamic_config_t {
  static uint32_t get_target_framerate();

  static uint32_t pick_by_fps(uint32_t at_144, uint32_t at_120, uint32_t at_90, uint32_t at_60, uint32_t below) {
    uint32_t fps = get_target_framerate();
    return fps >= 144 ? at_144 : fps >= 120 ? at_120 : fps >= 90 ? at_90 : fps >= 60 ? at_60 : below;
  }

  static uint64_t get_adaptive_bucket_size() {
    return get_adaptive_bitrate() / 4;
  }

  static uint64_t get_adaptive_bitrate();

  static size_t get_adaptive_queue_size() {
    return pick_by_fps(10, 8, 6, 4, 3);
  }

  static size_t get_adaptive_chunk_count() {
    return pick_by_fps(50, 40, 30, 25, 20);
  }

  static f32_t get_adaptive_bucket_multiplier() {
    return 1.5f;
  }
};

// What happens to the encoded frames on the way out, for the perf log. Written by the encode thread and the network
// loop, read and reset once a second by the gui (perf_log_tick)
struct send_debug_t {
  std::atomic<std::uint64_t> queued{0};        // frames put in the send queue
  std::atomic<std::uint64_t> sent{0};          // frames sent whole
  std::atomic<std::uint64_t> flushed{0};       // frames thrown away from the queue when a keyframe came
  std::atomic<std::uint64_t> backlog_idrs{0};  // keyframes asked for because the queue was long
  std::atomic<std::uint64_t> lock_misses{0};   // frames lost because the queue was locked too long
  std::atomic<std::uint64_t> passes{0};        // send passes with something to send
  std::atomic<std::uint64_t> chunks{0};        // 1 KB packets sent
  std::atomic<std::uint64_t> cap_stops{0};     // passes that stopped at the packet limit with frames left
  std::atomic<std::uint64_t> bucket_stops{0};  // passes that stopped because the bitrate budget was used up
  std::atomic<std::uint64_t> send_us{0};       // time spent sending
  std::atomic<std::uint64_t> max_send_us{0};   // longest single pass
  std::atomic<std::uint64_t> max_gap_us{0};    // longest wait between passes while frames waited
  std::atomic<std::uint64_t> max_queue{0};     // most frames waiting at once
  std::atomic<std::uint64_t> queued_bytes_total{0}; // never reset: the send pacing reads the encoder's real rate from it

  static void raise(std::atomic<std::uint64_t>& value, std::uint64_t to) {
    std::uint64_t now = value.load(std::memory_order_relaxed);
    while (to > now && !value.compare_exchange_weak(now, to, std::memory_order_relaxed)) {}
  }
}send_debug;

struct render_thread_t {
  render_thread_t() {
    window_icon = engine.image_load("icons/ecps_logo.png");

    request_precise_timers();
    // started again by the in-app update (it leaves this file for the new process): the window comes back without
    // taking the focus from what the user is doing. A start by hand shows it focused as usual
    {
      std::error_code ec;
      if (std::filesystem::exists(updater_t::restarted_file, ec)) {
        engine.window.set_focus_on_show(false);
      }
    }
    engine.set_target_fps(0, false);
    engine.set_window_name("ECPS - Every Communication Program Sucks");
    // image_load is async now, so read the icon pixels synchronously for glfw
    if (auto icon = fan::image::load_owned("icons/ecps_logo.png"); icon.valid() && icon.channels == 4) {
      fan::image::info_t icon_info;
      icon_info.data = icon.data.get();
      icon_info.size = fan::vec2i(icon.size.x, icon.size.y);
      icon_info.channels = icon.channels;
      engine.set_window_icon(icon_info);
    }

    if (!screen_encoder.open()) {
      fan::print("Failed to open modern encoder");
    }
    screen_encoder.set_user_resolution(1280, 720);

    if (!screen_decoder.open()) {
      fan::print("Failed to open modern decoder");
    }
    screen_decoder.reload_codec_cb = [this]() {
      ecps_gui.request_idr_reset();
    };
  }

  engine_t engine;
  #define engine OFFSETLESS(This, render_thread_t, ecps_gui)->engine
  #include "gui.h"
  ecps_gui_t ecps_gui;

  fan::graphics::image_t window_icon;

  fan::graphics::screen_encode_t screen_encoder;
  fan::graphics::screen_decode_t screen_decoder;

  std::atomic<bool> should_stop {false};

  fan::graphics::universal_image_renderer_t network_frame {{
      .position = fan::vec3(fan::vec2(0), 1),
      .size = gloco()->window.get_size() / 2,
  }};
  f32_t displayed_fps = 0.0f;

  fan::vec2ui host_mouse_pos;

  // The host's mouse pointer as the host reports it: where it is on the captured screen (0..1) and whether it is
  // showing at all. A game that hides the pointer hides it here too. Not set when the host is a Linux one,
  // there the picture itself has the pointer in it
  struct host_cursor_t {
    std::atomic<f32_t> x {0.f};
    std::atomic<f32_t> y {0.f};
    std::atomic<bool> visible {false};
    std::atomic<std::int64_t> updated_ms {0};
  }host_cursor;

  void render(auto l) {
    if (engine.process_frame([this, l] { ecps_gui.render(); l(); })) {
      should_stop.store(true);
      stop_worker_threads();
      std::exit(0);
    }
  }

  latest_frame_t latest_frame;

};

static fan::vec2i capture_origin(render_thread_t* rt) {
#if defined(_WIN32)
  return rt->screen_encoder.mdscr.desktop_origin;
#else
  return fan::vec2i(0, 0);
#endif
}

// the host's own setting decides, the server can't turn control on by itself
static bool host_allows_input(ecps_backend_t& backend, auto channel_id) {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  bool allowed = rt && rt->ecps_gui.stream_settings.input_control != 0 && !rt->ecps_gui.stream_settings.share_camera &&
    std::any_of(backend.channel_info.begin(), backend.channel_info.end(), [&](const auto& channel) {
      return channel.is_streaming && channel.channel_id.i == channel_id.i;
    });
  if (!allowed) {
    host_input.close();
    return false;
  }
  static bool reported = false;
  if (!host_input.is_open() && !host_input.open()) {
    if (!reported) {
      fan::print("input control: no way to inject input here, on wayland /dev/uinput must be writable");
    }
    reported = true;
    return false;
  }
  reported = false;
  auto& geometry = rt->screen_encoder.mdscr.Geometry;
  fan::vec2i origin = capture_origin(rt);
  host_input.set_desktop(origin.x, origin.y, geometry.Resolution.x, geometry.Resolution.y);
  return true;
}

// viewers send stream pixels, the stream can be smaller than the desktop
static bool stream_to_desktop(const fan::vec2si& position, std::int32_t& x, std::int32_t& y) {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (!rt) {
    return false;
  }
  auto& config = rt->screen_encoder.config_;
  std::int64_t desktop_width = rt->screen_encoder.mdscr.Geometry.Resolution.x;
  std::int64_t desktop_height = rt->screen_encoder.mdscr.Geometry.Resolution.y;
  if (position.x < 0 || position.y < 0 || !desktop_width || !desktop_height || !config.width || !config.height) {
    return false;
  }
  fan::vec2i origin = capture_origin(rt);
  x = origin.x + (std::int32_t)std::min<std::int64_t>((std::int64_t)position.x * desktop_width / config.width, desktop_width - 1);
  y = origin.y + (std::int32_t)std::min<std::int64_t>((std::int64_t)position.y * desktop_height / config.height, desktop_height - 1);
  return true;
}

// every handler reads its own message first. A failed read means the connection is going down,
// there is nothing in the message to act on then
#define s2c(name) __dme_get(Protocol_S2C, name) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t
#define s2c_read(name) \
  auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::name##_t>(); \
  if (msg.status != 0) { \
    co_return; \
  }
// messages another part of the client handles whole
#define s2c_forward(name, call) s2c(name) { \
    s2c_read(name); \
    call; \
    co_return; \
  }

static void encoder_force_keyframe() {
  if (auto* rt = get_render_thread()) {
    rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
  }
}

static bool is_midi_voice_message(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
  return status >= 0x80 && status < 0xF0 && !(data1 & 0x80) && !(data2 & 0x80);
}

static void play_midi(bool allowed, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
  if (!allowed) {
    midi_host.silence();
  }
  // the server checks too, but this is the player's own machine
  else if (is_midi_voice_message(status, data1, data2)) {
    midi_host.play(status, data1, data2);
  }
}

ecps_backend_t::ecps_backend_t() {
  s2c(KeepAlive) {
    co_return;
  };

  s2c(KeyExchange) {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::KeyExchange_t>();
    fan::network::crypto::x25519_key_t server_ephemeral, server_static;
    fan::network::crypto::session_keys_t keys;
    bool ok = msg.status == 0;
    if (ok) {
      std::memcpy(server_ephemeral.data(), msg->ServerEphemeral, server_ephemeral.size());
      std::memcpy(server_static.data(), msg->ServerStatic, server_static.size());
      ok = fan::network::crypto::client_handshake(backend.handshake_private, backend.handshake_public, server_ephemeral, server_static, Protocol::CryptoContext, keys)
        && fan::network::crypto::confirm_equal(keys.confirm, msg->Confirm)
        && backend.verify_server_key(server_static);
    }
    fan::network::crypto::cleanse(backend.handshake_private.data(), backend.handshake_private.size());
    if (!ok) {
      backend.mark_disconnected("server key exchange failed");
      co_return;
    }
    backend.udp_crypto.set_client(keys);
    backend.tcp_client.enable_secure(keys.tcp_c2s, keys.tcp_s2c);
    backend.tcp_secure_ready = true;
    fan::network::crypto::cleanse(&keys, sizeof(keys));
  };

  s2c(Response_Login) {
    s2c_read(Response_Login);
    backend.session_id = msg->SessionID;
    co_await backend.udp_write(0, ProtocolUDP::C2S_t::KeepAlive, {}, 0, 0);
  };

  s2c(CreateChannel_OK) {
    s2c_read(CreateChannel_OK);
    backend.finish_request(base.ID, msg->ChannelID);
    co_return;
  };

  s2c(CreateChannel_Error) {
    s2c_read(CreateChannel_Error);
    fan::print("the server refused to create the channel:", Protocol::JoinChannel_Error_Reason_String[(std::uint8_t)msg->Reason % 4]);
    backend.finish_request(base.ID, Protocol_ChannelID_t((uint16_t)-1));
    co_return;
  };

  s2c(JoinChannel_OK) {
    s2c_read(JoinChannel_OK);
    // the join this answers, by its request: an auto join without a password can be on its way next to the one with it
    ecps_backend_t::channel_info_t* answered = nullptr;
    for (auto& entry : backend.channel_info) {
      if (entry.join_request_id == base.ID && entry.channel_id.i == msg->ChannelID.i) {
        answered = &entry;
      }
    }
    if (auto* channel = answered ? answered : backend.find_channel(msg->ChannelID)) {
      channel->session_id = msg->ChannelSessionID;
      channel->join_confirmed = true;
      // the password worked, it is not asked for again
      if (!channel->password.empty()) {
        backend.remember_password(backend.channel_name_of(channel->channel_id), channel->password);
        channel->password.clear();
      }
    }
    encoder_force_keyframe();
    co_return;
  };

  s2c(JoinChannel_Error) {
    s2c_read(JoinChannel_Error);
    // the channel was added when the join was sent, it has to go again
    for (const auto& channel : backend.channel_info) {
      if (channel.join_request_id == base.ID) {
        fan::print("could not join channel", (uint64_t)channel.channel_id.i, ":", Protocol::JoinChannel_Error_Reason_String[(std::uint8_t)msg->Reason % 4]);
        backend.did_just_join = false;
        auto channel_id = channel.channel_id;
        std::string name = backend.channel_name_of(channel_id);
        // only this join goes: another one for the same channel (the password typed in) may still be on its way
        bool other_join = std::any_of(backend.channel_info.begin(), backend.channel_info.end(), [&](const auto& entry) {
          return entry.channel_id.i == channel_id.i && entry.join_request_id != base.ID;
        });
        if (other_join) {
          auto request = base.ID;
          backend.channel_info.erase(std::remove_if(backend.channel_info.begin(), backend.channel_info.end(),
            [&](const auto& entry) { return entry.join_request_id == request; }), backend.channel_info.end());
          break;
        }
        backend.forget_channel(channel_id);
        if (msg->Reason == Protocol::JoinChannel_Error_Reason_t::PasswordRequired ||
          msg->Reason == Protocol::JoinChannel_Error_Reason_t::WrongPassword) {
          bool wrong = msg->Reason == Protocol::JoinChannel_Error_Reason_t::WrongPassword;
          if (wrong) {
            backend.forget_password(name);
          }
          std::lock_guard<std::mutex> lock(backend.password_mutex);
          if (!backend.password_prompt.wanted || wrong) {
            backend.password_prompt = { true, wrong, channel_id, name };
          }
        }
        break;
      }
    }
    co_return;
  };

  s2c(Channel_ScreenShare_ViewToShare) {
    s2c_read(Channel_ScreenShare_ViewToShare);
    if (backend.is_channel_streaming(msg->ChannelID) && (msg->Flag & ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR)) {
      encoder_force_keyframe();
    }
    co_return;
  };

  s2c(ChannelList) {
    s2c_read(ChannelList);
    uint16_t channel_count = msg->ChannelCount;
    std::vector<ecps_backend_t::channel_list_info_t> channels;
    channels.reserve(channel_count);

    for (uint16_t i = 0; i < channel_count; ++i) {
      auto channel_info = co_await backend.tcp_client.read<Protocol_S2C_t::ChannelInfo_t>();
      if (channel_info.status != 0) {
        co_return;
      }
      ecps_backend_t::channel_list_info_t info;
      info.channel_id = channel_info->ChannelID;
      info.type = channel_info->Type;
      info.user_count = channel_info->UserCount;
      info.name = std::string(channel_info->Name, strnlen(channel_info->Name, 63));
      info.is_password_protected = (channel_info->IsPasswordProtected != 0);
      info.host_session_id = channel_info->HostSessionID;
      channels.push_back(info);
    }
    backend.available_channels = std::move(channels);
    backend.channel_list_received = true;
  };

  s2c(ChannelSessionList) {
    s2c_read(ChannelSessionList);
    Protocol_ChannelID_t channel_id = msg->ChannelID;
    uint16_t session_count = msg->SessionCount;
    // keep the old list until the new one is fully read, the gui draws from it between reads
    std::vector<ecps_backend_t::session_info_t> sessions;
    sessions.reserve(session_count);

    for (uint16_t i = 0; i < session_count; ++i) {
      auto session_info = co_await backend.tcp_client.read<Protocol_S2C_t::SessionInfo_t>();
      if (session_info.status != 0) {
        co_return;
      }
      ecps_backend_t::session_info_t info;
      info.session_id = session_info->SessionID;
      info.channel_session_id = session_info->ChannelSessionID;
      info.account_id = session_info->AccountID;
      info.username = std::string((const char*)session_info->Username, strnlen((const char*)session_info->Username, 31));
      info.is_host = (session_info->IsHost != 0);
      info.joined_at = session_info->JoinedAt;
      sessions.push_back(info);
    }
    backend.channel_sessions[channel_id.i] = std::move(sessions);
    for (auto& channel : backend.available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        channel.user_count = session_count;
        break;
      }
    }
  };

  // input
  s2c(Channel_ScreenShare_View_InformationToViewSetFlag) {
    s2c_read(Channel_ScreenShare_View_InformationToViewSetFlag);
    if (auto* channel = backend.find_channel(msg->ChannelID)) {
      channel->flag = msg->Flag;
    }
    co_return;
  };

  s2c(Channel_ScreenShare_View_InformationToViewMouseCoordinate) {
    s2c_read(Channel_ScreenShare_View_InformationToViewMouseCoordinate);
    backend.update_host_mouse_coordinate(msg->ChannelID, msg->pos);
    co_return;
  };

  s2c(Channel_Voice_Members) {
    s2c_read(Channel_Voice_Members);
    if (!backend.is_channel_joined(msg->ChannelID)) {
      co_return;
    }
    std::vector<std::uint32_t> members;
    for (std::uint8_t i = 0; i < msg->Count && i < 16; ++i) {
      members.push_back((std::uint32_t)msg->Members[i].i);
    }
    std::lock_guard<std::mutex> lock(backend.voice_mutex);
    backend.voice_members[(std::uint32_t)msg->ChannelID.i] = std::move(members);
    co_return;
  };

  s2c(Channel_Chat_Message) {
    s2c_read(Channel_Chat_Message);
    if (!backend.is_channel_joined(msg->ChannelID)) {
      co_return;
    }
    ecps_backend_t::chat_message_t message;
    message.id = msg->Id;
    message.from = (std::uint32_t)msg->From.i;
    message.name = std::string(msg->Name, strnlen(msg->Name, sizeof(msg->Name)));
    message.time = msg->Time;
    message.text = std::string(msg->Text, std::min<std::size_t>(msg->Length, sizeof(msg->Text)));
    backend.on_chat_message(msg->ChannelID, std::move(message));
    co_return;
  };

  s2c(Channel_Chat_History_End) {
    s2c_read(Channel_Chat_History_End);
    if (backend.is_channel_joined(msg->ChannelID)) {
      backend.on_chat_known(msg->ChannelID);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_View_Streamers) {
    s2c_read(Channel_ScreenShare_View_Streamers);
    std::vector<std::uint32_t> streamers;
    for (std::uint8_t i = 0; i < msg->Count && i < 8; ++i) {
      streamers.push_back((std::uint32_t)msg->Streamers[i].i);
    }
    std::lock_guard<std::mutex> lock(backend.streamers_mutex);
    backend.channel_streamers[(std::uint32_t)msg->ChannelID.i] = std::move(streamers);
    co_return;
  };

  s2c(SetUsername_Result) {
    s2c_read(SetUsername_Result);
    std::string name((const char*)msg->Name, strnlen((const char*)msg->Name, sizeof(msg->Name)));
    {
      std::lock_guard<std::mutex> lock(backend.username_mutex);
      backend.username_result_name = name;
      if (!name.empty()) {
        backend.my_username = name;
      }
    }
    backend.username_result.store(msg->Result);
    backend.username_result_seq.fetch_add(1);

    // the first one comes unasked right after login and shows this is a newer server: now the saved name can be
    // asked for, and the other newer messages may be sent
    if (!backend.server_new.exchange(true) && !backend.desired_username.empty() && backend.desired_username != name) {
      co_await backend.send_set_username(backend.desired_username);
    }
  };

  s2c(Channel_ScreenShare_View_InformationToViewCursor) {
    s2c_read(Channel_ScreenShare_View_InformationToViewCursor);
    auto* rt = get_render_thread();
    if (rt && backend.is_channel_viewing(msg->ChannelID)) {
      rt->host_cursor.x.store(msg->X / 65535.f);
      rt->host_cursor.y.store(msg->Y / 65535.f);
      rt->host_cursor.visible.store(msg->Visible != 0);
      rt->host_cursor.updated_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMouseCoordinate) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMouseCoordinate);
    std::int32_t x, y;
    if (host_allows_input(backend, msg->ChannelID) && stream_to_desktop(msg->pos, x, y)) {
      host_input.move(x, y);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMouseMotion) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMouseMotion);
    if (host_allows_input(backend, msg->ChannelID)) {
      host_input.move_relative(msg->Motion.x, msg->Motion.y);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMouseButton) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMouseButton);
    if (host_allows_input(backend, msg->ChannelID)) {
      std::int32_t x, y;
      if (stream_to_desktop(msg->pos, x, y)) {
        host_input.move(x, y);
      }
      host_input.button(msg->key, msg->state);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostKeyboard) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostKeyboard);
    if (host_allows_input(backend, msg->ChannelID) && !host_input.key(msg->Scancode, msg->State)) {
      ecps_print_throttled("input control: can't press scancode {:#x}", msg->Scancode);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMidi) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMidi);
    auto* rt = get_render_thread();
    // the host's own setting decides, and only for a channel it hosts. No stream needed
    play_midi(rt && rt->ecps_gui.allow_midi && backend.is_hosting_channel(msg->ChannelID), msg->Status, msg->Data1, msg->Data2);
    co_return;
  };

  s2c(Channel_ScreenShare_View_InformationToViewMidi) {
    s2c_read(Channel_ScreenShare_View_InformationToViewMidi);
    auto* rt = get_render_thread();
    // being in the channel is enough, the host doesn't have to be streaming
    play_midi(rt && rt->ecps_gui.play_host_midi && backend.is_channel_joined(msg->ChannelID) &&
      !backend.is_hosting_channel(msg->ChannelID), msg->Status, msg->Data1, msg->Data2);
    co_return;
  };

  s2c(Channel_Log_Request) {
    s2c_read(Channel_Log_Request);
    co_await remote_logs.on_request(msg.data);
  };
  s2c_forward(Channel_Log_Status, remote_logs.on_status(msg.data));
  s2c_forward(Channel_Log_Data, remote_logs.on_data(msg.data));

  // file transfer
  s2c(Channel_File_Offer) {
    s2c_read(Channel_File_Offer);
    co_await backend.on_file_offer(msg.data);
  };
  s2c(Channel_File_Chunk) {
    s2c_read(Channel_File_Chunk);
    co_await backend.on_file_chunk(msg.data);
  };
  s2c_forward(Channel_File_Response, backend.on_file_response(msg.data));
  s2c_forward(Channel_File_Ack, backend.on_file_ack(msg.data));
  s2c_forward(Channel_File_Cancel, backend.on_file_cancel(msg.data));

  // a channel closed (its host left) or we were put out of it
  s2c_forward(KickedFromChannel, backend.forget_channel(msg->ChannelID));
}
#undef s2c
#undef s2c_read
#undef s2c_forward

fan::event::task_t ecps_backend_t::default_s2c_cb(ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) {
  co_await backend.tcp_client.read(backend.Protocol_S2C.NA(base.Command)->m_DSS);
  co_return;
}

void ecps_backend_t::update_host_mouse_coordinate(Protocol_ChannelID_t channel_id, const fan::vec2ui& pos) {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (!rt) return;

  for (auto& channel : channel_info) {
    if (channel.channel_id.i == channel_id.i && channel.is_viewing) {
      rt->host_mouse_pos = pos;
      break;
    }
  }
}

void ecps_backend_t::share_t::CalculateNetworkFlowBucket() {
  uintptr_t MaxBufferSize = (sizeof(ScreenShare_StreamHeader_Head_t) + 0x400) * 8;

  m_NetworkFlow.BucketSize = dynamic_config_t::get_adaptive_bucket_size();
  m_NetworkFlow.Bucket = m_NetworkFlow.BucketSize;

#if ecps_debug_prints >= 1
  uint32_t fps = dynamic_config_t::get_target_framerate();
  ecps_print_throttled("Adaptive bucket: {}fps -> {} bits ({} MB)",
    fps, m_NetworkFlow.BucketSize, m_NetworkFlow.BucketSize / 8 / 1024 / 1024);
#endif
}

void ecps_backend_t::view_t::RequestKeyframe() {
  // keyframes are big, asking for one per lost frame makes the congestion worse
  static auto last_request = std::chrono::steady_clock::time_point{};
  auto now = std::chrono::steady_clock::now();
  if (now - last_request < std::chrono::milliseconds(250)) {
    return;
  }
  last_request = now;

  if (auto* rt = get_render_thread()) {
    rt->ecps_gui.request_idr_reset();
  }
}

static bool is_displayable(const fan::graphics::screen_decode_t::decode_data_t& frame) {
  using image_format_e = fan::graphics::image_format_e;
  size_t planes = frame.pixel_format == image_format_e::yuv420p ? 3 : frame.pixel_format == image_format_e::nv12 ? 2 : 1;
  for (size_t i = 0; i < planes; ++i) {
    if (frame.data[i].empty()) {
      return false;
    }
  }
  return frame.image_size.x > 0 && frame.image_size.x <= 7680 &&
    frame.image_size.y > 0 && frame.image_size.y <= 4320 &&
    frame.stride[0].x >= frame.image_size.x;
}

static void decode_frame(render_thread_t* rt, std::vector<uint8_t>& frame_data) {
  try {
    auto frame = rt->screen_decoder.decode(frame_data.data(), frame_data.size(), rt->network_frame);
    if (frame.type == 1 && is_displayable(frame)) {
      rt->latest_frame.publish(frame);
      rt->screen_decoder.recycle(frame);
      ecps_backend.view_frames_decoded.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  catch (const std::exception& e) {
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Decode exception: {}", e.what());
  #endif
  }
  ecps_backend.view_decode_failed.fetch_add(1, std::memory_order_relaxed);
  ecps_backend.view.m_stats.Frame_Drop++;
}

void ecps_backend_t::view_t::WriteFramePacket() {
  if (m_Possible == 0) return;

  uint32_t FramePacketSize = (uint32_t)(this->m_Possible - 1) * 0x400 + this->m_ModuloSize;
  if (FramePacketSize < 32 || FramePacketSize > 0x500000) {
    this->m_stats.Frame_Drop++;
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Frame dropped - invalid size: {} bytes (valid range: 32 - 5MB)",
      FramePacketSize);
  #endif
    return;
  }

  if (this->m_data.size() < FramePacketSize) {
    this->m_stats.Frame_Drop++;
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Frame dropped - buffer underrun: need {} bytes, have {}",
      FramePacketSize, this->m_data.size());
  #endif
    return;
  }

#if ecps_debug_prints >= 2
  printf("NETWORK: Frame received at %lld ms, size: %u bytes\n",
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count(),
    FramePacketSize);
#endif

  ecps_backend.view_frames_assembled.fetch_add(1, std::memory_order_relaxed);
  if (!decode_queue.push(std::vector<uint8_t>(this->m_data.begin(), this->m_data.begin() + FramePacketSize))) {
    this->m_stats.Frame_Drop++;
    RequestKeyframe();
  }

  frame_index++;
}

void ecps_backend_t::view_t::FixFrameOnComplete() {
  // the first packet carries the packet count; without it the frame can't be sized
  m_stats.Frame_Total++;
  if (m_Possible == (uint16_t)-1) {
    m_stats.Frame_Drop++;
    m_stats.Packet_HeadDrop++;
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Frame dropped - first packet missing (seq {})", m_Sequence);
  #endif
    RequestKeyframe();
    return;
  }

  UpdateMissingPackets();
  if (m_MissingPackets.empty()) {
    // whole only because lost packets were asked for again
    m_stats.Frame_Corrected += m_RecoveryRetries != 0;
    WriteFramePacket();
    return;
  }

  m_stats.Frame_Drop++;
  m_stats.Packet_BodyDrop += m_MissingPackets.size();
#if ecps_debug_prints >= 1
  ecps_print_throttled("NETWORK: Frame dropped - missing {} of {} packets (seq {}), first missing: {}",
    m_MissingPackets.size(), m_Possible, m_Sequence, m_MissingPackets.front());
#endif
  m_Possible = (uint16_t)-1;
  RequestKeyframe();
}

uint32_t dynamic_config_t::get_target_framerate() {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (rt && rt->screen_encoder.config_.frame_rate > 0) {
    return rt->screen_encoder.config_.frame_rate;
  }
  return 60;
}

uint64_t dynamic_config_t::get_adaptive_bitrate() {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (rt && rt->ecps_gui.stream_settings.bitrate_mode == 1) {
    return rt->ecps_gui.stream_settings.bitrate_mbps * 1000000;
  }

  uint64_t bitrate = pick_by_fps(25000000, 20000000, 15000000, 12000000, 8000000);
  // the table is for 1080p. More pixels need more bits, but less than in proportion: 1440p gets 1.33x, 4k 2x
  if (rt) {
    f64_t pixels = (f64_t)rt->screen_encoder.config_.width * rt->screen_encoder.config_.height;
    bitrate = (uint64_t)(bitrate * std::clamp(std::sqrt(pixels / (1920.0 * 1080.0)), 1.0, 3.0));
  }
  return bitrate;
}

int main() {
  // from here on everything printed can be read by another client that is allowed to
  log_capture().start();
  fan::print(std::string("ecps client v") + ecps_version);
  ecps_backend.reconnected_cb = [](ecps_backend_t::Protocol_ChannelID_t channel_id) {
    if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
      rt->ecps_gui.selected_channel_id = channel_id;
    }
  };
  ecps_backend.audio_cb = [](const uint8_t* data, uintptr_t size) {
    audio_share.on_packet(data, size);
  };
  ecps_backend.voice_cb = [](ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t session, const uint8_t* data, uintptr_t size) {
    voice_chat.on_packet(channel_id, session, data, size);
  };
  ecps_backend.login_fail_cb = [](fan::exception_t e) {
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (rt) {// might be able to be removed
      rt->screen_decoder.graphics_queue_callback([e] {
        fan::printcl("failed to connect to server:"_str + e.reason + ", retrying...");
      });
    }
  };
  ecps_backend.view.recovery_callback = [](std::vector<uint8_t> request_data) {
    auto* rt = get_render_thread();
    if (rt) {
      rt->ecps_gui.backend_queue([request_data = std::move(request_data)]() -> fan::event::task_t {
        try {
          ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_RecoveryRequest_t rest;
          rest.ChannelID = ecps_backend_t::Protocol_ChannelID_t(0);
          rest.ChannelSessionID = ecps_backend_t::Protocol_ChannelSessionID_t(0);

          for (const auto& channel : ecps_backend.channel_info) {
            if (channel.is_viewing) {
              rest.ChannelID = channel.channel_id;
              rest.ChannelSessionID = channel.session_id;
              break;
            }
          }

          co_await ecps_backend.udp_write(
            0,
            ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_RecoveryRequest,
            rest,
            request_data.data(),
            request_data.size()
          );
        }
        catch (...) {}
      });
    }
  };

  ecps_backend.share.resend_packet_callback = [](const std::vector<uint8_t>& packet_data) {
    auto* rt = get_render_thread();
    if (rt) {
      rt->ecps_gui.backend_queue([packet_data]() -> fan::event::task_t {
        try {
          auto* channel = ecps_backend.streaming_channel();
          if (!channel) {
            co_return;
          }
          ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t rest;
          rest.ChannelID = channel->channel_id;
          rest.ChannelSessionID = channel->session_id;

          co_await ecps_backend.udp_write(
            0,
            ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData,
            rest,
            packet_data.data(),
            packet_data.size()
          );
        }
        catch (...) {}
      });
    }
  };

  std::promise<void> render_thread_promise;
  std::future<void> render_thread_future = render_thread_promise.get_future();

  // the engine builds its gui fonts on the render thread, so enable emojis before it starts
  fan::graphics::gui::load_emojis();

  fan::event::thread_create([&render_thread_promise] {
    render_thread_t render_thread_instance;

    render_thread_ptr.store(&render_thread_instance, std::memory_order_release);
    render_thread_ready.store(true, std::memory_order_release);
    render_thread_promise.set_value();

    while (!render_thread_instance.screen_decoder.is_initialized() &&
      !render_thread_instance.should_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (!render_thread_instance.engine.should_close() && !render_thread_instance.should_stop.load()) {
      {
        std::lock_guard<std::mutex> lock(render_thread_instance.screen_decoder.mutex);
        for (auto& i : render_thread_instance.screen_decoder.graphics_queue) {
          i();
        }
        render_thread_instance.screen_decoder.graphics_queue.clear();
      }

      // uploaded outside the lock, the decoder doesn't wait for the gpu
      static fan::graphics::screen_decode_t::decode_data_t shown_frame;
      if (render_thread_instance.latest_frame.take(shown_frame)) {
        static fan::time::timer display_fps_timer(1e+9, true);
        static int display_frame_count = 0;

        display_frame_count++;
        if (display_fps_timer.elapsed() >= 1e+9) {
          render_thread_instance.displayed_fps = static_cast<float>(display_frame_count) * 1e+9 / display_fps_timer.elapsed();
          display_frame_count = 0;
          display_fps_timer.restart();
        }

        if (ecps_backend.did_just_join && !ecps_backend.is_current_user_host_of_channel(ecps_backend.channel_info.back().channel_id)) {
          render_thread_instance.ecps_gui.window_handler.main_tab = 1;
          ecps_backend.did_just_join = false;
        }

        std::array<void*, 4> planes;
        for (size_t i = 0; i < planes.size(); ++i) {
          planes[i] = shown_frame.data[i].empty() ? nullptr : shown_frame.data[i].data();
        }
        try {
          stage_scope_t upload_scope {stage_times.upload};
          render_thread_instance.network_frame.set_tc_size(fan::vec2((f32_t)shown_frame.image_size.x / shown_frame.stride[0].x, 1));
          render_thread_instance.network_frame.reload(
            shown_frame.pixel_format,
            planes.data(),
            fan::vec2ui(shown_frame.stride[0].x, shown_frame.image_size.y)
          );
        }
        catch (const std::exception& e) {
        #if ecps_debug_prints >= 1
          ecps_print_throttled("RENDER: frame upload failed: {}", e.what());
        #endif
        }
      }

      render_thread_instance.render([] {});
      {
        static auto second_start = std::chrono::steady_clock::now();
        static auto last_frame = second_start;
        static int frames = 0;
        static f32_t slowest_ms = 0.f;
        auto now = std::chrono::steady_clock::now();
        slowest_ms = std::max(slowest_ms, std::chrono::duration<f32_t, std::milli>(now - last_frame).count());
        last_frame = now;
        ++frames;
        f32_t seconds = std::chrono::duration<f32_t>(now - second_start).count();
        if (seconds >= 1.f) {
          draw_rate.fps.store(frames / seconds, std::memory_order_relaxed);
          draw_rate.slowest_ms.store(slowest_ms, std::memory_order_relaxed);
          frames = 0;
          slowest_ms = 0.f;
          second_start = now;
        }
      }

      std::this_thread::yield();
    }

    // the workers hold a pointer to render_thread_instance, it may only go away after they did
    stop_worker_threads();
    render_thread_ptr.store(nullptr, std::memory_order_release);
    render_thread_ready.store(false, std::memory_order_release);
  });

  render_thread_future.wait();


  // counted before the thread exists, so the render thread can't miss a worker that is about to start
  worker_threads_active.fetch_add(1);
  fan::event::thread_create([] {
    worker_thread_scope_t worker_scope;
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    while (!rt || !rt->screen_encoder.encoder_.is_initialized()) {
      if (app_stopping.load()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rt = render_thread_ptr.load(std::memory_order_acquire);
    }

    lower_thread_priority();

    uint64_t frame_id = 0;
    bool first_frame = true;
    auto last_idr_time = std::chrono::steady_clock::now();
    auto last_backlog_idr = last_idr_time;
    auto& flow = ecps_backend.share.m_NetworkFlow;

    // one line every few seconds while sharing, says where frames stop: capture, encode or the network
    struct share_stats_t {
      uint64_t read_ok = 0, read_failed = 0, encode_failed = 0, frames = 0, bytes = 0;
      std::chrono::steady_clock::time_point printed = std::chrono::steady_clock::now();
    } stats;
    auto report_stats = [&] {
      auto stamp = std::chrono::steady_clock::now();
      if (stamp - stats.printed < std::chrono::seconds(3)) {
        return;
      }
      stats.printed = stamp;
      fan::print("share: captured", stats.read_ok, "capture_failed", stats.read_failed,
        "encode_failed", stats.encode_failed, "encoded", stats.frames, "(", stats.bytes, "bytes ) udp_packets",
        ecps_backend.stream_udp_packets.load(), "udp_bytes", ecps_backend.stream_udp_bytes.load(),
        "queued", flow.FrameList.Usage());
    };

    bool was_streaming = false;
    while (!app_stopping.load()) {
      rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt || rt->should_stop.load()) break;

      if (ecps_backend.is_streaming_to_any_channel()) {
        was_streaming = true;
      #if ecps_debug_prints >= 1
        report_stats();
      #endif
        if (first_frame) {
          rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
          first_frame = false;
          last_idr_time = std::chrono::steady_clock::now();
        }

        auto now = std::chrono::steady_clock::now();
        auto time_since_idr = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_idr_time);
        if (time_since_idr > std::chrono::milliseconds(5000)) {
          rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
          last_idr_time = now;
        }

      #if !defined(_WIN32)
        // "Change..." while sharing: the next read opens the share dialog again
        if (rt->ecps_gui.capture_reselect.exchange(false)) {
          close_capture(rt->screen_encoder);
        }
      #endif
        // waits for the next frame of the desktop, but a failed read returns at once
        if (!rt->screen_encoder.screen_read()) {
          ++stats.read_failed;
          rt->screen_encoder.sleep_thread();
          continue;
        }
        ++stats.read_ok;
        stage_times.capture.add(rt->screen_encoder.capture_time_ns);
      #if defined(_WIN32)
        if (rt->ecps_gui.capture_preview_due()) {
          auto& encoder = rt->screen_encoder;
          const std::uint8_t* frame = encoder.screen_buffer ? encoder.screen_buffer : encoder.mdscr.map_for_cpu(true);
          auto& geometry = encoder.mdscr.Geometry;
          rt->ecps_gui.store_capture_preview_now(frame, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
        }
      #endif
      #if !defined(_WIN32)
        {
          auto& geometry = rt->screen_encoder.mdscr.Geometry;
          rt->ecps_gui.store_capture_preview(rt->screen_encoder.screen_buffer, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
        }
      #endif

        {
          // the encoder is set to a landscape size, a portrait screen squeezed into it comes out stretched.
          // Keep the encoder's orientation the same as the capture's, the pixel count stays what was chosen
          auto& geometry = rt->screen_encoder.mdscr.Geometry;
          auto& encoder_config = rt->screen_encoder.config_;
          if (geometry.Resolution.x > 0 && geometry.Resolution.y > 0 && encoder_config.width > 0 && encoder_config.height > 0) {
            bool capture_portrait = geometry.Resolution.y > geometry.Resolution.x;
            bool encoder_portrait = encoder_config.height > encoder_config.width;
            if (capture_portrait != encoder_portrait) {
              rt->screen_encoder.set_user_resolution(encoder_config.height, encoder_config.width);
              rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
            }
          }
        }

        bool encoded;
        {
          stage_scope_t encode_scope {stage_times.encode};
          encoded = rt->screen_encoder.encode_write();
        }
        if (!encoded) {
          ++stats.encode_failed;
          rt->screen_encoder.sleep_thread();
          continue;
        }

      #if ecps_debug_prints >= 1
        {
          // one-off dump of the captured screen, to tell capture problems from encode/render ones
          static int capture_dump_frame = 0;
          auto& geometry = rt->screen_encoder.mdscr.Geometry;
          if (++capture_dump_frame == 60 && rt->screen_encoder.screen_buffer && geometry.Resolution.x && geometry.Resolution.y) {
            int dump_width = geometry.LineSize ? (int)geometry.LineSize / 4 : (int)geometry.Resolution.x;
            fan::image::write("ecps_capture.png", rt->screen_encoder.screen_buffer,
              fan::vec2i(dump_width, (int)geometry.Resolution.y), 4);
            fan::print("DEBUG: wrote ecps_capture.png (" + std::to_string(geometry.Resolution.x) + "x" +
              std::to_string(geometry.Resolution.y) + ", line size " + std::to_string(geometry.LineSize) + ")");
          }
        }
      #endif

        uint8_t* encoded_data = nullptr;
        size_t encoded_size = rt->screen_encoder.encode_read(&encoded_data);

        if (encoded_size > 0 && encoded_data) {
          ++stats.frames;
          ecps_backend.share_frames_encoded.fetch_add(1, std::memory_order_relaxed);
          stats.bytes += encoded_size;
          if (rt->ecps_gui.show_own_stream &&
            !decode_queue.push(std::vector<uint8_t>(encoded_data, encoded_data + encoded_size))) {
            rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
          }

          // encode_write already cleared the force_keyframe flag, the encoded frame is the source of truth
          const bool is_keyframe = rt->screen_encoder.current_packet_.is_keyframe;

          // the frames after this one depend on it, losing it silently would corrupt the picture
          std::unique_lock<std::timed_mutex> frame_list_lock(ecps_backend.share.frame_list_mutex, std::chrono::milliseconds(20));
          if (frame_list_lock.owns_lock()) {
            // a backlog is latency on the viewer side, a keyframe lets the queue be dropped safely
            auto backlog_now = std::chrono::steady_clock::now();
            // a few frames waiting is a burst the sender catches up on, dropping them all for a keyframe is worse:
            // only a queue of about a sixth of a second is let go
            std::size_t backlog_limit = std::max<std::size_t>(dynamic_config_t::get_adaptive_queue_size(),
              dynamic_config_t::get_target_framerate() / 6);
            if (!is_keyframe && flow.FrameList.Usage() > backlog_limit &&
              backlog_now - last_backlog_idr > std::chrono::seconds(1)) {
              rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
              last_backlog_idr = backlog_now;
              send_debug.backlog_idrs.fetch_add(1, std::memory_order_relaxed);
            }

            if (is_keyframe) {
              // a partly sent frame is dropped here, its sequence must not be reused
              auto head = flow.FrameList.GetNodeFirst();
              if (head != flow.FrameList.dst && flow.FrameList[head].SentOffset) {
                ++ecps_backend.share.frame_index;
              }
              send_debug.flushed.fetch_add(flow.FrameList.Usage(), std::memory_order_relaxed);
              while (flow.FrameList.Usage() > 0) {
                flow.FrameList.unlrec(flow.FrameList.GetNodeFirst());
              }
            }

            auto f = &flow.FrameList[flow.FrameList.NewNodeLast()];
            // current_packet_ is only read above, its buffer goes to the queue without a copy
            f->vec = std::move(rt->screen_encoder.current_packet_.data);
            f->SentOffset = 0;
            f->keyframe = is_keyframe;
            send_debug.queued.fetch_add(1, std::memory_order_relaxed);
            send_debug.queued_bytes_total.fetch_add(f->vec.size(), std::memory_order_relaxed);
            send_debug_t::raise(send_debug.max_queue, flow.FrameList.Usage());
          }
          else {
            rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
            send_debug.lock_misses.fetch_add(1, std::memory_order_relaxed);
          }
        }

        rt->screen_encoder.sleep_thread();
      }
      else {
        if (was_streaming) {
          // stopped: let go of the capture, so the next Start asks which screen or window to share again.
          // A source chosen with "Choose..." is kept, it is what the next share uses
          was_streaming = false;
        #if !defined(_WIN32)
          if (!rt->ecps_gui.capture_armed.load() || rt->screen_encoder.is_camera())
        #endif
          close_capture(rt->screen_encoder);
        }
        first_frame = true;
      #if defined(_WIN32)
        if (rt->ecps_gui.capture_preview_wanted.load()) {
          // not sharing, but the preview shows: frames are captured (with the HDR tone mapping) only for it, not
          // encoded, so it shows what would go out
          auto& encoder = rt->screen_encoder;
          if (encoder.screen_read() && rt->ecps_gui.capture_preview_due()) {
            const std::uint8_t* frame = encoder.screen_buffer ? encoder.screen_buffer : encoder.mdscr.map_for_cpu(true);
            auto& geometry = encoder.mdscr.Geometry;
            rt->ecps_gui.store_capture_preview_now(frame, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(std::max(5, 500 / std::clamp(rt->ecps_gui.preview_fps, 1, 60))));
          continue;
        }
      #endif
      #if !defined(_WIN32)
        if (rt->ecps_gui.capture_reselect.exchange(false)) {
          close_capture(rt->screen_encoder);
        }
        if (rt->ecps_gui.capture_armed.load() && !rt->screen_encoder.is_camera()) {
          // chosen but not shared yet: the first read opens the share dialog, the frames after it only feed the preview
          if (rt->screen_encoder.screen_read()) {
            auto& geometry = rt->screen_encoder.mdscr.Geometry;
            rt->ecps_gui.store_capture_preview(rt->screen_encoder.screen_buffer, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
      #endif
        rt->screen_encoder.poll_source();
        if (rt->screen_encoder.is_camera()) {
          close_capture(rt->screen_encoder);
        }
        const int fps = std::max<int>(rt->screen_encoder.config_.frame_rate, 1);
        const auto sleep_time = std::chrono::microseconds(1000000 / fps);
        std::this_thread::sleep_for(sleep_time);
      }
    }
  });

  worker_threads_active.fetch_add(1);
  fan::event::thread_create([] {
    worker_thread_scope_t worker_scope;
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    while (!rt || !rt->screen_decoder.is_initialized()) {
      if (app_stopping.load()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rt = render_thread_ptr.load(std::memory_order_acquire);
    }

    lower_thread_priority();

    std::vector<uint8_t> frame;
    auto view_report_time = std::chrono::steady_clock::now();
    uint64_t view_reported_packets = 0;
    while (!app_stopping.load()) {
      rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt || rt->should_stop.load()) break;

    #if ecps_debug_prints >= 1
      // while something is being received, one line every few seconds says how far the frames get
      if (auto stamp = std::chrono::steady_clock::now(); stamp - view_report_time >= std::chrono::seconds(3)) {
        view_report_time = stamp;
        uint64_t packets = ecps_backend.view_udp_packets.load();
        if (packets != view_reported_packets) {
          view_reported_packets = packets;
          fan::print("view: udp_packets", packets, "udp_bytes", ecps_backend.view_udp_bytes.load(),
            "frames_assembled", ecps_backend.view_frames_assembled.load(),
            "decoded", ecps_backend.view_frames_decoded.load(),
            "decode_failed", ecps_backend.view_decode_failed.load());
        }
      }
    #endif

      if (decode_queue.pop(frame)) {
        stage_scope_t decode_scope {stage_times.decode};
        decode_frame(rt, frame);
      }
    }
  });

  fan::event::task_idle([]() -> fan::event::task_t {
    try {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt || rt->should_stop.load()) {
        co_return;
      }

      constexpr size_t MAX_BATCH_SIZE = 4;
      std::vector<std::function<fan::event::task_t()>> local_tasks;

      {
        // backend_queue() pushes under task_queue_mutex, from the gui and the network thread
        std::lock_guard<std::mutex> task_lock(rt->ecps_gui.task_queue_mutex);
        if (!rt->ecps_gui.task_queue.empty()) {
          size_t batch_size = std::min(rt->ecps_gui.task_queue.size(), MAX_BATCH_SIZE);
          auto first = rt->ecps_gui.task_queue.begin();
          local_tasks.assign(std::make_move_iterator(first), std::make_move_iterator(first + batch_size));
          rt->ecps_gui.task_queue.erase(first, first + batch_size);
        }
      }

      for (const auto& f : local_tasks) {
        co_await f();
      }

      if (local_tasks.empty()) {
        co_await fan::co_sleep(1);
      }
    }
    catch (...) {
      fan::print("error");
    }
  });

  auto network_task = fan::event::every(1, []() -> fan::event::runv_t<bool> {
    co_await audio_share.send_pending();
    if (voice_chat.has_work()) {
      co_await voice_chat.sync();
    }
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (rt) {
      if (rt->ecps_gui.window_handler.auto_refresh) {
        static fan::time::timer refresh_timer(rt->ecps_gui.window_handler.refresh_interval * 2e+9, true);
        if (refresh_timer.finished()) {
          // the user list of the open channel needs refreshing too, otherwise
          // people who join after it was opened never show up
          auto selected_channel_id = rt->ecps_gui.selected_channel_id;
          rt->ecps_gui.backend_queue([selected_channel_id]() -> fan::event::task_t {
            try {
              co_await ecps_backend.request_channel_list();
              if (selected_channel_id.i != (uint16_t)-1) {
                co_await ecps_backend.request_channel_session_list(selected_channel_id);
              }
            }
            catch (...) {}
          });
          refresh_timer.restart();
        }
      }
    }

    if (ecps_backend.channel_info.empty()) {
      co_return 0;
    }

    static uint64_t skip_counter = 0;
    skip_counter++;

    if (skip_counter % 5 != 0) {
      auto flnr_check = ecps_backend.share.m_NetworkFlow.FrameList.GetNodeFirst();
      if (flnr_check == ecps_backend.share.m_NetworkFlow.FrameList.dst) {
        co_return 0;
      }
    }

    std::unique_lock<std::timed_mutex> frame_list_lock(ecps_backend.share.frame_list_mutex, std::try_to_lock);
    if (!frame_list_lock.owns_lock()) {
      co_return 0;
    }

    uint64_t ctime = fan::event::now();
    uint64_t DeltaTime = ctime - ecps_backend.share.m_NetworkFlow.TimerLastCallAt;

    if (DeltaTime < 100000) {
      frame_list_lock.unlock();
      co_return 0;
    }

    ecps_backend.share.m_NetworkFlow.TimerLastCallAt = ctime;

    f32_t bucket_multiplier = dynamic_config_t::get_adaptive_bucket_multiplier();

    if (!rt) {
      co_return 0;
    }

    // The pacing smooths bursts, it must not hold the stream below what the encoder makes: the encoder can go over its
    // target (high frame rates, keyframes), and a pace below that only fills the queue until frames are thrown away.
    // So the rate is the target or what came out of the encoder over the last half second, whichever is higher
    static uint64_t rate_window_start = ctime;
    static uint64_t rate_window_bytes = send_debug.queued_bytes_total.load(std::memory_order_relaxed);
    static double encoder_bps = 0.0;
    if (ctime - rate_window_start >= 500000000ull) {
      uint64_t total = send_debug.queued_bytes_total.load(std::memory_order_relaxed);
      encoder_bps = (total - rate_window_bytes) * 8.0 / ((ctime - rate_window_start) / 1e9);
      rate_window_bytes = total;
      rate_window_start = ctime;
    }
    double pace_bps = std::max<double>((double)rt->screen_encoder.config_.bitrate, encoder_bps);

    double seconds_elapsed = static_cast<double>(DeltaTime) / 1000000000.0;
    double bits_to_add = seconds_elapsed * pace_bps * bucket_multiplier;

    auto& flow = ecps_backend.share.m_NetworkFlow;
    // room for a quarter second at the pace, so a keyframe's debt is paid back in time
    flow.BucketSize = std::max<uint64_t>(dynamic_config_t::get_adaptive_bucket_size(), (uint64_t)(pace_bps / 4));
    flow.Bucket = std::min(flow.Bucket + static_cast<int64_t>(bits_to_add), static_cast<int64_t>(flow.BucketSize));

    if (flow.FrameList.Usage() > 0) {
      send_debug_t::raise(send_debug.max_gap_us, DeltaTime / 1000);
    }

    // several frames can go in one tick, a late tick must not lower the frame rate. The limit is what the bitrate
    // budget holds, not a fixed packet count: a fixed count capped the upload at count x ticks per second
    const size_t max_chunks = std::clamp<size_t>(
      (size_t)std::max<int64_t>(flow.Bucket, 0) / (0x400 * 8) + 1, dynamic_config_t::get_adaptive_chunk_count(), 512);
    size_t chunks_left = max_chunks;
    auto pass_start = std::chrono::steady_clock::now();
    std::uint64_t pass_chunks = 0;
    bool bucket_stop = false;

    while (chunks_left > 0) {
      auto flnr = flow.FrameList.GetNodeFirst();
      if (flnr == flow.FrameList.dst) {
        break;
      }

      auto f = &flow.FrameList[flnr];

      uint8_t Flag = f->keyframe ? ecps_backend_t::ProtocolChannel::ScreenShare::StreamHeadFlag::KeyFrame : 0;
      uint16_t Possible = (f->vec.size() / 0x400) + !!(f->vec.size() % 0x400);
      uint16_t sent_offset = f->SentOffset;
      bool is_likely_keyframe = f->keyframe || f->vec.size() > 30000;
      int64_t min_bucket = is_likely_keyframe ? -static_cast<int64_t>(flow.BucketSize) : 0;
      if (is_likely_keyframe) {
        chunks_left += max_chunks;
      }

      bool blocked = false;
      for (; sent_offset < Possible && chunks_left > 0; sent_offset++, chunks_left--) {
        uintptr_t DataSize = std::min<uintptr_t>(f->vec.size() - sent_offset * 0x400, 0x400);
        if (flow.Bucket - static_cast<int64_t>(DataSize * 8) < min_bucket) {
          bucket_stop = true;
          blocked = true;
          break;
        }
        if (co_await ecps_backend.write_stream(sent_offset, Possible, Flag, &f->vec[sent_offset * 0x400], DataSize)) {
          blocked = true;
          break;
        }
        flow.Bucket -= static_cast<int64_t>(DataSize * 8);
        ++pass_chunks;
      }

      f->SentOffset = sent_offset;

      if (blocked || sent_offset < Possible) {
        break;
      }
      flow.FrameList.unlrec(flnr);
      ++ecps_backend.share.frame_index;
      send_debug.sent.fetch_add(1, std::memory_order_relaxed);
    }

    if (pass_chunks || bucket_stop) {
      auto pass_us = (std::uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pass_start).count();
      send_debug.passes.fetch_add(1, std::memory_order_relaxed);
      send_debug.chunks.fetch_add(pass_chunks, std::memory_order_relaxed);
      send_debug.send_us.fetch_add(pass_us, std::memory_order_relaxed);
      send_debug_t::raise(send_debug.max_send_us, pass_us);
      if (bucket_stop) {
        send_debug.bucket_stops.fetch_add(1, std::memory_order_relaxed);
      }
      else if (chunks_left == 0 && flow.FrameList.Usage() > 0) {
        send_debug.cap_stops.fetch_add(1, std::memory_order_relaxed);
      }
    }

    co_return 0;
  });
  fan::event::loop();

  auto* rt_final = render_thread_ptr.load(std::memory_order_acquire);
  if (rt_final) {
    rt_final->should_stop.store(true);
  }
  // static destructors run in exit, nothing may still be using the globals by then
  stop_worker_threads();
  auto render_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (render_thread_ready.load() && std::chrono::steady_clock::now() < render_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::exit(0);
}
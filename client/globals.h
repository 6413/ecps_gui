#pragma once

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
import fan.graphics.stb;
using namespace fan::graphics;

#include <fan/graphics/types.h>
#include <fan/nativefiledialog/nfd.h>
#include "midi/midi_synth.h"
#include "midi/midi_input.h"
#include "midi/midi_worker.h"
#include "midi/midi_sac_bank.h"

// the gui fonts and their size are in fan's settings (Graphics), saved in fan_settings.json
static fan::graphics::gui::settings_menu_t* fan_settings_menu() {
  return gloco() ? static_cast<fan::graphics::gui::settings_menu_t*>(gloco()->get_settings_menu()) : nullptr;
}

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
inline struct decode_queue_t {
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

// the decoded frames waiting to be shown, oldest first. Frames often arrive in pairs (network, the sharer's timing):
// with one slot the first of a pair was overwritten before a draw took it, so 60 decoded showed as ~50. Three fit,
// since a window that draws exactly as often as the stream sends (60 frames on a 60 Hz screen) meets the frames with
// jitter and a late draw can find three waiting. When full, the oldest is dropped so the delay can't grow, and a
// queue that stays backed up (delay that never goes away at equal rates) has one frame skipped after a while.
// Frames swap in and out so the plane buffers get reused, a new 1080p frame would page fault 3 mb
struct latest_frame_t {
  using frame_t = fan::graphics::screen_decode_t::decode_data_t;
  static constexpr std::size_t slot_count = 3;
  // draws in a row that found more than one frame waiting before one is skipped to catch up
  static constexpr int backlog_draws_limit = 30;
  int backlog_draws = 0;

  void publish(frame_t& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (count == slot_count) {
      // the oldest goes back to the caller to be reused, the new one takes its place as the newest
      std::swap(slots[head], frame);
      head = (head + 1) % slot_count;
      ++dropped;
      return;
    }
    std::swap(slots[(head + count) % slot_count], frame);
    ++count;
  }
  bool take(frame_t& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (count == 0) {
      backlog_draws = 0;
      return false;
    }
    backlog_draws = count > 1 ? backlog_draws + 1 : 0;
    if (backlog_draws >= backlog_draws_limit) {
      // the oldest is a frame of delay kept for a second: skipped, the shown picture catches up by one
      head = (head + 1) % slot_count;
      --count;
      ++dropped;
      backlog_draws = 0;
    }
    std::swap(slots[head], frame);
    head = (head + 1) % slot_count;
    --count;
    return true;
  }

  std::mutex mutex;
  std::array<frame_t, slot_count> slots;
  std::size_t head = 0;
  std::size_t count = 0;
  std::uint64_t dropped = 0;
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
inline struct stage_times_t {
  stage_time_t capture, encode, decode, upload;
}stage_times;

// how often the window is drawn, measured over each second by the render loop: a stream decoded at 60 still
// looks like 15 when the window only draws 15 times a second
inline struct draw_rate_t {
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

inline std::atomic<render_thread_t*> render_thread_ptr {nullptr};

inline render_thread_t* get_render_thread() {
  return render_thread_ptr.load(std::memory_order_acquire);
}

#define render_thread get_render_thread()

inline std::atomic<bool> render_thread_ready {false};

// the encoder and decoder threads are detached and use the render thread's state and the globals.
// They must be gone before either is destroyed, so everything that ends the program stops them first
inline std::atomic<bool> app_stopping {false};
inline std::atomic<int> worker_threads_active {0};

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

// an image on the clipboard (a screenshot, an image copied in a browser or an editor) saved as a .png for the chat,
// empty if there is none. A PNG copy (browsers and the Snipping Tool put one there) is kept as it is. Any other image
// Windows gives as a bitmap: GetDIBits turns whatever it is (palette, 16, 24 or 32 bit, bitfields, bottom-up) into
// 32 bit pixels, and stb writes them as a PNG
static std::string clipboard_image_file() {
  static UINT png_format = RegisterClipboardFormatW(L"PNG");
  bool has_png = png_format && IsClipboardFormatAvailable(png_format);
  bool has_bitmap = IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB) ||
    IsClipboardFormatAvailable(CF_DIBV5);
  if (!has_png && !has_bitmap) {
    return {};
  }
  std::error_code ec;
  auto folder = std::filesystem::temp_directory_path(ec) / "ecps";
  std::filesystem::create_directories(folder, ec);
  auto path = folder / ("image-" + std::to_string((long long)std::time(nullptr)) + ".png");
  if (!OpenClipboard(nullptr)) {
    return {};
  }
  bool written = false;
  if (has_png) {
    if (HANDLE data = GetClipboardData(png_format)) {
      if (auto* memory = (const std::uint8_t*)GlobalLock(data)) {
        std::size_t size = GlobalSize(data);
        static const std::uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (size > 8 && std::memcmp(memory, signature, 8) == 0) {
          std::ofstream out(path, std::ios::binary);
          out.write((const char*)memory, (std::streamsize)size);
          written = (bool)out;
        }
        GlobalUnlock(data);
      }
    }
  }
  if (!written && has_bitmap) {
    // Windows makes a CF_BITMAP from a DIB on the clipboard by itself
    if (auto bitmap = (HBITMAP)GetClipboardData(CF_BITMAP)) {
      BITMAP description{};
      if (GetObjectW(bitmap, sizeof(description), &description) && description.bmWidth > 0 && description.bmHeight > 0) {
        int width = description.bmWidth;
        int height = description.bmHeight;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint8_t> pixels((std::size_t)width * height * 4);
        HDC screen = GetDC(nullptr);
        int rows = GetDIBits(screen, bitmap, 0, (UINT)height, pixels.data(), &info, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);
        if (rows == height) {
          // BGRA to RGBA. A bitmap without alpha leaves it 0, which would be fully transparent
          bool any_alpha = false;
          for (std::size_t i = 3; i < pixels.size(); i += 4) {
            if (pixels[i]) {
              any_alpha = true;
              break;
            }
          }
          for (std::size_t i = 0; i < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
            if (!any_alpha) {
              pixels[i + 3] = 255;
            }
          }
          fan::stb::write(path.string(), pixels.data(), fan::vec2i(width, height), 4);
          written = std::filesystem::file_size(path, ec) > 0 && !ec;
        }
      }
    }
  }
  CloseClipboard();
  return written ? path.string() : std::string();
}
#endif

#if !defined(_WIN32)
// an image on the clipboard on Linux: Wayland's wl-paste, or xclip on X11. The window library only gives text, so
// the programs are asked for a PNG. Empty if there is none, or neither program is installed
static std::string clipboard_image_file() {
  std::error_code ec;
  auto folder = std::filesystem::temp_directory_path(ec) / "ecps";
  std::filesystem::create_directories(folder, ec);
  auto path = folder / ("image-" + std::to_string((long long)std::time(nullptr)) + ".png");
  const std::string out = "\"" + path.string() + "\"";
  const char* commands[] = {
    "wl-paste --no-newline --type image/png",
    "xclip -selection clipboard -t image/png -o",
  };
  for (const char* command : commands) {
    // only when the clipboard really has an image: the programs say a wrong type with a failure and no output
    std::string full = std::string(command) + " > " + out + " 2>/dev/null";
    if (std::system(full.c_str()) != 0) {
      continue;
    }
    std::ifstream in(path, std::ios::binary);
    std::uint8_t head[8] = {};
    in.read((char*)head, 8);
    static const std::uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (in.gcount() == 8 && std::memcmp(head, signature, 8) == 0) {
      return path.string();
    }
  }
  std::filesystem::remove(path, ec);
  return {};
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

#include "frame_assembler.h"
#include "notify_sounds.h"
#include "backend.h"

inline fan::window::input_injector_t host_input;

inline struct audio_share_t {
  using header_t = ecps_backend_t::ScreenShare_AudioHeader_t;

  using capture_options_t = fan::audio::loopback_capture_t::options_t;

  void update(bool share, bool play, f32_t volume, const capture_options_t& options);


  // discord started or restarted since the capture began, so the excluded tree is stale
  bool discord_changed(const capture_options_t& options);


  void encode(const f32_t* pcm);


  fan::event::task_t send_pending();


  void on_packet(const std::uint8_t* data, std::uintptr_t size);


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

inline struct voice_chat_t {
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
    bool normalize = false;
  };
  struct speaker_t {
    fan::audio::opus_decoder_t decoder;
    std::uint16_t expected_sequence = 0;
    bool receiving = false;
    std::int64_t heard_at_ms = 0;
    // the peak of the last decoded packet
    f32_t level = 0.f;
    // voice normalization: how loud this one speaks (followed slowly, only while they are talking) and the gain
    // that brings it to a fixed level
    f32_t norm_level = 0.f;
    f32_t norm_gain = 1.f;

    // Brings the voice in pcm (samples, interleaved) towards a fixed level and returns the gain it was given.
    // Quiet below the gate is left alone, so the pauses between words and the noise in them are not boosted. The
    // gain falls quickly when someone gets loud and rises slowly when they are quiet, so it doesn't pump, and never
    // takes the sound past full scale
    f32_t normalize(f32_t* pcm, std::int32_t samples) {
      constexpr f32_t gate = 0.004f;       // about -48 dB
      constexpr f32_t target = 0.09f;      // about -21 dB, where speech sits comfortably
      constexpr f32_t min_gain = 0.35f;
      constexpr f32_t max_gain = 8.f;      // +18 dB
      if (samples <= 0) {
        return norm_gain;
      }
      f64_t sum = 0.0;
      f32_t peak = 0.f;
      for (std::int32_t i = 0; i < samples; ++i) {
        sum += (f64_t)pcm[i] * pcm[i];
        peak = std::max(peak, std::abs(pcm[i]));
      }
      f32_t rms = (f32_t)std::sqrt(sum / samples);
      if (rms > gate) {
        norm_level = norm_level <= 0.f ? rms : norm_level + (rms - norm_level) * (rms > norm_level ? 0.25f : 0.05f);
        f32_t wanted = std::clamp(target / norm_level, min_gain, max_gain);
        norm_gain += (wanted - norm_gain) * (wanted < norm_gain ? 0.2f : 0.03f);
      }
      f32_t gain = norm_gain;
      if (peak * gain > 0.97f) {
        gain = 0.97f / peak;
      }
      for (std::int32_t i = 0; i < samples; ++i) {
        pcm[i] *= gain;
      }
      return gain;
    }
  };

  static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void join(channel_id_t channel_id);

  void leave();

  bool is_wanted(channel_id_t channel_id) const;

  bool is_active() const;


  void update(const settings_t& settings);


  bool has_work() const;


  void encode(const f32_t* input);


  fan::event::task_t sync();


  void on_packet(channel_id_t channel_id, std::uint32_t session, const std::uint8_t* data, std::uintptr_t size);


  void set_user_volume(std::uint32_t session, f32_t volume);


  bool is_talking(std::uint32_t session);


  // how loud someone is right now, 0..1 on a dB scale (-50 dB..0 dB). Only what is sent or heard: 0 when silent
  f32_t talk_level(std::uint32_t session);


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
  std::atomic<bool> normalize = false;
  bool noise_running = false;
  std::atomic<f32_t> level = 0.f;
  std::mutex outgoing_mutex;
  std::vector<std::vector<std::uint8_t>> outgoing;
  std::uint16_t send_sequence = 0;
  std::mutex speakers_mutex;
  std::unordered_map<std::uint32_t, std::unique_ptr<speaker_t>> speakers;
  // local playback volume per person, kept by session while they talk; the GUI remembers it by name
  std::unordered_map<std::uint32_t, f32_t> speaker_volumes;
  std::atomic<int> wanted_channel = -1;
  std::atomic<int> active_channel = -1;
  std::atomic<bool> muted = false;
  // what the server was last told about the microphone: 0 or 1, -1 when not in the voice chat
  std::atomic<int> announced_muted{-1};
  std::atomic<bool> deafened = false;
  std::atomic<f32_t> threshold = 0.02f;
  std::atomic<std::int64_t> loud_at_ms = 0;
  std::string device;
  bool capture_failed = false;
}voice_chat;

// MIDI a viewer plays is synthesized here, so the host needs no MIDI device or loopback driver.
// The pump thread only runs while there is sound, a note-off that never arrives dies out by itself.
inline struct midi_host_t {
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
    #if defined(_WIN32)
      if (directory.empty()) {
        fan::print("midi: downloading the piano recordings, the plain tone plays until they are here");
        if (fan::audio::midi_sac_bank_t::download()) {
          directory = fan::audio::midi_sac_bank_t::find_directory();
        }
        else {
          fan::print("midi: could not download the piano recordings (ecps_piano.7z from the GitHub release)");
        }
      }
    #endif
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
inline remote_logs_t remote_logs;

#include "updater.h"
inline updater_t updater;

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
inline struct send_debug_t {
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
    engine.set_window_name("ECPS - Every Communication Program Sucks");    // image_load is async now, so read the icon pixels synchronously for glfw
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

  #include "gui.h"
  #undef engine
  engine_t engine;
  ecps_gui_t ecps_gui{*this};

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


// Shared application state implementation.
#include "gui_std.h" // precompiled, first
#define ECPS_CORE_ONLY
#include "app_state.h"


 fan::graphics::gui::settings_menu_t* fan_settings_menu() {
  return gloco() ? static_cast<fan::graphics::gui::settings_menu_t*>(gloco()->get_settings_menu()) : nullptr;
}


 std::vector<std::string> find_gpu_encoders(const std::vector<std::string>& compiled) {
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


bool decode_queue_t::push(std::vector<uint8_t> frame) {
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


bool decode_queue_t::pop(std::vector<uint8_t>& frame) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(100), [this] { return !frames.empty(); });
    if (frames.empty()) {
      return false;
    }
    frame = std::move(frames.front());
    frames.pop_front();
    return true;
  }


void latest_frame_t::publish(frame_t& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    if (count == slot_count) {
      // the oldest goes back to the caller to be reused, the new one takes its place as the newest
      std::swap(slots[head], frame);
      head = (head + 1) % slot_count;
      ++dropped;
      arrived.notify_one();
      return;
    }
    std::swap(slots[(head + count) % slot_count], frame);
    ++count;
    arrived.notify_one();
  }


bool latest_frame_t::take(frame_t& frame) {
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


void stage_time_t::add(std::uint64_t ns) {
    ms.store(ms.load(std::memory_order_relaxed) * 0.95f + (f32_t)ns / 1e6f * 0.05f, std::memory_order_relaxed);
  }


stage_scope_t::~stage_scope_t() {
    stage.add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
  }


 void lower_thread_priority() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}


 void request_precise_timers() {
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


 void stop_worker_threads() {
  app_stopping.store(true);
  // a worker stuck in a driver call must not keep the program from closing
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (worker_threads_active.load() > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}


midi_host_t::~midi_host_t() {
    stop();
  }


void midi_host_t::play(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    if (!start()) {
      return;
    }
    synth.midi(status, data1, data2);
  }


void midi_host_t::silence() {
    if (running.load()) {
      synth.all_notes_off();
    }
  }


bool midi_host_t::start() {
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


void midi_host_t::stop() {
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


void send_debug_t::raise(std::atomic<std::uint64_t>& value, std::uint64_t to) {
    std::uint64_t now = value.load(std::memory_order_relaxed);
    while (to > now && !value.compare_exchange_weak(now, to, std::memory_order_relaxed)) {}
  }


decode_queue_t decode_queue;
std::atomic<render_thread_t*> render_thread_ptr{nullptr};
std::atomic<bool> render_thread_ready{false};
std::atomic<bool> app_stopping{false};
std::atomic<int> worker_threads_active{0};
stage_times_t stage_times;
draw_rate_t draw_rate;
fan::window::input_injector_t host_input;
audio_share_t audio_share;
voice_chat_t voice_chat;
midi_host_t midi_host;
remote_logs_t remote_logs;
updater_t updater;
send_debug_t send_debug;

render_thread_t* get_render_thread() {
  return render_thread_ptr.load(std::memory_order_acquire);
}

// Everything of the client but the render thread and its gui: the backend, audio, voice chat, MIDI, the updater.
// Compiled as the module ecps.core (ecps_core.ixx); the sources that don't use the gui import only that, so an edit
// to a gui header doesn't recompile them
#pragma once

#include "gui_std.h"

#ifndef ECPS_APP_MODULE
import fan;
import fan.fmt;

import fan.graphics.video.screen;
import fan.graphics.stb;
using namespace fan::graphics;
#endif

#include "midi/midi_synth.h"
#include "midi/midi_input.h"
#include "midi/midi_worker.h"
#include "midi/midi_sac_bank.h"
#include "frame_assembler.h"
#include "notify_sounds.h"

// the gui fonts and their size are in fan's settings (Graphics), saved in fan_settings.json
fan::graphics::gui::settings_menu_t* fan_settings_menu();

#if defined(_WIN32)
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

// The graphics card encoders this computer can run, as "NVIDIA (h264_nvenc)": the encoder is in the bundled FFmpeg
// and the driver library it needs loads. Opening a real encoder for each would take long at start
std::vector<std::string> find_gpu_encoders(const std::vector<std::string>& compiled);


// frames wait here for the decoder thread, decoding on the network thread would
// leave incoming packets sitting in the socket while a frame is being decoded
struct decode_queue_t {
  static constexpr size_t max_size = 4;

  bool push(std::vector<uint8_t> frame);
  bool pop(std::vector<uint8_t>& frame);

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::vector<uint8_t>> frames;
};
extern decode_queue_t decode_queue;

// the game comes first when the cpu is full
void lower_thread_priority();

// the sender runs on a 1 ms event loop timer, which windows runs at 15.6 ms by default.
// behind a fullscreen game windows would also throttle the process, so opt out of that
void request_precise_timers();

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

  void publish(frame_t& frame);
  bool take(frame_t& frame);

  std::mutex mutex;
  std::array<frame_t, slot_count> slots;
  std::size_t head = 0;
  std::size_t count = 0;
  std::uint64_t dropped = 0;
};

// ms per frame, smoothed. the stream stats show these to tell what limits the frame rate
struct stage_time_t {
  void add(std::uint64_t ns);
  std::atomic<f32_t> ms = 0.f;
};
struct stage_scope_t {
  ~stage_scope_t();
  stage_time_t& stage;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
struct stage_times_t {
  stage_time_t capture, encode, decode, upload;
};
extern stage_times_t stage_times;

// how often the window is drawn, measured over each second by the render loop: a stream decoded at 60 still
// looks like 15 when the window only draws 15 times a second
struct draw_rate_t {
  std::atomic<f32_t> fps{0.f};
  std::atomic<f32_t> slowest_ms{0.f};  // the longest single frame in that second
};
extern draw_rate_t draw_rate;

struct render_thread_t;
#include "ecps_macros.h"

extern std::atomic<render_thread_t*> render_thread_ptr;

render_thread_t* get_render_thread();


extern std::atomic<bool> render_thread_ready;

// the encoder and decoder threads are detached and use the render thread's state and the globals.
// They must be gone before either is destroyed, so everything that ends the program stops them first
extern std::atomic<bool> app_stopping;
extern std::atomic<int> worker_threads_active;

struct worker_thread_scope_t {
  ~worker_thread_scope_t() {
    worker_threads_active.fetch_sub(1);
  }
};

// lets go of the screen capture, the next read opens it again (on Wayland: asks which screen or window).
// A fan older than close_screen() still builds, it just keeps the old capture until restart. The generic lambda
// makes the requires check dependent, so a missing member is not an error
std::vector<std::string> clipboard_file_paths();
std::string clipboard_image_file();
// a quick "is there a picture on the clipboard" check (Windows only, elsewhere false). The picture itself is saved
// by clipboard_image_begin in the background, so the render thread never waits for it; clipboard_image_take hands
// over the ones that finished
bool clipboard_has_image();
void clipboard_image_begin();
std::vector<std::string> clipboard_image_take();




inline void close_capture(auto& encoder) {
  if constexpr (requires { encoder.close_screen(); }) {
    encoder.close_screen();
  }
}

void stop_worker_threads();

#include "backend.h"

extern fan::window::input_injector_t host_input;

struct audio_share_t {
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
};
extern audio_share_t audio_share;

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
      // Only speech teaches the level: breathing, keys and room noise above the gate taught it "a very quiet
      // speaker", the gain climbed towards its top while nobody talked, and the first words came out far too loud
      constexpr f32_t speech = 0.016f;     // about -36 dB
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
      if (rms > speech) {
        norm_level = norm_level <= 0.f ? rms : norm_level + (rms - norm_level) * (rms > norm_level ? 0.25f : 0.05f);
        f32_t wanted = std::clamp(target / norm_level, min_gain, max_gain);
        norm_gain += (wanted - norm_gain) * (wanted < norm_gain ? 0.2f : 0.03f);
      }
      f32_t gain = norm_gain;
      // and no packet comes out louder than twice the target, whatever the gain has grown to: the first word after a
      // silence can't blast before the gain has come down
      if (rms > gate) {
        gain = std::min(gain, target * 2.f / rms);
      }
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
};
extern voice_chat_t voice_chat;

// MIDI a viewer plays is synthesized here, so the host needs no MIDI device or loopback driver.
// The pump thread only runs while there is sound, a note-off that never arrives dies out by itself.
struct midi_host_t {
  ~midi_host_t();

  void play(std::uint8_t status, std::uint8_t data1, std::uint8_t data2);

  void silence();

  bool start();

  void stop();

  static constexpr std::uint32_t block_frames = 240; // 5 ms
  fan::audio::midi_synth_t synth;
  fan::audio::live_player_t player;
  std::thread pump;
  std::thread loader;
  std::atomic<bool> running = false;
  bool failed = false;
};
extern midi_host_t midi_host;

#include "remote_logs.h"
extern remote_logs_t remote_logs;

#include "updater.h"
extern updater_t updater;

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

  static void raise(std::atomic<std::uint64_t>& value, std::uint64_t to);
};
extern send_debug_t send_debug;

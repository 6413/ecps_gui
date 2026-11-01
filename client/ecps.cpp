#include <cstdint>
#include <type_traits>
#include <fan/utils/utility.h>
#include <fan/event/types.h>
#include <fan/types/dme.h>
#include <cstring>
#include <unordered_map>
#include <exception>
#include <coroutine>
#include <string>
#include <array>
#include <functional>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <future>
#include <queue>
#include <deque>
#include <memory>
#include <cmath>

#include <WITCH/WITCH.h>

#define use_input_control 0

#if use_input_control == 1
  #include <WITCH/MD/Mice.h>
  #include <WITCH/MD/Keyboard/Keyboard.h>
#endif

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

#if defined(_WIN32)
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

std::timed_mutex render_mutex;

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
std::timed_mutex task_mutex;

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

struct render_thread_t;
#define ecps_debug_prints 0

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

std::mutex render_thread_mutex;
std::condition_variable render_thread_cv;
std::atomic<bool> render_thread_ready {false};

#include "backend.h"

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

struct render_thread_t {
  render_thread_t() {
    window_icon = engine.image_load("icons/ecps_logo.png");

    request_precise_timers();
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
      ecps_gui.backend_queue([=]() -> fan::event::task_t {
        try {
          for (const auto& channel : ecps_backend.channel_info) {
            if (channel.is_viewing) {
              ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_ViewToShare_t rest;
              rest.ChannelID = channel.channel_id;
              rest.Flag = ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR;
              co_await ecps_backend.tcp_write(
                ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_ViewToShare,
                &rest,
                sizeof(rest)
              );
            }
          }
        }
        catch (...) {}
      });
    };
  }

  engine_t engine;
  #define engine OFFSETLESS(This, render_thread_t, ecps_gui)->engine
  #include "gui.h"
  ecps_gui_t ecps_gui;

  fan::graphics::image_t window_icon;

  fan::graphics::screen_encode_t screen_encoder;
  fan::graphics::screen_decode_t screen_decoder;

  std::condition_variable frame_cv;
  std::condition_variable task_cv;
  std::atomic<bool> has_task_work {false};
  std::atomic<bool> should_stop {false};

  fan::graphics::universal_image_renderer_t network_frame {{
      .position = fan::vec3(fan::vec2(0), 1),
      .size = gloco()->window.get_size() / 2,
  }};
  f32_t displayed_fps = 0.0f;

  fan::vec2ui host_mouse_pos;

  void render(auto l) {
    if (engine.process_frame([this, l] { ecps_gui.render(); l(); })) {
      std::exit(0);
    }
  }

  latest_frame_t latest_frame;

};

ecps_backend_t::ecps_backend_t() {
  __dme_get(Protocol_S2C, KeepAlive) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    co_return;
  };

  __dme_get(Protocol_S2C, KeyExchange) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::KeyExchange_t>();
    fan::network::crypto::x25519_key_t server_ephemeral, server_static;
    fan::network::crypto::session_keys_t keys;
    std::memcpy(server_ephemeral.data(), msg->ServerEphemeral, server_ephemeral.size());
    std::memcpy(server_static.data(), msg->ServerStatic, server_static.size());
    bool ok = msg.status == 0
      && fan::network::crypto::client_handshake(backend.handshake_private, backend.handshake_public, server_ephemeral, server_static, Protocol::CryptoContext, keys)
      && fan::network::crypto::confirm_equal(keys.confirm, msg->Confirm)
      && backend.verify_server_key(server_static);
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

  __dme_get(Protocol_S2C, Response_Login) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Response_Login_t>();
    backend.session_id = msg->SessionID;
    co_await backend.udp_write(0, ProtocolUDP::C2S_t::KeepAlive, {}, 0, 0);
  };

  __dme_get(Protocol_S2C, CreateChannel_OK) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::CreateChannel_OK_t>();
    auto it = backend.pending_requests.find(base.ID);
    if (it != backend.pending_requests.end()) {
      it->second.channel_id = msg->ChannelID;
      it->second.completed = true;
      if (it->second.continuation) {
        it->second.continuation.resume();
      }
    }
  };

  __dme_get(Protocol_S2C, JoinChannel_OK) = [this](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::JoinChannel_OK_t>();
    for (auto& channel : channel_info) {
      if (channel.channel_id.i == msg->ChannelID.i) {
        channel.session_id = msg->ChannelSessionID;
        break;
      }
    }
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (rt) {
      rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
    }
  };

  __dme_get(Protocol_S2C, JoinChannel_Error) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::JoinChannel_Error_t>();
  };

  __dme_get(Protocol_S2C, Channel_ScreenShare_ViewToShare) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Channel_ScreenShare_ViewToShare_t>();
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (!rt || !ecps_backend.is_channel_streaming(msg->ChannelID)) {
      co_return;
    }
    if (msg->Flag & ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR) {

      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (rt) {
        rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
      }
    }
  };

  __dme_get(Protocol_S2C, ChannelList) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::ChannelList_t>();
    uint16_t channel_count = msg->ChannelCount;
    std::vector<ecps_backend_t::channel_list_info_t> channels;
    channels.reserve(channel_count);

    for (uint16_t i = 0; i < channel_count; ++i) {
      auto channel_info = co_await backend.tcp_client.read<Protocol_S2C_t::ChannelInfo_t>();
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

  __dme_get(Protocol_S2C, ChannelSessionList) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::ChannelSessionList_t>();
    Protocol_ChannelID_t channel_id = msg->ChannelID;
    uint16_t session_count = msg->SessionCount;
    // keep the old list until the new one is fully read, the gui draws from it between reads
    std::vector<ecps_backend_t::session_info_t> sessions;
    sessions.reserve(session_count);

    for (uint16_t i = 0; i < session_count; ++i) {
      auto session_info = co_await backend.tcp_client.read<Protocol_S2C_t::SessionInfo_t>();
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
  __dme_get(Protocol_S2C, Channel_ScreenShare_View_InformationToViewSetFlag) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewSetFlag_t>();

    for (auto& channel : backend.channel_info) {
      if (channel.channel_id.i == msg->ChannelID.i) {
        channel.flag = msg->Flag;
        break;
      }
    }
    co_return;
  };

  __dme_get(Protocol_S2C, Channel_ScreenShare_View_InformationToViewMouseCoordinate) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewMouseCoordinate_t>();

    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (rt) {
      backend.update_host_mouse_coordinate(msg->ChannelID, msg->pos);
    }
    co_return;
  };

  __dme_get(Protocol_S2C, Channel_ScreenShare_Share_ApplyToHostMouseCoordinate) = [this](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseCoordinate_t>();
  #if use_input_control == 1
    bool input_control_enabled = false;
    for (const auto& channel : backend.channel_info) {
      if (channel.channel_id.i == msg->ChannelID.i) {
        input_control_enabled = (channel.flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl) != 0;
        break;
      }
    }

    if (input_control_enabled) {
      MD_Mice_Error err = MD_Mice_Coordinate_Write(&mice, msg->pos.x, msg->pos.y);
      if (err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary) {
        fan::print("mouse coordinate write failed");
      }
    }
  #endif
    co_return;
  };

  __dme_get(Protocol_S2C, Channel_ScreenShare_Share_ApplyToHostMouseMotion) = [this](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseMotion_t>();
  #if use_input_control == 1
    bool input_control_enabled = false;
    for (const auto& channel : backend.channel_info) {
      if (channel.channel_id.i == msg->ChannelID.i) {
        input_control_enabled = (channel.flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl) != 0;
        break;
      }
    }

    if (input_control_enabled) {
      MD_Mice_Error err = MD_Mice_Motion_Write(&mice, msg->Motion.x, msg->Motion.y);
      if (err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary) {
        fan::print("mouse motion write failed");
      }
    }
  #endif
    co_return;
  };

  __dme_get(Protocol_S2C, Channel_ScreenShare_Share_ApplyToHostMouseButton) = [this](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseButton_t>();
  #if use_input_control == 1
    bool input_control_enabled = false;
    for (const auto& channel : backend.channel_info) {
      if (channel.channel_id.i == msg->ChannelID.i) {
        input_control_enabled = (channel.flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl) != 0;
        break;
      }
    }

    if (input_control_enabled) {
      if (msg->pos != fan::vec2i(-1)) {
        MD_Mice_Error err = MD_Mice_Coordinate_Write(&mice, msg->pos.x, msg->pos.y);
        if (err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary) {
          fan::print("mouse coordinate write failed");
        }
      }

      MD_Mice_Error err = MD_Mice_Button_Write(&mice, msg->key, msg->state);
      if (err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary) {
        fan::print("mouse button write failed");
      }
    }
  #endif
    co_return;
  };

  __dme_get(Protocol_S2C, Channel_ScreenShare_Share_ApplyToHostKeyboard) = [this](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostKeyboard_t>();
  #if use_input_control == 1
    bool input_control_enabled = false;
    for (const auto& channel : backend.channel_info) {
      if (channel.channel_id.i == msg->ChannelID.i) {
        input_control_enabled = (channel.flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl) != 0;
        break;
      }
    }

    if (input_control_enabled) {
      MD_Keyboard_Error err = MD_Keyboard_WriteKey(&keyboard, msg->Scancode, msg->State);
      if (err == MD_Keyboard_Error_UnknownArgument) {
        fan::print("unknown keyboard scancode:", msg->Scancode);
      }
      else if (err != MD_Keyboard_Error_Success && err != MD_Keyboard_Error_Temporary) {
        fan::print("keyboard write failed");
      }
    }
  #endif
    co_return;
  };

#if use_input_control == 1
  MD_Mice_Error mice_err = MD_Mice_Open(&mice);
  if (mice_err != MD_Mice_Error_Success) {
    fan::print("Failed to initialize mouse device");
  }

  MD_Keyboard_Error keyboard_err = MD_Keyboard_open(&keyboard);
  if (keyboard_err != MD_Keyboard_Error_Success) {
    fan::print("Failed to initialize keyboard device");
  }
#endif
}

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

  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (rt) {
    rt->ecps_gui.backend_queue([]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          if (channel.is_viewing) {
            ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_ViewToShare_t rest;
            rest.ChannelID = channel.channel_id;
            rest.Flag = ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR;
            co_await ecps_backend.tcp_write(
              ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_ViewToShare,
              &rest,
              sizeof(rest)
            );
          }
        }
      }
      catch (...) {}
    });
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
      return;
    }
  }
  catch (const std::exception& e) {
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Decode exception: {}", e.what());
  #endif
  }
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

  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (!rt) {
  #if ecps_debug_prints >= 1
    fan::print_throttled("NETWORK: No render thread available");
  #endif
    this->m_stats.Frame_Drop++;
    return;
  }

  std::vector<uint8_t> frame_data;

  if (FramePacketSize == this->m_data.size()) {
    frame_data.swap(this->m_data);
    this->m_data.clear();
    this->m_data.resize(0x400400);
  #if ecps_debug_prints >= 3
    printf("NETWORK: Zero-copy swap for frame %llu\n", frame_index);
  #endif
  }
  else {
    frame_data.resize(FramePacketSize);
    std::memcpy(frame_data.data(), this->m_data.data(), FramePacketSize);
  #if ecps_debug_prints >= 3
    printf("NETWORK: Memcpy for frame %llu (%u bytes)\n", frame_index, FramePacketSize);
  #endif
  }

  if (!decode_queue.push(std::move(frame_data))) {
    this->m_stats.Frame_Drop++;
    RequestKeyframe();
  }

  frame_index++;
}

void ecps_backend_t::view_t::FixFrameOnComplete() {
  // the first packet carries the packet count; without it the frame can't be sized
  if (m_Possible == (uint16_t)-1) {
    m_stats.Frame_Drop++;
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Frame dropped - first packet missing (seq {})", m_Sequence);
  #endif
    RequestKeyframe();
    return;
  }

  UpdateMissingPackets();
  if (m_MissingPackets.empty()) {
    WriteFramePacket();
    return;
  }

  m_stats.Frame_Drop++;
#if ecps_debug_prints >= 1
  ecps_print_throttled("NETWORK: Frame dropped - missing {} of {} packets (seq {}), first missing: {}",
    m_MissingPackets.size(), m_Possible, m_Sequence, m_MissingPackets.front());
#endif
  m_Possible = (uint16_t)-1;
  RequestKeyframe();

  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (rt && m_MissingPackets.size() > m_Possible / 4) {
    rt->screen_decoder.reload_codec_cb();
  }
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

  uint64_t fps = get_target_framerate();
  if (fps >= 144) return 25000000;
  else if (fps >= 120) return 20000000;
  else if (fps >= 90) return 15000000;
  else if (fps >= 60) return 12000000;
  else return 8000000;
}

int main() {
  ecps_backend.reconnected_cb = [](ecps_backend_t::Protocol_ChannelID_t channel_id) {
    if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
      rt->ecps_gui.selected_channel_id = channel_id;
    }
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
          ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t rest;
          for (const auto& channel : ecps_backend.channel_info) {
            if (channel.is_streaming) {
              rest.ChannelID = channel.channel_id;
              rest.ChannelSessionID = channel.session_id;
              break;
            }
          }

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
    {
      std::lock_guard<std::mutex> lock(render_thread_mutex);
      render_thread_ready.store(true, std::memory_order_release);
    }
    render_thread_cv.notify_all();
    render_thread_promise.set_value();

    while (!render_thread_instance.screen_decoder.is_initialized() ||
      render_thread_instance.should_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    while (!render_thread_instance.engine.should_close() && !render_thread_instance.should_stop.load()) {
      {
        std::lock_guard<std::timed_mutex> render_lock(render_mutex);
        {
          std::lock_guard<std::mutex> lock(render_thread_instance.screen_decoder.mutex);
          for (auto& i : render_thread_instance.screen_decoder.graphics_queue) {
            i();
          }
          render_thread_instance.screen_decoder.graphics_queue.clear();
        }

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

      render_thread_instance.render([] {
        static const char* sampler_names[] = {
          "Nearest", "Linear", "Nearest Mipmap Nearest",
          "Linear Mipmap Nearest", "Nearest Mipmap Linear", "Linear Mipmap Linear"
        };

        static std::uintptr_t sampler_filters[] = {
          fan::graphics::image_filter_e::nearest,
          fan::graphics::image_filter_e::linear,
          fan::graphics::image_filter_e::nearest_mipmap_nearest,
          fan::graphics::image_filter_e::linear_mipmap_nearest,
          fan::graphics::image_filter_e::nearest_mipmap_linear,
          fan::graphics::image_filter_e::linear_mipmap_linear
        };

        static int current_sampler = fan::graphics::image_filter_e::linear;

        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) return;

        std::vector<fan::graphics::image_t> img_list;
        img_list.emplace_back(rt->network_frame.get_image());
        auto images = rt->network_frame.get_images();
        img_list.insert(img_list.end(), images.begin(), images.end());
      });

      std::this_thread::yield();
    }

    render_thread_ptr.store(nullptr, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(render_thread_mutex);
      render_thread_ready.store(false, std::memory_order_release);
    }
  });

  render_thread_future.wait();


  fan::event::thread_create([] {
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    while (!rt || !rt->screen_encoder.encoder_.is_initialized()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rt = render_thread_ptr.load(std::memory_order_acquire);
    }

    lower_thread_priority();

    uint64_t frame_id = 0;
    bool first_frame = true;
    auto last_idr_time = std::chrono::steady_clock::now();
    auto last_backlog_idr = last_idr_time;
    auto& flow = ecps_backend.share.m_NetworkFlow;

    while (!rt->should_stop.load()) {
      rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt) break;

      if (ecps_backend.is_streaming_to_any_channel()) {
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

        // waits for the next frame of the desktop, but a failed read returns at once
        if (!rt->screen_encoder.screen_read()) {
          rt->screen_encoder.sleep_thread();
          continue;
        }
        stage_times.capture.add(rt->screen_encoder.capture_time_ns);

        bool encoded;
        {
          stage_scope_t encode_scope {stage_times.encode};
          encoded = rt->screen_encoder.encode_write();
        }
        if (!encoded) {
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
            if (!is_keyframe && flow.FrameList.Usage() > dynamic_config_t::get_adaptive_queue_size() &&
              backlog_now - last_backlog_idr > std::chrono::seconds(1)) {
              rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
              last_backlog_idr = backlog_now;
            }

            if (is_keyframe) {
              // a partly sent frame is dropped here, its sequence must not be reused
              auto head = flow.FrameList.GetNodeFirst();
              if (head != flow.FrameList.dst && flow.FrameList[head].SentOffset) {
                ++ecps_backend.share.frame_index;
              }
              while (flow.FrameList.Usage() > 0) {
                flow.FrameList.unlrec(flow.FrameList.GetNodeFirst());
              }
            }

            auto f = &flow.FrameList[flow.FrameList.NewNodeLast()];
            f->vec.assign(encoded_data, encoded_data + encoded_size);
            f->SentOffset = 0;
            f->keyframe = is_keyframe;
          }
          else {
            rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
          }
        }

        rt->screen_encoder.sleep_thread();
      }
      else {
        first_frame = true;
        const int fps = rt ? rt->screen_encoder.config_.frame_rate : 30;
        const auto sleep_time = std::chrono::microseconds(1000000 / fps);
        std::this_thread::sleep_for(sleep_time);
      }
    }
  });

  fan::event::thread_create([] {
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    while (!rt || !rt->screen_decoder.is_initialized()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rt = render_thread_ptr.load(std::memory_order_acquire);
    }

    lower_thread_priority();

    std::vector<uint8_t> frame;
    while (!rt->should_stop.load()) {
      rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt) break;

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
        std::unique_lock<std::timed_mutex> task_lock(render_mutex, std::defer_lock);
        if (task_lock.try_lock_for(std::chrono::milliseconds(5)) && !rt->ecps_gui.task_queue.empty()) {
          size_t batch_size = std::min(rt->ecps_gui.task_queue.size(), MAX_BATCH_SIZE);
          local_tasks.reserve(batch_size);

          for (size_t i = 0; i < batch_size; ++i) {
            local_tasks.emplace_back(std::move(rt->ecps_gui.task_queue[i]));
          }

          rt->ecps_gui.task_queue.erase(
            rt->ecps_gui.task_queue.begin(),
            rt->ecps_gui.task_queue.begin() + batch_size
          );

          if (rt->ecps_gui.task_queue.empty()) {
            rt->has_task_work.store(false);
          }
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

  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  while (!rt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    rt = render_thread_ptr.load(std::memory_order_acquire);
  }

  rt = render_thread_ptr.load(std::memory_order_acquire);
  while (rt && rt->should_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    rt = render_thread_ptr.load(std::memory_order_acquire);
    continue;
  }

  auto network_task = fan::event::every(1, []() -> fan::event::runv_t<bool> {
    /*if (ecps_backend.cleanup_disconnected_channels()) {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (rt) {
        rt->ecps_gui.window_handler.main_tab = 0;
      }
    }*/
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

    double seconds_elapsed = static_cast<double>(DeltaTime) / 1000000000.0;
    double bits_to_add = seconds_elapsed * rt->screen_encoder.config_.bitrate * bucket_multiplier;

    auto& flow = ecps_backend.share.m_NetworkFlow;
    flow.Bucket = std::min(flow.Bucket + static_cast<int64_t>(bits_to_add), static_cast<int64_t>(flow.BucketSize));

    // several frames can go in one tick, a late tick must not lower the frame rate
    const size_t max_chunks = dynamic_config_t::get_adaptive_chunk_count();
    size_t chunks_left = max_chunks;

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
        if (flow.Bucket - static_cast<int64_t>(DataSize * 8) < min_bucket ||
          co_await ecps_backend.write_stream(sent_offset, Possible, Flag, &f->vec[sent_offset * 0x400], DataSize)) {
          blocked = true;
          break;
        }
        flow.Bucket -= static_cast<int64_t>(DataSize * 8);
      }

      f->SentOffset = sent_offset;

      if (blocked || sent_offset < Possible) {
        break;
      }
      flow.FrameList.unlrec(flnr);
      ++ecps_backend.share.frame_index;
    }

    co_return 0;
  });
  fan::event::loop();

  auto* rt_final = render_thread_ptr.load(std::memory_order_acquire);
  if (rt_final) {
    rt_final->should_stop.store(true);
    rt_final->frame_cv.notify_all();
    rt_final->task_cv.notify_all();
  }

  std::exit(0);
}
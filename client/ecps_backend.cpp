// The backend's methods that are not message handlers, the frame decode, the adaptive rates
#include "gui_std.h" // precompiled, first
#include "app_state.h"
#include "ecps_threads.h"


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
  // whatever broke the picture, the frames after it refer to what is missing: hold them back until the keyframe
  ecps_backend.view_need_keyframe.store(true, std::memory_order_relaxed);
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
  // still on the GPU (zero copy): no planes here, only its size to check
  if (frame.gpu_slot >= 0) {
    return frame.image_size.x > 0 && frame.image_size.x <= 7680 && frame.image_size.y > 0 && frame.image_size.y <= 4320;
  }
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

void decode_frame(render_thread_t* rt, std::vector<uint8_t>& frame_data) {
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

void ecps_backend_t::view_t::WriteFramePacket(const frame_assembler_t::frame_t& frame) {
  if (frame.size < 32 || frame.size > 0x500000) {
    this->m_stats.Frame_Drop++;
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Frame dropped - invalid size: {} bytes (valid range: 32 - 5MB)", frame.size);
  #endif
    return;
  }

#if ecps_debug_prints >= 2
  printf("NETWORK: Frame %u received, %zu bytes%s\n", frame.sequence, frame.size, frame.recovered ? " (resends)" : "");
#endif

  ecps_backend.view_frames_assembled.fetch_add(1, std::memory_order_relaxed);

  // a frame before this one was dropped (its resends never came): decoding this one would show blocks until intra
  // refresh swept over them. A keyframe needs nothing before it
  if (frame.after_loss && !frame.keyframe) {
    RequestKeyframe();
  }

  // after a start or a loss only a keyframe can be decoded cleanly; the frames before it are held back (the last
  // picture stays) instead of decoding into blocks. A sharer that never marks keyframes isn't waited on forever
  if (ecps_backend.view_need_keyframe.load(std::memory_order_relaxed)) {
    uint64_t now = fan::event::now();
    if (frame.keyframe) {
      ecps_backend.view_need_keyframe.store(false, std::memory_order_relaxed);
      m_KeyFrameWaitSince = 0;
    }
    else {
      if (m_KeyFrameWaitSince == 0) {
        m_KeyFrameWaitSince = now;
      }
      if (now - m_KeyFrameWaitSince < 3000000000ull) {
        ecps_backend.view_keyframe_waits.fetch_add(1, std::memory_order_relaxed);
        RequestKeyframe();
        frame_index++;
        return;
      }
      ecps_backend.view_need_keyframe.store(false, std::memory_order_relaxed);
      m_KeyFrameWaitSince = 0;
    }
  }

  if (!decode_queue.push(std::vector<uint8_t>(frame.data, frame.data + frame.size))) {
    this->m_stats.Frame_Drop++;
    ecps_backend.view_queue_full.fetch_add(1, std::memory_order_relaxed);
    RequestKeyframe();
  }

  frame_index++;
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

// The encode thread: captures, encodes and queues the shared screen
#include "gui_std.h" // precompiled, first
#include "app_state.h"
#include "ecps_threads.h"

void run_encode_thread() {
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
      // a keyframe now and then repairs what a lost frame broke for a viewer that didn't ask. With intra refresh
      // the picture is renewed all the time anyway, and an extra keyframe would only be the burst it avoids
      if (time_since_idr > std::chrono::milliseconds(5000) && !rt->screen_encoder.encoder_.uses_intra_refresh()) {
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
      #if !defined(_WIN32)
        if (rt->screen_encoder.take_capture_open_failed()) {
          rt->ecps_gui.capture_armed = false;
        }
      #endif
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
        else if (rt->screen_encoder.take_capture_open_failed()) {
          // the portal dialog was cancelled: stay cancelled instead of asking again every 100 ms
          rt->ecps_gui.capture_armed = false;
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
}

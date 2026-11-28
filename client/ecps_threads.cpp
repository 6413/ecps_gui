// The render thread, the decode thread and the two event-loop tasks main starts
#include "gui_std.h" // precompiled, first
#include "app_state.h"
#include "ecps_threads.h"

// This process's class in the GPU scheduler. The capture raises it to HIGH when it starts (fan's
// wgc::raise_gpu_priority), so a game in front can't starve the capture and the encoder. It covers every GPU job of the
// process, this window's draws too: in front, drawn at the monitor's rate with HIGH, they went ahead of every other
// program on the card and the desktop froze. So HIGH only while sharing with ECPS in the background (then the window
// draws 20-30 times a second), NORMAL otherwise. Set again every second: the capture sets HIGH whenever it restarts
static void pace_gpu_priority(bool sharing, bool background) {
#if defined(_WIN32)
  static int applied = -1;
  static auto last_set = std::chrono::steady_clock::time_point{};
  int wanted = sharing && background ? 4 /* D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH */ : 2 /* NORMAL */;
  auto now = std::chrono::steady_clock::now();
  // not sharing and already NORMAL: nothing raises it, no need to keep setting it
  if (wanted == applied && (now - last_set < std::chrono::seconds(1) || !sharing)) {
    return;
  }
  last_set = now;
  using set_class_t = LONG(APIENTRY*)(HANDLE, int);
  static auto set_class = [] {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    return gdi ? (set_class_t)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass") : nullptr;
  }();
  if (set_class && set_class(GetCurrentProcess(), wanted) == 0) {
    if (wanted != applied) {
      fan::print(std::string("GPU priority: ") + (wanted == 4 ? "high (sharing, in the background)" : "normal"));
    }
    applied = wanted;
  }
#else
  (void)sharing;
  (void)background;
#endif
}

void run_render_thread(std::promise<void>& render_thread_promise) {
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

    #if ecps_flicker_debug
      {
        // what the picture is made of changing between frames (size, pixel format, GPU or CPU frame) is a flicker
        static fan::vec2ui last_size {};
        static int last_format = -1;
        static bool last_gpu = false;
        static auto last_shown = std::chrono::steady_clock::now();
        auto shown_now = std::chrono::steady_clock::now();
        bool gpu = shown_frame.gpu_slot >= 0;
        fan::vec2ui size((uint32_t)shown_frame.image_size.x, (uint32_t)shown_frame.image_size.y);
        if (size != last_size || (int)shown_frame.pixel_format != last_format || gpu != last_gpu) {
          ecps_flick("picture now {}x{}, format {}, {} (was {}x{}, format {}, {})", size.x, size.y,
            (int)shown_frame.pixel_format, gpu ? "GPU frame" : "CPU frame", last_size.x, last_size.y, last_format,
            last_gpu ? "GPU frame" : "CPU frame");
          last_size = size;
          last_format = (int)shown_frame.pixel_format;
          last_gpu = gpu;
        }
        double gap = std::chrono::duration<double, std::milli>(shown_now - last_shown).count();
        if (gap > 150.0 && gap < 30000.0) {
          ecps_flick("{:.0f} ms between two shown frames", gap);
        }
        last_shown = shown_now;
      }
    #endif
      std::array<void*, 4> planes;
      for (size_t i = 0; i < planes.size(); ++i) {
        planes[i] = shown_frame.data[i].empty() ? nullptr : shown_frame.data[i].data();
      }
      auto uploaded = ecps_expected_sync([&] {
        stage_scope_t upload_scope {stage_times.upload};
        if (shown_frame.gpu_slot >= 0) {
          // decoded on the GPU and still there: copied inside the GPU into the picture, rows tightly packed
          if (render_thread_instance.screen_decoder.show_gpu_frame(shown_frame, render_thread_instance.network_frame)) {
            render_thread_instance.network_frame.set_tc_size(fan::vec2(1, 1));
          }
        }
        else {
          render_thread_instance.network_frame.set_tc_size(fan::vec2((f32_t)shown_frame.image_size.x / shown_frame.stride[0].x, 1));
          render_thread_instance.network_frame.reload(
            shown_frame.pixel_format,
            planes.data(),
            fan::vec2ui(shown_frame.stride[0].x, shown_frame.image_size.y)
          );
        }
      });
      if (!uploaded) {
      #if ecps_debug_prints >= 1
        ecps_print_throttled("RENDER: frame upload failed: {}", uploaded.error());
      #endif
      }
    }

    render_thread_instance.render([] {});
    // the first frame drawn: from the log's times, how long the start took until the window showed
    static bool first_drawn = false;
    if (!first_drawn) {
      first_drawn = true;
      fan::print("window shown");
    }
    notify_sounds.tick(render_thread_instance.ecps_gui.stream_settings.notify_volume);
    {
      static auto second_start = std::chrono::steady_clock::now();
      static auto last_frame = second_start;
      static int frames = 0;
      static f32_t slowest_ms = 0.f;
      auto now = std::chrono::steady_clock::now();
      f32_t frame_ms = std::chrono::duration<f32_t, std::milli>(now - last_frame).count();
      if (frame_ms > 60.f && frame_ms < 30000.f) {
        ecps_flick("window draw took {:.0f} ms (upload {:.1f} ms, decode {:.1f} ms, smoothed)", frame_ms,
          stage_times.upload.ms.load(), stage_times.decode.ms.load());
      }
      slowest_ms = std::max(slowest_ms, frame_ms);
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

    // In the background (another program in front, a game for example), the window draws only as often as the
    // stream needs, with room to spare: every decoded frame still shows, and the rest of the monitor's refresh
    // rate (165 draws a second on a 165 Hz screen) is GPU time left to the game. The Target Framerate setting caps
    // it in front too: the engine's own frame timer isn't used, this thread draws the frames
    {
      static auto last_draw = std::chrono::steady_clock::now();
      auto at_fps = [](f64_t fps) {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<f64_t>(1.0 / fps));
      };
      // the Target Framerate setting: never sooner than this
      if (int target = render_thread_instance.engine.target_fps; target > 0) {
        std::this_thread::sleep_until(last_draw + at_fps(target));
      }
      // In front it follows the settings (Target Framerate, vsync), also while sharing: the GPU priority is normal
      // then (pace_gpu_priority), so drawing at the monitor's rate takes no more than any other program's fair share
      bool background = !render_thread_instance.engine.window.is_focused();
      pace_gpu_priority(ecps_backend.is_streaming_to_any_channel(), background);
      if (background) {
        f32_t stream_fps = render_thread_instance.ecps_gui.debug_sample.decoded_fps;
        if (stream_fps > 1.f) {
          // a stream playing: drawn when its next frame arrives, so exactly as often as it has frames (a fixed 1.25x
          // timer drew 80 times a second for 60 and showed each frame up to a tick late), and at least 20 times a
          // second for the rest of the window. Never more than 240, a burst of frames is shown as they come
          std::this_thread::sleep_until(last_draw + at_fps(240.0));
          render_thread_instance.latest_frame.wait(last_draw + at_fps(20.0));
        }
        else {
          std::this_thread::sleep_until(last_draw + at_fps(30.0));
        }
      }
      last_draw = std::chrono::steady_clock::now();
    }

    std::this_thread::yield();
  }

  // the workers hold a pointer to render_thread_instance, it may only go away after they did
  stop_worker_threads();
  render_thread_ptr.store(nullptr, std::memory_order_release);
  render_thread_ready.store(false, std::memory_order_release);
}

void run_decode_thread() {
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
}

// one pass over the gui's backend_queue() work, run on the network thread: a few tasks, or a 1 ms sleep when none
static fan::event::task_t queue_pass(render_thread_t* rt) {
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
      if (auto done = co_await ecps_expected(f()); !done) {
        fan::print("error: " + done.error());
      }
    }

    if (local_tasks.empty()) {
      co_await fan::co_sleep(1);
    }
  }

// The queue's task for the whole run. It was an idle handle, run on every pass of the event loop, which kept the
// loop from ever waiting: a whole CPU core busy all the time, taken from the game being shared. Now it sleeps 1 ms
// when there is nothing to do, queued work still starts within a millisecond
fan::event::task_t idle_task() {
  while (true) {
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (rt && rt->should_stop.load()) {
      co_return;
    }
    if (!rt) {
      co_await fan::co_sleep(1);
    }
    else {
      co_await queue_pass(rt);
    }
  }
}

fan::event::runv_t<bool> network_tick() {
  // lost video packets are asked for and given up on with time, not only when the next packet comes
  ecps_backend.view.assembler.set_rtt_ms(ecps_backend.ping_ms);
  ecps_backend.view.assembler.poll(fan::event::now());
  ecps_backend.view.SyncStats();
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
          if (auto listed = co_await ecps_expected(ecps_backend.request_channel_list()); !listed) {
            co_return;
          }
          if (selected_channel_id.i != (uint16_t)-1) {
            (void)co_await ecps_expected(ecps_backend.request_channel_session_list(selected_channel_id));
          }
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
  // A keyframe goes out on credit (the bucket below zero), and every frame after it waited until the debt was paid
  // at the stream's own rate: up to a quarter second of held frames, a visible hitch after each keyframe. The link
  // is faster than the stream's average, so the debt is paid back at three times the pace
  if (flow.Bucket < 0) {
    bits_to_add *= 3.0;
  }
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
}

// Part of ecps_gui_t, included inside the struct by gui.h: the toolbar and the text over the stream, the debug tab, the host pointer.

  // The bottom toolbar of the stream view (start/stop, settings, fullscreen). It is drawn at the bottom of the
  // stream area, over the picture, and fades out unless the mouse is near the bottom or there is no picture, so
  // the stream gets the whole height. Call it inside the stream child, no window or focus tricks of its own
  // a small list of who is in the channel, over the picture. Off by default so it never covers the stream
  bool show_stream_users = false;

  void render_stream_users_overlay() {
    if (!show_stream_users) {
      return;
    }
    auto channel_id = selected_channel_id;
    if (!ecps_backend.is_channel_joined(channel_id) && !ecps_backend.channel_info.empty()) {
      channel_id = ecps_backend.channel_info.front().channel_id;
    }
    std::vector<std::string> lines;
    auto it = ecps_backend.channel_sessions.find((std::uint32_t)channel_id.i);
    if (it != ecps_backend.channel_sessions.end()) {
      for (const auto& session : it->second) {
        std::string name = session.username;
        if (session.session_id.i == ecps_backend.session_id.i) {
          name += " (you)";
        }
        if (session.is_host) {
          name += " (host)";
        }
        lines.push_back(name);
      }
    }
    lines.insert(lines.begin(), "In channel  " + std::to_string(lines.size()));
    f32_t width = 180.f;
    for (const auto& line : lines) {
      width = std::max(width, gui::calc_text_size(line).x + 24.f);
    }
    f32_t height = lines.size() * gui::get_text_line_height_with_spacing() + 12.f;
    fan::vec2 at = gui::get_window_pos() + fan::vec2(std::max(0.f, gui::get_window_size().x - width - 10.f), 10.f + stream_top_offset());
    gui::set_cursor_screen_pos(at);
    gui::push_style_color(gui::col_child_bg, fan::color(0.f, 0.f, 0.f, 0.6f));
    if (gui::begin_child("##stream_users", fan::vec2(width, std::min(height, gui::get_window_size().y - 20.f)), true)) {
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i == 0) {
          gui::text_disabled(lines[i]);
        }
        else {
          gui::text(lines[i]);
        }
      }
    }
    gui::end_child();
    gui::pop_style_color();
  }

  void render_stream_toolbar() {
    top_bar.render_stream_stats_overlay();
    render_stream_users_overlay();
    render_stream_top_overlay();
    auto& io = gui::get_io();
    fan::vec2 area_pos = gui::get_window_pos();
    fan::vec2 area_size = gui::get_window_size();
    f32_t frame_height = gui::get_frame_height();
    f32_t zone = frame_height * 3.f;

    bool mouse_near = io.MousePos.x >= area_pos.x && io.MousePos.x <= area_pos.x + area_size.x &&
      io.MousePos.y >= area_pos.y + area_size.y - zone && io.MousePos.y <= area_pos.y + area_size.y;
    auto* rt = get_render_thread();
    bool has_picture = rt && rt->network_frame.get_image() != engine.default_texture;
    auto now = std::chrono::steady_clock::now();
    // also while the side panel is open (its icons are down there, the bar stays to close it again), and while a
    // chat message waits unread, so its dot is seen
    if (mouse_near || !has_picture || stream_settings.show_in_stream_view || chat_unread()) {
      toolbar_visible_until = now + std::chrono::milliseconds(1500);
    }
    f32_t target = now < toolbar_visible_until ? 1.f : 0.f;
    toolbar_alpha += (target - toolbar_alpha) * std::min(1.f, io.DeltaTime * 14.f);
    if (target == 0.f && toolbar_alpha < 0.02f) {
      toolbar_alpha = 0.f;
      return; // not drawn, so nothing under it can be clicked by accident
    }

    gui::push_style_var(gui::style_var_alpha, std::max(toolbar_alpha, 0.02f));
    f32_t y = area_size.y - frame_height - 14.f + gui::get_scroll_y();
    // as wide as the text needs, a fixed width cut the label off
    f32_t button_width = gui::pill_button_width(stream_button_renderer_t::share_button_text(false));
    gui::set_cursor_pos(fan::vec2((area_size.x - button_width) / 2, y));
    stream_button_renderer_t::render_stream_toggle_button(selected_channel_id, fan::vec2(button_width, 0));

    gui::same_line();
    gui::set_cursor_pos_x(area_size.x - (frame_height * 3) - 22.f - 68.f);
    if (gui::button("Users##btn_users", fan::vec2(60.f, frame_height))) {
      show_stream_users = !show_stream_users;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip(show_stream_users ? "Hide the users in this channel" : "Show the users in this channel");
    }
    gui::same_line(0, 8.f);
    gui::set_cursor_pos_x(area_size.x - (frame_height * 3) - 22.f);
    render_side_chat_button("##btn_chat", frame_height);
    gui::same_line(0, 8.f);
    render_side_settings_button("##btn_settings", frame_height);

    gui::same_line(area_size.x - frame_height - 4.f);
    if (icon_button("##btn_fullscreen", icon_e::fullscreen, "Full screen", frame_height, icon_style_e::overlay)) {
      is_fullscreen_stream = true;
      fullscreen_toolbar_alpha = 1.f;
      fullscreen_toolbar_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      engine.window.set_borderless();
    }
    gui::pop_style_var();
  }

  // what the stream view says while there is no picture
  void render_stream_empty_hint(fan::vec2 area) {
    std::string title;
    std::string detail;
    bool others_sharing = false;
    for (const auto& channel : ecps_backend.channel_info) {
      for (std::uint32_t session : ecps_backend.streamers_of(channel.channel_id)) {
        others_sharing |= session != (std::uint32_t)ecps_backend.session_id.i;
      }
    }
    if (others_sharing) {
      title = "Loading the stream...";
      detail = "The first picture takes a moment";
    }
    else if (ecps_backend.is_streaming_to_any_channel()) {
      title = "You are sharing";
      detail = "Turn on Preview in the settings to see it here";
    }
    else if (!ecps_backend.channel_info.empty()) {
      title = "Nobody is sharing yet";
      detail = "Press Share to share your screen, or wait for someone else";
    }
    else {
      title = "Not in a channel";
      detail = "Pick or create one in the Channels tab";
    }
    fan::vec2 title_size = gui::calc_text_size(title);
    fan::vec2 detail_size = gui::calc_text_size(detail);
    gui::set_cursor_pos(fan::vec2((area.x - title_size.x) / 2, area.y / 2 - title_size.y));
    gui::text(title);
    gui::set_cursor_pos_x((area.x - detail_size.x) / 2);
    gui::text_disabled(detail);
  }

  // Numbers about the stream and the program. Drawn only while this tab is open, read from counters the stream
  // threads already keep, and the ones that need two samples are worked out once a second. No locks the stream uses
  struct debug_sample_t {
    // one value a second, the newest is the one before head
    struct history_t {
      void push(f32_t value) {
        values[head] = value;
        head = (head + 1) % values.size();
      }
      std::array<f32_t, 90> values{};
      std::size_t head = 0;
    };
    std::chrono::steady_clock::time_point at{};
    std::uint64_t view_packets = 0, view_bytes = 0, share_packets = 0, share_bytes = 0, decoded = 0, assembled = 0;
    f32_t view_kbps = 0, view_pps = 0, share_kbps = 0, share_pps = 0, decoded_fps = 0, assembled_fps = 0;
    std::size_t voices = 0;
    std::uint64_t share_frames = 0;
    f32_t share_fps = 0;
    history_t view_kbps_history, decoded_fps_history, share_kbps_history, share_fps_history, ping_history;
  }debug_sample;

  // The rates need two samples, so they are worked out once a second from counters the stream threads already
  // keep. Cheap enough to run every frame, the top bar and the stats overlay show them too
  void update_debug_sample() {
    auto now = std::chrono::steady_clock::now();
    f32_t seconds = std::chrono::duration<f32_t>(now - debug_sample.at).count();
    if (seconds >= 1.f) {
      std::uint64_t view_packets = ecps_backend.view_udp_packets.load();
      std::uint64_t view_bytes = ecps_backend.view_udp_bytes.load();
      std::uint64_t share_packets = ecps_backend.stream_udp_packets.load();
      std::uint64_t share_bytes = ecps_backend.stream_udp_bytes.load();
      std::uint64_t decoded = ecps_backend.view_frames_decoded.load();
      std::uint64_t assembled = ecps_backend.view_frames_assembled.load();
      std::uint64_t shared = ecps_backend.share_frames_encoded.load();
      if (debug_sample.at != std::chrono::steady_clock::time_point{}) {
        debug_sample.view_pps = (view_packets - debug_sample.view_packets) / seconds;
        debug_sample.view_kbps = (view_bytes - debug_sample.view_bytes) * 8.f / 1000.f / seconds;
        debug_sample.share_pps = (share_packets - debug_sample.share_packets) / seconds;
        debug_sample.share_kbps = (share_bytes - debug_sample.share_bytes) * 8.f / 1000.f / seconds;
        debug_sample.decoded_fps = (decoded - debug_sample.decoded) / seconds;
        debug_sample.assembled_fps = (assembled - debug_sample.assembled) / seconds;
        debug_sample.share_fps = (shared - debug_sample.share_frames) / seconds;
        debug_sample.view_kbps_history.push(debug_sample.view_kbps);
        debug_sample.decoded_fps_history.push(debug_sample.decoded_fps);
        debug_sample.share_kbps_history.push(debug_sample.share_kbps);
        debug_sample.share_fps_history.push(debug_sample.share_fps);
        debug_sample.ping_history.push(ecps_backend.ping_ms);
        perf_log_tick();
      }
      debug_sample.view_packets = view_packets;
      debug_sample.view_bytes = view_bytes;
      debug_sample.share_packets = share_packets;
      debug_sample.share_bytes = share_bytes;
      debug_sample.decoded = decoded;
      debug_sample.assembled = assembled;
      debug_sample.share_frames = shared;
      debug_sample.voices = midi_host.synth.active_voices();
      debug_sample.at = now;
    }
  }

  // ---- performance in the log -------------------------------------------------------------------------------
  // One line every 30 s while sharing or watching, with what each step takes, and a warning (at most once a minute
  // of each kind) when something holds the stream back, saying where. Runs once a second, from update_debug_sample.
  // The viewer's line is the one that tells a slow stream apart: frames that never arrive (network, server) or
  // frames that arrive but are not decoded in time (this computer)
  struct perf_log_t {
    std::chrono::steady_clock::time_point summary_at = std::chrono::steady_clock::now();
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> warned_at;
    int share_slow_seconds = 0;
    int decode_behind_seconds = 0;
    int draw_behind_seconds = 0;
    // the screen's new frames: counts read from the encoder each second, the last second's rate, and sums for the summary
    std::uint64_t seen_new_frames = 0, seen_wait_ns = 0;
    f32_t screen_fps = 0.f, screen_ms = 0.f;
    std::uint64_t sum_new_frames = 0, sum_wait_ns = 0;
    std::uint64_t frame_total = 0, frame_drop = 0, packet_total = 0, packet_drop = 0;
    std::uint64_t keyframe_waits = 0, queue_full = 0, decode_failed = 0;
    // send_debug summed (or the most) since the last summary
    std::uint64_t queued = 0, sent = 0, flushed = 0, backlog_idrs = 0, lock_misses = 0, passes = 0, chunks = 0,
      cap_stops = 0, bucket_stops = 0, send_us = 0, max_send_us = 0, max_gap_us = 0, max_queue = 0;
  } perf_log;

  // once a second: what the sending did, with a warning when frames were thrown away
  void perf_collect_send(bool sharing) {
    auto take = [](std::atomic<std::uint64_t>& value) { return value.exchange(0, std::memory_order_relaxed); };
    std::uint64_t flushed = take(send_debug.flushed), backlog_idrs = take(send_debug.backlog_idrs),
      lock_misses = take(send_debug.lock_misses), max_queue = take(send_debug.max_queue), max_gap_us = take(send_debug.max_gap_us),
      queued = take(send_debug.queued), sent = take(send_debug.sent), cap_stops = take(send_debug.cap_stops),
      bucket_stops = take(send_debug.bucket_stops);
    perf_log.queued += queued;
    perf_log.sent += sent;
    perf_log.flushed += flushed;
    perf_log.backlog_idrs += backlog_idrs;
    perf_log.lock_misses += lock_misses;
    perf_log.passes += take(send_debug.passes);
    perf_log.chunks += take(send_debug.chunks);
    perf_log.cap_stops += cap_stops;
    perf_log.bucket_stops += bucket_stops;
    perf_log.send_us += take(send_debug.send_us);
    perf_log.max_send_us = std::max(perf_log.max_send_us, take(send_debug.max_send_us));
    perf_log.max_gap_us = std::max(perf_log.max_gap_us, max_gap_us);
    perf_log.max_queue = std::max(perf_log.max_queue, max_queue);
    if (!sharing) {
      return;
    }
    if (flushed || lock_misses) {
      perf_warn("send_drop", "threw away " + std::to_string(flushed) + " waiting frames for a keyframe (" +
        std::to_string(backlog_idrs) + " asked for by a long queue, up to " + std::to_string(max_queue) + " waited), " +
        std::to_string(lock_misses) + " lost to a locked queue, in the last second: " + std::to_string(queued) + " queued, " +
        std::to_string(sent) + " sent, " + std::to_string(cap_stops) + " passes hit the packet limit, " +
        std::to_string(bucket_stops) + " the bitrate budget, longest wait between passes " + fan::number_text(max_gap_us / 1000.0, 1) + " ms");
    }
  }

  void perf_warn(const std::string& kind, const std::string& text) {
    auto now = std::chrono::steady_clock::now();
    auto it = perf_log.warned_at.find(kind);
    if (it != perf_log.warned_at.end() && now - it->second < std::chrono::seconds(60)) {
      return;
    }
    perf_log.warned_at[kind] = now;
    fan::print("perf: " + text);
  }

  void perf_log_tick() {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    bool sharing = ecps_backend.is_streaming_to_any_channel();
    bool watching = debug_sample.view_pps > 0.5f;

    // what was lost in the last second
    auto& stats = ecps_backend.view.m_stats;
    std::uint64_t packet_drops_now = stats.Packet_HeadDrop + stats.Packet_BodyDrop;
    std::uint64_t frames = stats.Frame_Total - perf_log.frame_total;
    std::uint64_t frames_lost = stats.Frame_Drop - perf_log.frame_drop;
    std::uint64_t packets = stats.Packet_Total - perf_log.packet_total;
    std::uint64_t packets_lost = packet_drops_now - perf_log.packet_drop;
    // of the lost frames, the ones this computer dropped (decode queue full, decode failed) rather than the network
    auto delta = [](std::uint64_t now_value, std::uint64_t& last) { std::uint64_t d = now_value - last; last = now_value; return d; };
    std::uint64_t keyframe_waits = delta(ecps_backend.view_keyframe_waits.load(), perf_log.keyframe_waits);
    std::uint64_t queue_full = delta(ecps_backend.view_queue_full.load(), perf_log.queue_full);
    std::uint64_t decode_failed = delta(ecps_backend.view_decode_failed.load(), perf_log.decode_failed);
    perf_log.frame_total = stats.Frame_Total;
    perf_log.frame_drop = stats.Frame_Drop;
    perf_log.packet_total = stats.Packet_Total;
    perf_log.packet_drop = packet_drops_now;
    f32_t frame_loss = frames ? frames_lost * 100.f / frames : 0.f;
    f32_t packet_loss = packets + packets_lost ? packets_lost * 100.f / (packets + packets_lost) : 0.f;

    std::string encoder_name = rt->screen_encoder.encoder_.get_codec_name();
    auto& decoder = rt->screen_decoder.decoder_;
    std::string decoder_name = (decoder.using_hardware_ ? decoder.get_hardware_type_string() : std::string("CPU")) +
      (rt->screen_decoder.zero_copy_active() ? ", zero copy" : "");
    std::size_t queued = ecps_backend.share.m_NetworkFlow.FrameList.Usage();

    perf_collect_send(sharing);

    {
      auto& encoder = rt->screen_encoder;
      std::uint64_t frames_now = encoder.new_frames, wait_now = encoder.screen_wait_ns;
      std::uint64_t got = frames_now - perf_log.seen_new_frames, waited = wait_now - perf_log.seen_wait_ns;
      perf_log.seen_new_frames = frames_now;
      perf_log.seen_wait_ns = wait_now;
      perf_log.screen_fps = (f32_t)got;
      perf_log.screen_ms = got ? waited / 1e6f / got : 0.f;
      perf_log.sum_new_frames += got;
      perf_log.sum_wait_ns += waited;
    }

    if (sharing) {
      int target = std::max(1, (int)rt->screen_encoder.config_.frame_rate);
      f32_t budget = 1000.f / target;
      f32_t capture_ms = stage_times.capture.ms.load();
      f32_t encode_ms = stage_times.encode.ms.load();
      perf_log.share_slow_seconds = debug_sample.share_fps < target * 0.8f ? perf_log.share_slow_seconds + 1 : 0;
      if (perf_log.share_slow_seconds == 5) {
        std::string why;
        if (encode_ms > budget * 0.8f) {
          why = "encoding takes " + fan::number_text(encode_ms, 1) + " ms of the " + fan::number_text(budget, 1) + " ms a frame has (" +
            encoder_name + (encoder_name.rfind("lib", 0) == 0 ? ", the CPU encoder: try Encode on GPU" : "") + ")";
        }
        else if (capture_ms > budget * 0.8f) {
          why = "capturing takes " + fan::number_text(capture_ms, 1) + " ms of the " + fan::number_text(budget, 1) + " ms a frame has";
        }
        else if (queued > 2) {
          why = std::to_string(queued) + " frames wait to be sent: the bitrate is too high for the upload";
        }
        else if (perf_log.screen_ms > budget * 0.8f) {
          why = "the screen gave " + fan::number_text(perf_log.screen_fps, 0) + " new frames, getting each took " +
            fan::number_text(perf_log.screen_ms, 1) + " ms (waiting for the screen included): the screen or the compositor gives no more";
        }
        else {
          why = "the screen gave " + fan::number_text(perf_log.screen_fps, 0) + " new frames in the last second, each in " +
            fan::number_text(perf_log.screen_ms, 1) + " ms, encoding " + fan::number_text(encode_ms, 1) +
            " ms: a still screen, or a game capped below " + std::to_string(target) + " fps";
        }
        perf_warn("share_fps", "sending " + fan::number_text(debug_sample.share_fps, 0) + " of " + std::to_string(target) + " fps: " + why);
      }
    }
    else {
      perf_log.share_slow_seconds = 0;
      perf_log.sum_new_frames = perf_log.sum_wait_ns = 0; // the preview captures too, its frames are not the share's
    }

    if (watching) {
      bool behind = debug_sample.assembled_fps > debug_sample.decoded_fps + 3.f;
      perf_log.decode_behind_seconds = behind ? perf_log.decode_behind_seconds + 1 : 0;
      if (perf_log.decode_behind_seconds == 5) {
        perf_warn("decode", "this computer decodes too slowly: " + fan::number_text(debug_sample.assembled_fps, 0) + " frames/s come in, " +
          fan::number_text(debug_sample.decoded_fps, 0) + " decoded, decode takes " + fan::number_text(stage_times.decode.ms.load(), 1) +
          " ms (" + decoder_name + (decoder.using_hardware_ ? "" : ": try Decode on GPU") + ")");
      }
      // decoded in time but drawn less often: the window, not the stream
      f32_t draw_fps = draw_rate.fps.load(std::memory_order_relaxed);
      bool draw_behind = draw_fps < debug_sample.decoded_fps * 0.8f && debug_sample.decoded_fps > 10.f;
      perf_log.draw_behind_seconds = draw_behind ? perf_log.draw_behind_seconds + 1 : 0;
      if (perf_log.draw_behind_seconds == 5) {
        perf_warn("draw", "this window draws only " + fan::number_text(draw_fps, 0) + " times a second (slowest frame " +
          fan::number_text(draw_rate.slowest_ms.load(std::memory_order_relaxed), 0) + " ms) while " +
          fan::number_text(debug_sample.decoded_fps, 0) + " frames/s are decoded: the stream shows at the window's rate. Near the monitor's refresh rate that is the most it can show");
      }
      if (frames >= 5 && frame_loss > 5.f) {
        std::uint64_t here = std::min(frames_lost, queue_full + decode_failed);
        std::string why;
        if (here * 2 >= frames_lost) {
          why = "dropped on this computer: " + std::to_string(queue_full) + " found the decode queue full, " +
            std::to_string(decode_failed) + " failed to decode";
        }
        else if (packets_lost == 0) {
          why = "no packet went missing, the frames came too late (the sharer's upload or the server is slow)";
        }
        else {
          why = "data is lost between the sharer, the server and here";
        }
        perf_warn("frame_loss", "lost " + fan::number_text(frame_loss, 0) + "% of frames in the last second (" + std::to_string(frames_lost) +
          " of " + std::to_string(frames) + "), " + fan::number_text(packet_loss, 1) + "% of packets, ping " +
          fan::number_text(ecps_backend.ping_ms, 0) + " ms: " + why);
      }
      // held back while waiting for a keyframe: normal for a moment after watching starts or a frame is lost
      if (keyframe_waits >= 30) {
        perf_warn("keyframe_wait", "waited for a keyframe for " + std::to_string(keyframe_waits) +
          " frames in the last second: the sharer is slow to send one (an older version, or its upload is full)");
      }
    }
    else {
      perf_log.decode_behind_seconds = 0;
    }

    if ((sharing || watching) && ecps_backend.ping_ms > 150.f) {
      perf_warn("ping", "ping to the server is " + fan::number_text(ecps_backend.ping_ms, 0) + " ms");
    }

    // the sums cover the time since the last summary, which is 30 s only when this ran every second: divided by
    // that time, not by 30 (a window that drew rarely, or a pause in sharing, made 60 fps read as 91)
    auto reset_sums = [&] {
      perf_log.queued = perf_log.sent = perf_log.flushed = perf_log.backlog_idrs = perf_log.lock_misses = perf_log.passes = 0;
      perf_log.chunks = perf_log.cap_stops = perf_log.bucket_stops = perf_log.send_us = 0;
      perf_log.max_send_us = perf_log.max_gap_us = perf_log.max_queue = 0;
      perf_log.sum_new_frames = perf_log.sum_wait_ns = 0;
    };
    if (!sharing && !watching) {
      // nothing to report: the next summary starts from here, not with what piled up meanwhile
      perf_log.summary_at = now;
      reset_sums();
    }
    if ((sharing || watching) && now - perf_log.summary_at >= std::chrono::seconds(30)) {
      f64_t seconds = std::max(1.0, std::chrono::duration<f64_t>(now - perf_log.summary_at).count());
      perf_log.summary_at = now;
      if (sharing) {
        fan::print("perf: sending " + fan::size_text(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) + " " +
          fan::number_text(debug_sample.share_fps, 0) + "/" + std::to_string((int)rt->screen_encoder.config_.frame_rate) + " fps " +
          fan::bitrate_text(debug_sample.share_kbps) + ", capture " + fan::number_text(stage_times.capture.ms.load(), 1) +
          " ms, encode " + fan::number_text(stage_times.encode.ms.load(), 1) + " ms (" + encoder_name + "), " +
          std::to_string(queued) + " frames queued, the screen gave " + fan::number_text(perf_log.sum_new_frames / seconds, 1) +
          " new frames/s, " + fan::number_text(perf_log.sum_new_frames ? perf_log.sum_wait_ns / 1e6 / perf_log.sum_new_frames : 0.0, 1) +
          " ms each (waiting for the screen included)");
        // where the frames went: sent whole, thrown away for a keyframe, or lost before the queue. And how the network
        // loop kept up: packets per pass, passes stopped by the packet limit or the bitrate budget, the longest stall
        auto& p = perf_log;
        fan::print("perf: send path " + fan::number_text(p.queued / seconds, 1) + " frames/s queued, " + fan::number_text(p.sent / seconds, 1) +
          " sent, " + std::to_string(p.flushed) + " thrown away (" + std::to_string(p.backlog_idrs) + " keyframes for a long queue), " +
          std::to_string(p.lock_misses) + " lost to a locked queue, most waiting " + std::to_string(p.max_queue) + ". " +
          fan::number_text(p.passes / seconds, 0) + " passes/s, " + fan::number_text(p.passes ? (f64_t)p.chunks / p.passes : 0.0, 1) +
          " packets each, " + fan::number_text(p.passes ? p.send_us / 1000.0 / p.passes : 0.0, 2) + " ms each (longest " +
          fan::number_text(p.max_send_us / 1000.0, 1) + " ms), " + std::to_string(p.cap_stops) + " stopped at the packet limit, " +
          std::to_string(p.bucket_stops) + " at the bitrate budget, longest stall " + fan::number_text(p.max_gap_us / 1000.0, 1) + " ms");
      }
      reset_sums();
      if (watching) {
        // "fps" is what arrives whole. Lower than the sharer sends: lost on the way. Decoded lower than that: this computer
        fan::print("perf: receiving " + fan::size_text(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y) + " " +
          fan::number_text(debug_sample.assembled_fps, 0) + " fps " + fan::bitrate_text(debug_sample.view_kbps) +
          ", decoded " + fan::number_text(debug_sample.decoded_fps, 0) + "/s, decode " + fan::number_text(stage_times.decode.ms.load(), 1) +
          " ms (" + decoder_name + "), upload " + fan::number_text(stage_times.upload.ms.load(), 1) + " ms, lost " +
          fan::number_text(frame_loss, 1) + "% frames " + fan::number_text(packet_loss, 1) + "% packets, ping " +
          fan::number_text(ecps_backend.ping_ms, 0) + " ms, shown " + fan::number_text(rt->displayed_fps, 0) + "/s, window draws " +
          fan::number_text(draw_rate.fps.load(std::memory_order_relaxed), 0) + "/s (slowest " +
          fan::number_text(draw_rate.slowest_ms.load(std::memory_order_relaxed), 0) + " ms)");
      }
    }
  }

  void render_debug_tab() {
    // the stream view draws the picture, any other tab has to hide it
    if (auto* rt = get_render_thread(); rt) {
      rt->network_frame.set_size(0);
    }
    engine.viewport_set(engine.orthographic_render_view.viewport, 0, engine.window.get_size());


    auto number = [](double value, int decimals) { return fan::number_text(value, decimals); };
    f32_t value_column = gui::get_cursor_pos_x() + gui::calc_text_size("Viewers may play here").x + 28.f;
    auto row = [value_column](const char* label, const std::string& value) {
      f32_t start = gui::get_cursor_pos_x();
      gui::text_disabled(label);
      gui::same_line();
      gui::set_cursor_pos_x(std::max(value_column, start + gui::calc_text_size(label).x + 16.f));
      gui::text(value);
    };
    // a size that was never set holds garbage, show that as "none yet" instead of a huge number
    auto size_text = [](auto x, auto y) {
      long long width = (long long)x;
      long long height = (long long)y;
      if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return std::string("none yet");
      }
      return std::to_string(width) + " x " + std::to_string(height);
    };

    // the last 90 seconds as a line scaled to its own highest value, nothing is drawn for a value that stayed 0
    auto graph = [&](const char* label, const debug_sample_t::history_t& history, const char* unit) {
      f32_t peak = *std::max_element(history.values.begin(), history.values.end());
      if (peak <= 0.f) {
        return;
      }
      fan::vec2 size(std::max(gui::get_content_region_avail().x, 120.f), gui::get_frame_height() * 2.2f);
      fan::vec2 min = gui::get_cursor_screen_pos();
      auto* list = gui::get_window_draw_list();
      list->AddRectFilled(min, min + size, fan::color(1, 1, 1, 0.05f).get_gui_color(), 4.f);
      std::size_t count = history.values.size();
      for (std::size_t i = 0; i < count; ++i) {
        f32_t value = history.values[(history.head + i) % count];
        list->PathLineTo(fan::vec2(min.x + size.x * i / (count - 1), min.y + size.y - 2.f - (size.y - 4.f) * value / peak));
      }
      list->PathStroke(fan::color(0.45f, 0.85f, 0.55f, 1.f).get_gui_color(), 0, 1.5f);
      std::string text = std::string(label) + "  now " + number(history.values[(history.head + count - 1) % count], 0) +
        ", peak " + number(peak, 0) + " " + unit;
      list->AddText(min + fan::vec2(6.f, 3.f), fan::color(1, 1, 1, 0.6f).get_gui_color(), text.c_str());
      gui::dummy(size);
    };

    if (!gui::begin_child("##debug_panel", fan::vec2(0, 0), true)) {
      gui::end_child();
      return;
    }
    auto* rt = get_render_thread();
    auto& io = gui::get_io();

    gui::simple_section("Program");
    row("Version", std::string(ecps_version) + (ecps_update_marker[0] ? std::string(" [") + ecps_update_marker + "]" : std::string()));
    row("Build", updater.build_short().empty() ? std::string("unknown commit") : updater.build_short());
    row("Server", ecps_backend.ip + ":" + std::to_string(ecps_backend.port));
    row("Ping to server", number(ecps_backend.ping_ms, 1) + " ms");
    row("Session id", std::to_string((long long)ecps_backend.session_id.i));
    row("Window", size_text(engine.window.get_size().x, engine.window.get_size().y));
    row("UI frame rate", number(io.Framerate, 0) + " fps");
    graph("Ping", debug_sample.ping_history, "ms");

    if (rt) {
      gui::simple_section("Capture and encoder (this computer shares)");
      auto& geometry = rt->screen_encoder.mdscr.Geometry;
      row("Screen captured at", size_text(geometry.Resolution.x, geometry.Resolution.y) +
        (geometry.Resolution.x == 0 || geometry.Resolution.y == 0 ? "" :
          geometry.Resolution.y > geometry.Resolution.x ? "  (portrait)" : "  (landscape)"));
      row("Encoded as", size_text(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) + " @ " +
        std::to_string((long long)rt->screen_encoder.config_.frame_rate) + " fps");
      row("Bitrate", fan::bitrate_text(rt->screen_encoder.config_.bitrate / 1000.0));
      row("Encoder", rt->screen_encoder.name);
      row("Capture time", number(stage_times.capture.ms.load(), 2) + " ms");
      row("Encode time", number(stage_times.encode.ms.load(), 2) + " ms");
      row("Sending", number(debug_sample.share_kbps, 0) + " kbit/s, " + number(debug_sample.share_pps, 0) + " packets/s (" +
        std::to_string((unsigned long long)debug_sample.share_packets) + " total)");
      graph("Sending", debug_sample.share_kbps_history, "kbit/s");
      graph("Frames encoded", debug_sample.share_fps_history, "fps");

      gui::simple_section("Decoder (this computer watches)");
      row("Picture", size_text(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y));
      row("Decode time", number(stage_times.decode.ms.load(), 2) + " ms");
      row("Receiving", number(debug_sample.view_kbps, 0) + " kbit/s, " + number(debug_sample.view_pps, 0) + " packets/s (" +
        std::to_string((unsigned long long)debug_sample.view_packets) + " total)");
      row("Frames put together", number(debug_sample.assembled_fps, 1) + " /s (" + std::to_string((unsigned long long)debug_sample.assembled) + " total)");
      row("Frames decoded", number(debug_sample.decoded_fps, 1) + " /s (" + std::to_string((unsigned long long)debug_sample.decoded) + " total)");
      graph("Receiving", debug_sample.view_kbps_history, "kbit/s");
      graph("Frames decoded", debug_sample.decoded_fps_history, "fps");
      row("Decode failures", std::to_string((unsigned long long)ecps_backend.view_decode_failed.load()));
      auto& stats = ecps_backend.view.m_stats;
      row("Frames dropped", std::to_string((unsigned long long)stats.Frame_Drop) + " of " + std::to_string((unsigned long long)stats.Frame_Total));
      row("Packets lost", std::to_string((unsigned long long)(stats.Packet_HeadDrop + stats.Packet_BodyDrop)) + " of " +
        std::to_string((unsigned long long)stats.Packet_Total) + ", repaired frames " + std::to_string((unsigned long long)stats.Frame_Corrected));
    }

    gui::simple_section("Channels");
    if (ecps_backend.channel_info.empty()) {
      gui::text_wrapped_disabled("Not in any channel");
    }
    for (const auto& channel : ecps_backend.channel_info) {
      std::string line = "Channel " + std::to_string((long long)channel.channel_id.i) +
        (ecps_backend.is_hosting_channel(channel.channel_id) ? "  hosting" : "  member") +
        (channel.is_streaming ? ", streaming" : "") + (channel.is_viewing ? ", watching" : "") +
        std::string(", host allows:") +
        ((channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl) ? " mouse+keyboard" : "") +
        ((channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl) ? " MIDI" : "") +
        ((channel.flag & (ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl |
          ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl)) ? "" : " nothing");
      gui::text_wrapped(line);
      auto sharing_now = ecps_backend.streamers_of(channel.channel_id);
      std::string names;
      for (std::uint32_t session : sharing_now) {
        names += (names.empty() ? "" : ", ") + session_label(channel.channel_id, session);
      }
      gui::text_wrapped(std::string("    sharing: ") + (names.empty() ? "nobody" : names));
    }
    row("Watching", watch_active ? session_label(ecps_backend_t::Protocol_ChannelID_t{ (uint16_t)watch_channel }, watch_source) : std::string("nobody"));

    gui::simple_section("MIDI");
    row("Keyboard", midi_io.is_open() ? "connected" : "not connected");
    row("Keyboards found", std::to_string((unsigned long long)midi_io.devices().size()));
    row("Playing notes here", std::to_string((unsigned long long)debug_sample.voices));
    row("Sending to host", midi_to_host.load() ? "yes" : "no");
    row("Sending to viewers", midi_to_viewers.load() ? "yes" : "no");
    row("Viewers may play here", allow_midi ? "yes" : "no");

    gui::end_child();
  }

  // The pointer of the computer that shares, drawn over the picture. The capture doesn't contain it (on Windows),
  // the host sends where it is and whether it is showing, so a pointer the host hides is hidden here too
  void draw_host_cursor() {
    auto* rt = get_render_thread();
    if (!rt || !rt->host_cursor.visible.load() || rt->network_frame.get_image() == engine.default_texture) {
      return;
    }
    // the host repeats itself every second while it streams, silence means it stopped
    std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms - rt->host_cursor.updated_ms.load() > 3000) {
      return;
    }
    fan::vec2 frame_size = rt->network_frame.get_size() * 2;
    fan::vec2 frame_min = gui::get_window_pos() + fan::vec2(rt->network_frame.get_position()) - frame_size / 2;
    if (frame_size.x <= 0 || frame_size.y <= 0) {
      return;
    }
    fan::vec2 at = frame_min + fan::vec2(rt->host_cursor.x.load() * frame_size.x, rt->host_cursor.y.load() * frame_size.y);

    // an arrow, white with a dark edge so it shows on any picture
    f32_t scale = std::max(1.f, gui::get_frame_height() / 22.f);
    auto point = [&](f32_t x, f32_t y) { return fan::vec2(at.x + x * scale, at.y + y * scale); };
    auto* list = gui::get_window_draw_list();
    list->PushClipRect(frame_min, frame_min + frame_size, true);
    std::uint32_t white = 0xffffffffu;
    std::uint32_t dark = 0xff000000u;
    // one arrow shape with one outline around it, so no edge runs across the inside
    const fan::vec2 shape[] = {
      point(0, 0), point(0, 17), point(4, 13), point(7.5f, 20.5f), point(10.5f, 19.f), point(7.f, 12.f), point(12, 12),
    };
    for (const auto& p : shape) {
      list->PathLineTo(p);
    }
    list->PathFillConcave(white);
    for (const auto& p : shape) {
      list->PathLineTo(p);
    }
    list->PathStroke(dark, 1 /* closed */, 1.5f);
    list->PopClipRect();
  }

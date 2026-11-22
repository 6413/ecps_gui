// Part of ecps_gui_t, included inside the struct by gui.h: the toolbar and the text over the stream, the debug tab, the host pointer.

  // The bottom toolbar of the stream view (start/stop, settings, fullscreen). It is drawn at the bottom of the
  // stream area, over the picture, and fades out unless the mouse is near the bottom or there is no picture, so
  // the stream gets the whole height. Call it inside the stream child, no window or focus tricks of its own
  // a small list of who is in the channel, over the picture. Off by default so it never covers the stream
  bool show_stream_users = false;

  void render_stream_users_overlay();


  void render_stream_toolbar();


  // what the stream view says while there is no picture
  void render_stream_empty_hint(fan::vec2 area);


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
  void update_debug_sample();


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
  void perf_collect_send(bool sharing);


  void perf_warn(const std::string& kind, const std::string& text);


  void perf_log_tick();


  void render_debug_tab();


  // The pointer of the computer that shares, drawn over the picture. The capture doesn't contain it (on Windows),
  // the host sends where it is and whether it is showing, so a pointer the host hides is hidden here too
  void draw_host_cursor();


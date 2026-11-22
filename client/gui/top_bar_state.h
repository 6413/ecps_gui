// State and small helpers of the top bar. The drawing itself lives in top_bar_component.h.

  // through Gui so the bar's template body does not need the complete render_thread_t while it is being defined
  auto gui_rt() { return ::get_render_thread(); }

  void save_ui_config() {
    fan::json ui = fan::json::object();
    ui["compact_bar"] = top_bar_compact;
    ui["autohide"] = ui_autohide;
    ui["preview_size"] = preview_fraction;
    ui["hdr_brightness"] = hdr_exposure;
    ui["preview_fps"] = preview_fps;
    ui["encode_on_gpu"] = encode_on_gpu;
    ui["video_codec"] = video_codec;
    ui["decode_on_gpu"] = decode_on_gpu;
    ui["stats_mode"] = top_bar_stats_mode;
    ui["auto_update"] = auto_update;
    write_to_config("ui", ui);
  }

  // on: a new version found is installed by itself and the program restarts into it, also while you share your
  // screen (the restart stops the stream)
  bool auto_update = false;
  void update_automatically() {
    if (!auto_update || !updater_t::self_updates() || updater.state.load() != updater_t::state_e::available) {
      return;
    }
    updater.start_update();
  }

  // on: the bar and the tabs leave the Stream View and show again near the top edge. Off pins them back in place
  void set_ui_autohide(bool on) {
    ui_autohide = on;
    autohide_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    if (on) {
      window_handler.main_tab = 1;
      window_handler.current_tab = 1;
    }
    save_ui_config();
  }

  // how far the overlays in the stream area move down while the bar is shown over it
  f32_t stream_top_offset() const {
    return ui_autohide ? autohide_alpha * (gui::get_frame_height() + 18.f) : 0.f;
  }

  // A channel stays selected whenever there is one to select: if the selection is gone (nothing picked yet, or the
  // channel closed), yours is taken, else the first one this client is in
  void update_selected_channel() {
    if (ecps_backend.channel_info.empty()) {
      return;
    }
    if (selected_channel_id.i != (uint16_t)-1) {
      bool joined = ecps_backend.is_channel_joined(selected_channel_id);
      bool listed = ecps_backend.is_channel_available(selected_channel_id);
      if (joined || listed || !ecps_backend.channel_list_received) {
        return;
      }
    }
    for (const auto& channel : ecps_backend.channel_info) {
      if (ecps_backend.is_hosting_channel(channel.channel_id)) {
        selected_channel_id = channel.channel_id;
        return;
      }
    }
    selected_channel_id = ecps_backend.channel_info.front().channel_id;
  }

  // When the channel ticked "Join automatically" has someone sharing, its stream is shown without a click
  void update_auto_show() {
    if (auto_join_host.empty()) {
      return;
    }
    std::uint32_t me = (std::uint32_t)ecps_backend.session_id.i;
    for (const auto& channel : ecps_backend.channel_info) {
      if (ecps_backend.is_hosting_channel(channel.channel_id) || channel_name_of(channel.channel_id) != auto_join_host) {
        continue;
      }
      std::uint32_t key = (std::uint32_t)channel.channel_id.i;
      std::vector<std::uint32_t> others;
      for (std::uint32_t session : ecps_backend.streamers_of(channel.channel_id)) {
        if (session != me) {
          others.push_back(session);
        }
      }
      if (others.empty()) {
        auto_shown.erase(key);
        continue;
      }
      if (!auto_shown.insert(key).second) {
        continue;
      }
      selected_channel_id = channel.channel_id;
      set_watch(channel.channel_id, others.front());
      if (window_handler.current_tab == 0) {
        window_handler.main_tab = 1;
      }
    }
  }
  std::unordered_set<std::uint32_t> auto_shown;

  bool autohide_bar_active = false;

  bool ui_autohide = false;
  f32_t autohide_alpha = 0.f;
  std::chrono::steady_clock::time_point autohide_visible_until;

  // one line whenever the sizes the UI is laid out with change: the window, what imgui thinks the display is,
  // its framebuffer scale and the main viewport. They have to agree, a stretched picture means one of them doesn't
  void report_display_sizes() {
    auto& io = gui::get_io();
    auto* viewport = gui::get_main_viewport();
    fan::vec2i window_size = engine.window.get_size();
    std::array<f32_t, 8> now = {
      (f32_t)window_size.x, (f32_t)window_size.y, io.DisplaySize.x, io.DisplaySize.y,
      io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y, viewport->Size.x, viewport->Size.y
    };
    if (now == last_display_sizes) {
      return;
    }
    last_display_sizes = now;
  #if ecps_debug_prints >= 1
    fan::print("display: window", window_size.x, "x", window_size.y, "imgui display", io.DisplaySize.x, "x", io.DisplaySize.y,
      "framebuffer scale", io.DisplayFramebufferScale.x, "x", io.DisplayFramebufferScale.y,
      "main viewport", viewport->Size.x, viewport->Size.y);
  #endif
  }
  std::array<f32_t, 8> last_display_sizes{};

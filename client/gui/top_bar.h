// Part of ecps_gui_t, included inside the struct by gui.h: the bar: tabs, sharing, version, name, pin, and the about popup.

  // ---- the interface that hides ------------------------------------------------------------------------------

  void save_ui_config() {
    fan::json ui = fan::json::object();
    ui["compact_bar"] = top_bar_compact;
    ui["autohide"] = ui_autohide;
    ui["preview_size"] = preview_fraction;
    ui["hdr_brightness"] = hdr_exposure;
    ui["preview_fps"] = preview_fps;
    ui["encode_on_gpu"] = encode_on_gpu;
    ui["decode_on_gpu"] = decode_on_gpu;
    write_to_config("ui", ui);
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

  // The bar and the tabs when the interface is hidden: drawn over the top of the stream, shown while the mouse is
  // near the top edge (and a moment after), with a Pin button that puts everything back
  // The bar and the tabs when the interface is hidden: a window of its own over the top of the main window, at
  // the same place as the pinned bar, shown while the mouse is near the top edge (and a moment after)
  void render_autohide_bar(fan::vec2 area_pos, fan::vec2 area_size) {
    if (!ui_autohide) {
      return;
    }
    auto& io = gui::get_io();
    f32_t frame_height = gui::get_frame_height();
    f32_t bar_height = frame_height + 18.f;

    // a thin strip brings it back, once it is there the whole bar keeps it
    f32_t reach = autohide_alpha > 0.05f ? bar_height + 16.f : frame_height * 0.9f;
    bool mouse_near = io.MousePos.x >= area_pos.x && io.MousePos.x <= area_pos.x + area_size.x &&
      io.MousePos.y >= area_pos.y && io.MousePos.y <= area_pos.y + reach;
    auto now = std::chrono::steady_clock::now();
    if (mouse_near || autohide_bar_active) {
      autohide_visible_until = now + std::chrono::milliseconds(1200);
    }
    f32_t target = now < autohide_visible_until ? 1.f : 0.f;
    autohide_alpha += (target - autohide_alpha) * std::min(1.f, io.DeltaTime * 14.f);
    if (target == 0.f && autohide_alpha < 0.02f) {
      autohide_alpha = 0.f;
      return; // no window at all, so nothing blocks the stream under it
    }

    gui::set_next_window_pos(area_pos);
    gui::set_next_window_size(fan::vec2(area_size.x, bar_height + 2.f));
    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_window_border_size, 0.f);
    gui::push_style_var(gui::style_var_alpha, std::max(autohide_alpha, 0.02f));
    if (gui::begin("##autohide_bar", nullptr,
      gui::window_flags_topmost | gui::window_flags_no_decoration | gui::window_flags_no_background | gui::window_flags_no_move |
      gui::window_flags_no_saved_settings | gui::window_flags_no_docking | gui::window_flags_no_focus_on_appearing |
      gui::window_flags_no_nav | gui::window_flags_no_scroll_with_mouse)) {
      render_nav_bar(gui::get_window_pos(), area_size.x, bar_height, 0.f, autohide_alpha, true);
      autohide_bar_active = gui::is_any_item_active() || gui::is_popup_open("##top_bar_menu");
    }
    gui::end();
    gui::pop_style_var(3);
  }
  bool autohide_bar_active = false;

  bool ui_autohide = false;
  f32_t autohide_alpha = 0.f;
  std::chrono::steady_clock::time_point autohide_visible_until;

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

  // When the channel ticked "Join automatically" has someone sharing, its stream is shown without a click: that
  // channel is selected, its sharer is watched and the Stream View opens (from the Channels tab only, another tab
  // is where you chose to be). Once per time they start sharing, so leaving the Stream View stays possible
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
        auto_shown.erase(key); // the next time they share, it shows again
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

  // grey text that breaks at the edge of its panel, a long hint must not run out of it
  static void text_wrapped_disabled(const std::string& text) {
    gui::push_text_wrap_pos(0.f);
    gui::text_disabled(text);
    gui::pop_text_wrap_pos();
  }

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
      "main viewport", viewport->Size.x, "x", viewport->Size.y);
  #endif
  }
  std::array<f32_t, 8> last_display_sizes{};

  // The bar at the top: the tabs as words, sharing and what goes out and comes in, then at the right the version,
  // your name and the pin. The same bar shows over the stream when the interface is hidden, see render_autohide_bar
  void render_top_bar() {
    report_display_sizes();
    updater.tick();
    updater.tick_restart();
    f32_t bar_height = gui::get_frame_height() + 18.f;
    render_nav_bar(gui::get_window_pos(), gui::get_window_size().x, bar_height, 0.f, 1.f, false);
    gui::set_cursor_pos(fan::vec2(gui::get_style().WindowPadding.x, bar_height + 10.f));
    render_about_popup();
    render_password_popup();
  }

  void render_nav_bar(fan::vec2 area_pos, f32_t width, f32_t bar_height, f32_t y_offset, f32_t fade, bool overlay) {
    using state_e = updater_t::state_e;
    auto* list = gui::get_window_draw_list();
    f32_t frame_height = gui::get_frame_height();
    std::uint32_t alpha = (std::uint32_t)(std::clamp(fade, 0.f, 1.f) * 255.f);
    auto faded = [alpha](std::uint32_t color) {
      return (color & 0x00ffffffu) | ((((color >> 24) * alpha) / 255u) << 24);
    };
    // a dark strip with a thin blue line under it
    fan::vec2 top = area_pos + fan::vec2(0, y_offset > 0 ? 0 : 0);
    list->AddRectFilled(top, top + fan::vec2(width, bar_height), faded(0xc80c0402u));
    list->AddLine(top + fan::vec2(0, bar_height), top + fan::vec2(width, bar_height), faded(0x70ff5919u), 1.f);

    f32_t row_y = (bar_height - frame_height) * 0.5f + y_offset;
    f32_t text_y = (bar_height - gui::get_font_size()) * 0.5f + y_offset;

    // the tabs, as words: the one you are on is white and underlined
    gui::set_cursor_pos(fan::vec2(6.f, row_y));
    auto tab = [&](const char* label, int index) {
      bool active = window_handler.current_tab == index;
      fan::vec2 text = gui::calc_text_size(label);
      fan::vec2 at = gui::get_cursor_screen_pos();
      bool clicked = gui::invisible_button(label, fan::vec2(text.x + 24.f, frame_height));
      bool hovered = gui::is_item_hovered();
      std::uint32_t color = active ? 0xffffffffu : hovered ? 0xffd0d0d0u : 0xff6e6e6eu;
      list->AddText(at + fan::vec2(12.f, (frame_height - gui::get_font_size()) * 0.5f), faded(color), label);
      if (active) {
        list->AddRectFilled(at + fan::vec2(12.f, frame_height - 3.f), at + fan::vec2(text.x + 12.f, frame_height - 1.f), faded(accent_u32), 1.f);
      }
      gui::same_line(0, 2.f);
      if (clicked && !active) {
        window_handler.main_tab = index;
      }
    };
    tab("Channels", 0);
    tab("Stream View", 1);
    tab("Debug", 2);

    // what goes out and comes in is written after the tabs, once the right side is placed and the room is known
    gui::same_line(0, 22.f);
    fan::vec2 after_tabs = gui::get_cursor_pos();

    // at the right, from the edge in: the pin, your name, the version, then notes about updates and the name
    f32_t right = width - 10.f;
    f32_t pin_size = frame_height;
    right -= pin_size;
    gui::set_cursor_pos(fan::vec2(right, row_y));
    fan::vec2 pin_at = gui::get_cursor_screen_pos();
    bool pin_clicked = gui::invisible_button("##pin_interface", fan::vec2(pin_size, pin_size));
    bool pin_hovered = gui::is_item_hovered();
    if (pin_hovered) {
      gui::set_tooltip(overlay
        ? "Pin the bar"
        : "Unpin: hide the bar in Stream View");
      list->AddCircleFilled(pin_at + fan::vec2(pin_size, pin_size) * 0.5f, pin_size * 0.5f, faded(0xff1a1310u));
    }
    {
      // a push pin: round head, short body, needle. Blue while pinned
      fan::vec2 c = pin_at + fan::vec2(pin_size, pin_size) * 0.5f;
      f32_t u = pin_size * 0.11f;
      std::uint32_t color = faded(!overlay ? (pin_hovered ? accent_hover_u32 : accent_u32) : (pin_hovered ? 0xffffffffu : 0xff9a9a9au));
      list->AddCircleFilled(c + fan::vec2(u * 1.4f, -u * 1.4f), u * 1.5f, color);
      list->AddLine(c + fan::vec2(u * 1.4f, -u * 1.4f), c + fan::vec2(-u * 0.6f, u * 0.6f), color, u * 1.3f);
      list->AddLine(c + fan::vec2(-u * 1.6f, -u * 0.2f), c + fan::vec2(u * 0.2f, u * 1.6f), color, u * 0.9f);
      list->AddLine(c + fan::vec2(-u * 0.6f, u * 0.6f), c + fan::vec2(-u * 3.f, u * 3.f), color, u * 0.5f);
    }
    if (pin_clicked) {
      set_ui_autohide(!overlay);
    }

    std::string name = ecps_backend.get_current_username();
    right -= 14.f + gui::calc_text_size(name).x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.35f, 0.5f, 1.f, 1.f), name);
    if (gui::is_item_clicked()) {
      username_popup_requested = true;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Click to change your name");
    }

    std::string version = std::string("v") + ecps_version;
    right -= 16.f + gui::calc_text_size(version).x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.45f, 0.45f, 0.5f, 1.f), version);
    if (gui::is_item_clicked()) {
      about_popup_requested = true;
    }
    if (gui::is_item_clicked(1)) {
      gui::open_popup("##top_bar_menu");
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("About and updates");
    }

    right -= 16.f + gui::calc_text_size("Audio").x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.7f, 0.7f, 0.75f, 1.f), "Audio");
    if (gui::is_item_clicked()) {
      window_handler.current_tab = 0;
      voice_settings_open = true;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Voice chat microphone, speakers and filters");
    }

    // something about updates that needs a look, or just happened
    const char* hint = nullptr;
    fan::color hint_color(1.f, 1.f, 1.f, 1.f);
    switch (updater.state.load()) {
      case state_e::available: hint = "update available"; hint_color = fan::color(1.f, 0.7f, 0.15f, 1.f); break;
      case state_e::building: hint = "updating..."; hint_color = fan::color(0.3f, 0.6f, 1.f, 1.f); break;
      case state_e::ready: hint = "restarting..."; hint_color = fan::color(0.15f, 1.f, 0.3f, 1.f); break;
      case state_e::failed:
      case state_e::build_failed: hint = "update failed"; hint_color = fan::color(1.f, 0.25f, 0.2f, 1.f); break;
      default: break;
    }
    std::string update_notice = updater.update_notice();
    std::string notice = username_notice_text();
    for (const auto& [text, color, opens_about] : {
      std::tuple<std::string, fan::color, bool>{hint ? std::string(hint) : std::string(), hint_color, true},
      std::tuple<std::string, fan::color, bool>{update_notice, fan::color(0.15f, 1.f, 0.3f, 1.f), false},
      std::tuple<std::string, fan::color, bool>{notice, fan::color(1.f, 0.3f, 0.2f, 1.f), false},
    }) {
      if (text.empty()) {
        continue;
      }
      right -= 18.f + gui::calc_text_size(text).x;
      gui::set_cursor_pos(fan::vec2(right, text_y));
      gui::text(color, text);
      if (opens_about && gui::is_item_clicked()) {
        about_popup_requested = true;
      }
    }

    // the share button, left of all that
    f32_t pill_height = std::round(frame_height * 0.82f);
    f32_t pill_width = pill_button_width(stream_button_renderer_t::share_button_text(true));
    right -= 26.f + pill_width;
    gui::set_cursor_pos(fan::vec2(right, (bar_height - pill_height) * 0.5f + y_offset));
    stream_button_renderer_t::render_stream_toggle_button(selected_channel_id, fan::vec2(pill_width, pill_height), true);

    if (!top_bar_compact) {
      gui::set_cursor_pos(fan::vec2(after_tabs.x, text_y));
      render_share_chips(right - 20.f - after_tabs.x);
    }

    if (gui::begin_popup("##top_bar_menu")) {
      if (gui::selectable(top_bar_compact ? "Show what goes out and comes in" : "Leave out what goes out and comes in")) {
        top_bar_compact = !top_bar_compact;
        save_ui_config();
      }
      gui::end_popup();
    }
  }

  // version, which build it is, and the update controls
  void render_about_popup() {
    if (about_popup_requested) {
      about_popup_requested = false;
      gui::set_next_window_pos(fan::vec2(24.f, 70.f));
      gui::open_popup("About ECPS");
    }
    if (!gui::begin_popup("About ECPS")) {
      return;
    }
    using state_e = updater_t::state_e;
    gui::text(std::string("ECPS v") + ecps_version + (ecps_update_marker[0] ? std::string(" [") + ecps_update_marker + "]" : std::string()));
    std::string build = updater.build_short();
    text_wrapped_disabled(build.empty() ? updater.build_label() : build);
    gui::separator();

    switch (updater.state.load()) {
      case state_e::idle:
        if (gui::button("Check for updates")) {
          updater.check();
        }
        break;
      case state_e::checking:
        gui::text("Checking for updates...");
        break;
      case state_e::up_to_date:
        gui::text("Up to date");
        text_wrapped_disabled(updater.build_label());
        if (gui::button("Check again")) {
          updater.check();
        }
        break;
      case state_e::failed:
        gui::text_wrapped(updater.error());
        if (gui::button("Retry")) {
          updater.check();
        }
        break;
      case state_e::available:
        gui::text_wrapped("Update available: " + updater.latest());
        if (updater_t::self_updates()) {
          if (gui::button("Update now")) {
            updater.start_update();
          }
          if (updater_t::windows) {
            text_wrapped_disabled("Downloads it from GitHub, replaces the files next to this exe and restarts. Your settings are kept");
          }
        }
        else if (updater_t::installer() == "pacman") {
          text_wrapped_disabled("Update it the way it was installed: in the ecps_gui/arch folder run makepkg -si");
        }
        else {
          text_wrapped_disabled("Download it from github.com/6413/ecps_gui");
        }
        break;
      case state_e::building:
        if (updater_t::windows) {
          gui::text(updater.progress() + "...");
        }
        else {
          gui::text("Building the update...");
          text_wrapped_disabled("This can take a while. Progress is in ecps_update.log");
        }
        break;
      case state_e::ready:
        gui::text("Update " + updater.latest() + " is ready, restarting...");
        if (gui::button("Restart now")) {
          updater.restart();
        }
        break;
      case state_e::build_failed:
        gui::text_wrapped(updater.error());
        if (gui::button("Check again")) {
          updater.check();
        }
        break;
    }
    gui::end_popup();
  }

  static std::string number_text(double value, int decimals) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return std::string(text);
  }

  // a size that was never set holds garbage
  static std::string size_label(long long width, long long height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
      return "";
    }
    return std::to_string(width) + "x" + std::to_string(height);
  }

  // Top bar: share your screen (always into your own channel) and, while it happens, what goes out and what
  // comes in, both at once. They are separate things: you can send and watch in the same moment
  // what goes out and comes in, as long as it fits in room
  void render_share_chips(f32_t room) {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    bool first = true;

    // short: an arrow, the frame rate and the bitrate. The picture size and the rest are in the tooltip
    auto chip = [&](bool up, fan::color color, f32_t fps, f32_t kbps, const std::string& size) {
      std::string text = number_text(fps, 0) + " fps  " + number_text(kbps / 1000.0, 1) + " Mb/s";
      f32_t arrow = gui::get_font_size() * 0.55f;
      f32_t width = arrow + 6.f + gui::calc_text_size(text).x;
      if (width + (first ? 0.f : 14.f) > room) {
        return;
      }
      if (!first) {
        gui::same_line(0, 14.f);
      }
      fan::vec2 at = gui::get_cursor_screen_pos();
      gui::dummy(fan::vec2(arrow + 6.f, gui::get_font_size()));
      bool hovered = gui::is_item_hovered();
      fan::vec2 c = at + fan::vec2(arrow * 0.5f, gui::get_font_size() * 0.5f);
      f32_t h = arrow * 0.5f;
      auto col = color.get_gui_color();
      if (up) {
        gui::get_window_draw_list()->AddTriangleFilled(c + fan::vec2(0, -h), c + fan::vec2(h, h * 0.7f), c + fan::vec2(-h, h * 0.7f), col);
      }
      else {
        gui::get_window_draw_list()->AddTriangleFilled(c + fan::vec2(-h, -h * 0.7f), c + fan::vec2(h, -h * 0.7f), c + fan::vec2(0, h), col);
      }
      gui::same_line(0, 0);
      gui::text(color, text);
      if (hovered || gui::is_item_hovered()) {
        gui::set_tooltip((up ? std::string("Sending ") : std::string("Receiving ")) + size + "  " + text);
      }
      room -= width + (first ? 0.f : 14.f);
      first = false;
    };
    if (ecps_backend.is_streaming_to_any_channel()) {
      chip(true, fan::color(0.3f, 0.6f, 1.f, 1.f), debug_sample.share_fps, debug_sample.share_kbps,
        size_label(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height));
    }
    if (debug_sample.view_pps > 0.5f) {
      chip(false, fan::color(0.2f, 0.85f, 0.4f, 1.f), debug_sample.decoded_fps, debug_sample.view_kbps,
        size_label(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y));
    }
  }

  // Over the picture of the stream view, switched on with the Stats tick in the toolbar: what this computer sends
  // and what it receives. Both show when both are happening
  void render_stream_stats_overlay() {
    if (!show_stream_stats) {
      return;
    }
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    struct line_t {
      std::string text;
      fan::color color;
    };
    std::vector<line_t> lines;
    fan::color header(1.f, 1.f, 1.f, 1.f);
    fan::color normal(0.82f, 0.82f, 0.82f, 1.f);
    if (ecps_backend.is_streaming_to_any_channel()) {
      lines.push_back({"SENDING", fan::color(0.55f, 0.8f, 1.f, 1.f)});
      lines.push_back({"Encoded as  " + size_label(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) +
        " @ " + number_text(debug_sample.share_fps, 0) + " fps", normal});
      lines.push_back({"Bitrate  " + number_text(debug_sample.share_kbps / 1000.0, 1) + " Mbit/s", normal});
      lines.push_back({"Capture " + number_text(stage_times.capture.ms.load(), 1) + " ms, encode " +
        number_text(stage_times.encode.ms.load(), 1) + " ms", normal});
    }
    if (debug_sample.view_pps > 0.5f) {
      lines.push_back({"RECEIVING", fan::color(0.55f, 1.f, 0.7f, 1.f)});
      lines.push_back({"Picture  " + size_label(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y) +
        " @ " + number_text(debug_sample.decoded_fps, 0) + " fps", normal});
      lines.push_back({"Bitrate  " + number_text(debug_sample.view_kbps / 1000.0, 1) + " Mbit/s", normal});
      lines.push_back({"Decode " + number_text(stage_times.decode.ms.load(), 1) + " ms, lost packets " +
        std::to_string((unsigned long long)(ecps_backend.view.m_stats.Packet_HeadDrop + ecps_backend.view.m_stats.Packet_BodyDrop)), normal});
    }
    if (lines.empty()) {
      lines.push_back({"Not sending, not receiving", normal});
    }

    f32_t width = 0;
    for (const auto& line : lines) {
      width = std::max(width, gui::calc_text_size(line.text).x);
    }
    f32_t line_height = gui::get_text_line_height_with_spacing();
    // The box can be dragged anywhere inside the stream area. Until it is moved it sits at the top left
    fan::vec2 box_size(width + 16.f, lines.size() * line_height + 10.f);
    fan::vec2 area_size = gui::get_window_size();
    fan::vec2 pos = stats_moved ? stats_pos : fan::vec2(6.f, 6.f + stream_top_offset());
    pos.x = std::max(0.f, std::min(pos.x, area_size.x - box_size.x));
    pos.y = std::max(0.f, std::min(pos.y, area_size.y - box_size.y));
    gui::set_cursor_pos(pos);
    gui::invisible_button("##stats_drag", box_size);
    if (gui::is_item_active()) {
      auto delta = gui::get_io().MouseDelta;
      pos.x = std::max(0.f, std::min(pos.x + delta.x, area_size.x - box_size.x));
      pos.y = std::max(0.f, std::min(pos.y + delta.y, area_size.y - box_size.y));
      stats_moved = true;
    }
    if (stats_moved) {
      stats_pos = pos;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Drag to move");
    }
    gui::set_cursor_pos(pos);
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::get_window_draw_list()->AddRectFilled(at, at + box_size, 0xb4000000u, 6.f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      gui::set_cursor_pos(pos + fan::vec2(8.f, 6.f + i * line_height));
      gui::text(lines[i].color, lines[i].text);
    }
  }

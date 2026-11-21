// The top bar as its own component. It owns the drawing; state and helpers stay on Gui.
#pragma once

template <typename Gui>
struct top_bar_t {
  Gui& gui;
  explicit top_bar_t(Gui& owner) : gui(owner) {}
  // bottom right corner of the version text in the bar, screen pixels. Negative until the bar is drawn
  fan::vec2 about_anchor = fan::vec2(-1.f, -1.f);

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
    // Home is the stream with the channels, chat and settings panels around it
    gui::nav_tabs(list, {"Home", "Debug"}, gui.window_handler.current_tab - 1,
      frame_height, y_offset, Gui::accent_u32, faded,
      [this](int index) { gui.window_handler.main_tab = index + 1; });

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
      std::uint32_t color = faded(!overlay ? (pin_hovered ? Gui::accent_hover_u32 : Gui::accent_u32) : (pin_hovered ? 0xffffffffu : 0xff9a9a9au));
      list->AddCircleFilled(c + fan::vec2(u * 1.4f, -u * 1.4f), u * 1.5f, color);
      list->AddLine(c + fan::vec2(u * 1.4f, -u * 1.4f), c + fan::vec2(-u * 0.6f, u * 0.6f), color, u * 1.3f);
      list->AddLine(c + fan::vec2(-u * 1.6f, -u * 0.2f), c + fan::vec2(u * 0.2f, u * 1.6f), color, u * 0.9f);
      list->AddLine(c + fan::vec2(-u * 0.6f, u * 0.6f), c + fan::vec2(-u * 3.f, u * 3.f), color, u * 0.5f);
    }
    if (pin_clicked) {
      gui.set_ui_autohide(!overlay);
    }

    std::string name = ecps_backend.get_current_username();
    right -= 14.f + gui::calc_text_size(name).x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.35f, 0.5f, 1.f, 1.f), name);
    if (gui::is_item_clicked()) {
      gui.username_popup_requested = true;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Click to change your name");
    }

    std::string version = std::string("v") + ecps_version;
    right -= 16.f + gui::calc_text_size(version).x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.45f, 0.45f, 0.5f, 1.f), version);
    // the About popup drops down from the bar's bottom edge, its right side lined up with the version
    about_anchor = fan::vec2(gui::get_item_rect_max().x, top.y + bar_height);
    if (gui::is_item_clicked()) {
      gui.about_popup_requested = true;
    }
    if (gui::is_item_clicked(1)) {
      gui::open_popup("##top_bar_menu");
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("About and updates");
    }

    // an update to get: a download icon in the accent colour next to the version, it opens the same dropdown
    if (updater.state.load() == state_e::available) {
      f32_t icon_box = frame_height;
      right -= 6.f + icon_box;
      gui::set_cursor_pos(fan::vec2(right, row_y));
      fan::vec2 at = gui::get_cursor_screen_pos();
      bool clicked = gui::invisible_button("##update_available", fan::vec2(icon_box, icon_box));
      bool hovered = gui::is_item_hovered();
      // a slow bob so it gets noticed without blinking
      f64_t seconds = std::chrono::duration<f64_t>(std::chrono::steady_clock::now().time_since_epoch()).count();
      f32_t bob = hovered ? 0.f : (f32_t)std::sin(std::fmod(seconds, 1000.0) * 3.0) * icon_box * 0.04f;
      Gui::draw_icon(list, Gui::icon_e::download, at + fan::vec2(icon_box * 0.5f, icon_box * 0.5f + bob), icon_box * 0.78f,
        faded(hovered ? Gui::accent_hover_u32 : Gui::accent_u32));
      if (hovered) {
        gui::set_tooltip("Update available: " + updater.latest() + ". Click to update");
      }
      if (clicked) {
        gui.about_popup_requested = true;
      }
    }

    right -= 16.f + gui::calc_text_size("Audio").x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.7f, 0.7f, 0.75f, 1.f), "Audio");
    if (gui::is_item_clicked()) {
      gui.window_handler.main_tab = 0; // the channels panel, where the voice settings are
      gui.voice_settings_open = true;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Voice chat microphone, speakers and filters");
    }

    // something about updates that needs a look, or just happened
    const char* hint = nullptr;
    fan::color hint_color(1.f, 1.f, 1.f, 1.f);
    switch (updater.state.load()) {
      case state_e::building: hint = "updating..."; hint_color = fan::color(0.3f, 0.6f, 1.f, 1.f); break;
      case state_e::ready: hint = "restarting..."; hint_color = fan::color(0.15f, 1.f, 0.3f, 1.f); break;
      case state_e::failed:
      case state_e::build_failed: hint = "update failed"; hint_color = fan::color(1.f, 0.25f, 0.2f, 1.f); break;
      default: break;
    }
    std::string update_notice = updater.update_notice();
    std::string notice = gui.username_notice_text();
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
        gui.about_popup_requested = true;
      }
    }

    // the share button, left of all that
    f32_t pill_height = std::round(frame_height * 0.82f);
    f32_t pill_width = gui::pill_button_width(stream_button_renderer_t::share_button_text(true));
    right -= 26.f + pill_width;
    gui::set_cursor_pos(fan::vec2(right, (bar_height - pill_height) * 0.5f + y_offset));
    stream_button_renderer_t::render_stream_toggle_button(gui.selected_channel_id, fan::vec2(pill_width, pill_height), true);

    if (!gui.top_bar_compact) {
      gui::set_cursor_pos(fan::vec2(after_tabs.x, text_y));
      this->render_share_chips(right - 20.f - after_tabs.x);
    }

    if (gui::begin_popup("##top_bar_menu")) {
      if (gui::selectable(gui.top_bar_compact ? "Show what goes out and comes in" : "Leave out what goes out and comes in")) {
        gui.top_bar_compact = !gui.top_bar_compact;
        gui.save_ui_config();
      }
      if (gui::selectable(gui.top_bar_stats_mode ? "Stats: show only Mb/s" : "Stats: show picture size, fps and Mb/s")) {
        gui.top_bar_stats_mode = gui.top_bar_stats_mode ? 0 : 1;
        gui.save_ui_config();
      }
      gui::end_popup();
    }
  }

  void render_autohide_bar(fan::vec2 area_pos, fan::vec2 area_size) {
    if (!gui.ui_autohide) {
      return;
    }
    auto& io = gui::get_io();
    f32_t frame_height = gui::get_frame_height();
    f32_t bar_height = frame_height + 18.f;

    // a thin strip brings it back, once it is there the whole bar keeps it
    f32_t reach = gui.autohide_alpha > 0.05f ? bar_height + 16.f : frame_height * 0.9f;
    bool mouse_near = io.MousePos.x >= area_pos.x && io.MousePos.x <= area_pos.x + area_size.x &&
      io.MousePos.y >= area_pos.y && io.MousePos.y <= area_pos.y + reach;
    auto now = std::chrono::steady_clock::now();
    if (mouse_near || gui.autohide_bar_active) {
      gui.autohide_visible_until = now + std::chrono::milliseconds(1200);
    }
    f32_t target = now < gui.autohide_visible_until ? 1.f : 0.f;
    gui.autohide_alpha += (target - gui.autohide_alpha) * std::min(1.f, io.DeltaTime * 14.f);
    if (target == 0.f && gui.autohide_alpha < 0.02f) {
      gui.autohide_alpha = 0.f;
      return; // no window at all, so nothing blocks the stream under it
    }

    gui::set_next_window_pos(area_pos);
    gui::set_next_window_size(fan::vec2(area_size.x, bar_height + 2.f));
    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_window_border_size, 0.f);
    gui::push_style_var(gui::style_var_alpha, std::max(gui.autohide_alpha, 0.02f));
    if (gui::begin("##autohide_bar", nullptr,
      gui::window_flags_topmost | gui::window_flags_no_decoration | gui::window_flags_no_background | gui::window_flags_no_move |
      gui::window_flags_no_saved_settings | gui::window_flags_no_docking | gui::window_flags_no_focus_on_appearing |
      gui::window_flags_no_nav | gui::window_flags_no_scroll_with_mouse)) {
      this->render_nav_bar(gui::get_window_pos(), area_size.x, bar_height, 0.f, gui.autohide_alpha, true);
      gui.autohide_bar_active = gui::is_any_item_active() || gui::is_popup_open("##top_bar_menu");
    }
    gui::end();
    gui::pop_style_var(3);
  }

  void render_top_bar() {
    gui.report_display_sizes();
    updater.tick();
    updater.tick_restart();
    f32_t bar_height = gui::get_frame_height() + 18.f;
    this->render_nav_bar(gui::get_window_pos(), gui::get_window_size().x, bar_height, 0.f, 1.f, false);
    gui::set_cursor_pos(fan::vec2(gui::get_style().WindowPadding.x, bar_height + 10.f));
    this->render_about_popup();
    gui.render_password_popup();
  }

  void render_about_popup() {
    if (gui.about_popup_requested) {
      gui.about_popup_requested = false;
      gui::open_popup("About ECPS");
    }
    // a dropdown under the version, inside the app window: placed every frame and not movable, so it can't be
    // dragged out of the window into an OS window of its own that falls behind the app
    if (about_anchor.y >= 0.f) {
      gui::set_next_window_pos(about_anchor + fan::vec2(0, 4.f), gui::cond_always, fan::vec2(1.f, 0.f));
    }
    if (!gui::begin_popup("About ECPS", gui::window_flags_no_move | gui::window_flags_no_docking | gui::window_flags_no_saved_settings)) {
      return;
    }
    using state_e = updater_t::state_e;
    gui::text(std::string("ECPS v") + ecps_version + (ecps_update_marker[0] ? std::string(" [") + ecps_update_marker + "]" : std::string()));
    std::string build = updater.build_short();
    gui::text_wrapped_disabled(build.empty() ? updater.build_label() : build);
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
        gui::text_wrapped_disabled(updater.build_label());
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
            gui::text_wrapped_disabled("Downloads it from GitHub, replaces the files next to this exe and restarts. Your settings are kept");
          }
        }
        else if (updater_t::installer() == "pacman") {
          gui::text_wrapped_disabled("Update it the way it was installed: in the ecps_gui/arch folder run makepkg -si");
        }
        else {
          gui::text_wrapped_disabled("Download it from github.com/6413/ecps_gui");
        }
        break;
      case state_e::building:
        if (updater_t::windows) {
          gui::text(updater.progress() + "...");
        }
        else {
          gui::text("Building the update...");
          gui::text_wrapped_disabled("This can take a while. Progress is in ecps_update.log");
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

  void render_share_chips(f32_t room) {
    auto* rt = gui.gui_rt();
    if (!rt) {
      return;
    }
    bool first = true;

    // short: an arrow, and either the bitrate alone or the picture size, the frame rate and the bitrate. Clicking
    // switches between the two. The tooltip always has all of it
    auto chip = [&](bool up, fan::color color, f32_t fps, f32_t kbps, const std::string& size) {
      std::string bitrate = fan::bitrate_text(kbps);
      std::string details = (size.empty() ? std::string() : size + "  ") + fan::number_text(fps, 0) + " fps  " + bitrate;
      std::string text = gui.top_bar_stats_mode ? details : bitrate;
      f32_t arrow = gui::get_font_size() * 0.55f;
      f32_t width = arrow + 6.f + gui::calc_text_size(text).x;
      if (width + (first ? 0.f : 14.f) > room) {
        return;
      }
      if (!first) {
        gui::same_line(0, 14.f);
      }
      fan::vec2 at = gui::get_cursor_screen_pos();
      bool clicked = gui::invisible_button(up ? "##stats_out" : "##stats_in", fan::vec2(width, gui::get_font_size()));
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
      gui::get_window_draw_list()->AddText(at + fan::vec2(arrow + 6.f, 0.f), col, text.c_str());
      if (clicked) {
        gui.top_bar_stats_mode = gui.top_bar_stats_mode ? 0 : 1;
        gui.save_ui_config();
      }
      if (hovered) {
        gui::set_tooltip((up ? std::string("Sending ") : std::string("Receiving ")) + details +
          (gui.top_bar_stats_mode ? "\nClick to show only Mb/s" : "\nClick to show the picture size and frame rate"));
      }
      room -= width + (first ? 0.f : 14.f);
      first = false;
    };
    if (ecps_backend.is_streaming_to_any_channel()) {
      chip(true, fan::color(0.3f, 0.6f, 1.f, 1.f), gui.debug_sample.share_fps, gui.debug_sample.share_kbps,
        fan::size_text(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height));
    }
    if (gui.debug_sample.view_pps > 0.5f) {
      chip(false, fan::color(0.2f, 0.85f, 0.4f, 1.f), gui.debug_sample.decoded_fps, gui.debug_sample.view_kbps,
        fan::size_text(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y));
    }
  }

  void render_stream_stats_overlay() {
    if (!gui.show_stream_stats) {
      return;
    }
    auto* rt = gui.gui_rt();
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
      lines.push_back({"Encoded as  " + fan::size_text(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) +
        " @ " + fan::number_text(gui.debug_sample.share_fps, 0) + " fps", normal});
      lines.push_back({"Bitrate  " + fan::bitrate_text(gui.debug_sample.share_kbps), normal});
      lines.push_back({"Capture " + fan::number_text(stage_times.capture.ms.load(), 1) + " ms, encode " +
        fan::number_text(stage_times.encode.ms.load(), 1) + " ms", normal});
    }
    if (gui.debug_sample.view_pps > 0.5f) {
      lines.push_back({"RECEIVING", fan::color(0.55f, 1.f, 0.7f, 1.f)});
      lines.push_back({"Picture  " + fan::size_text(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y) +
        " @ " + fan::number_text(gui.debug_sample.decoded_fps, 0) + " fps", normal});
      lines.push_back({"Bitrate  " + fan::bitrate_text(gui.debug_sample.view_kbps), normal});
      lines.push_back({"Decode " + fan::number_text(stage_times.decode.ms.load(), 1) + " ms, lost packets " +
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
    fan::vec2 pos = gui.stats_moved ? gui.stats_pos : fan::vec2(6.f, 6.f + gui.stream_top_offset());
    pos.x = std::max(0.f, std::min(pos.x, area_size.x - box_size.x));
    pos.y = std::max(0.f, std::min(pos.y, area_size.y - box_size.y));
    gui::set_cursor_pos(pos);
    gui::invisible_button("##stats_drag", box_size);
    if (gui::is_item_active()) {
      auto delta = gui::get_io().MouseDelta;
      pos.x = std::max(0.f, std::min(pos.x + delta.x, area_size.x - box_size.x));
      pos.y = std::max(0.f, std::min(pos.y + delta.y, area_size.y - box_size.y));
      gui.stats_moved = true;
    }
    if (gui.stats_moved) {
      gui.stats_pos = pos;
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
};

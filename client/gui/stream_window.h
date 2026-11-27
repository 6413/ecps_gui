// Part of ecps_gui_t, included inside the struct by gui.h: full screen, the fps counter, the channel flags.

  void request_idr_reset() {
    backend_queue([]() -> fan::event::task_t {
      co_await ecps_backend.request_idr_reset();
      });
  }

  // Full screen: the panel at the right pushed against the edge hides, like the sides of the normal view. A tab at
  // the edge (shown with the mouse near) brings it back, clicked or dragged out
  bool fullscreen_panel_hidden = false;
  bool fullscreen_edge_was_active = false;
  bool fullscreen_grip_was_active = false;
  f32_t fullscreen_drag_width = 0.f;
  f32_t fullscreen_grip_alpha = 0.f;

  // the mini player's window has no title bar: dragged by the picture. A double click or the button goes back
  bool mini_dragging = false;
  fan::vec2 mini_grab = 0;

  void render_mini_player() {
    auto* viewport = gui::get_main_viewport();
    gui::set_next_window_pos(fan::vec2(viewport->WorkPos.x, viewport->WorkPos.y));
    gui::set_next_window_size(fan::vec2(viewport->WorkSize.x, viewport->WorkSize.y));
    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_window_border_size, 0.0f);
    gui::push_style_var(gui::style_var_window_rounding, 0.0f);
    bool shown = gui::begin("##MiniPlayer", nullptr,
      gui::window_flags_no_docking |
      gui::window_flags_no_saved_settings |
      gui::window_flags_no_move |
      gui::window_flags_no_collapse |
      gui::window_flags_no_resize |
      gui::window_flags_no_title_bar |
      gui::window_flags_no_scrollbar |
      gui::window_flags_no_scroll_with_mouse |
      gui::window_flags_no_background);
    gui::pop_style_var(3);
    if (!shown) {
      gui::end();
      return;
    }
    fan::vec2 area = gui::get_content_region_avail();
    gui::set_viewport(engine.orthographic_render_view);
    auto rt = get_render_thread();
    if (!rt) {
      gui::end();
      return;
    }
    This->network_frame_helper.setup_network_frame(area, false);
    bool has_picture = rt->network_frame.get_image() != engine.default_texture;
    if (!has_picture) {
      const char* note = "No stream";
      gui::set_cursor_pos((area - gui::calc_text_size(note)) * 0.5f);
      gui::text_disabled(note);
    }

    auto& io = gui::get_io();
    bool hovered = gui::is_window_hovered();
    f32_t button_size = 28.f;
    fan::vec2 button_at(area.x - button_size - 6.f, 6.f);
    bool over_button = false;
    if (hovered || mini_dragging) {
      gui::set_cursor_pos(button_at);
      if (icon_button("##mini_player_back", icon_e::exit_fullscreen, "Back to the full window (Esc)", button_size, icon_style_e::overlay)) {
        gui::end();
        This->set_mini_player(false);
        return;
      }
      over_button = gui::is_item_hovered();
    }
    if (hovered && !over_button && gui::is_mouse_double_clicked(0)) {
      mini_dragging = false;
      gui::end();
      This->set_mini_player(false);
      return;
    }
    // the mouse is read in the window: moving the window by the mouse's way from where it was grabbed keeps it there
    if (hovered && !over_button && io.MouseClicked[0]) {
      mini_dragging = true;
      mini_grab = fan::vec2(io.MousePos) - fan::vec2(viewport->Pos.x, viewport->Pos.y);
    }
    if (!io.MouseDown[0]) {
      mini_dragging = false;
    }
    if (mini_dragging) {
      fan::vec2 mouse = fan::vec2(io.MousePos) - fan::vec2(viewport->Pos.x, viewport->Pos.y);
      fan::vec2 moved = mouse - mini_grab;
      if (moved.x != 0.f || moved.y != 0.f) {
        engine.window.set_position(engine.window.get_position() + moved);
      }
    }
    gui::end();
  }

  void render_fullscreen_stream() {
    auto* viewport = gui::get_main_viewport();
    fan::vec2 window_pos = fan::vec2(viewport->WorkPos.x, viewport->WorkPos.y);
    fan::vec2 window_size = fan::vec2(viewport->WorkSize.x, viewport->WorkSize.y);
    gui::set_next_window_pos(window_pos);
    gui::set_next_window_size(window_size);
    gui::set_next_window_bg_alpha(1.0f);

    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_window_border_size, 0.0f);
    gui::push_style_var(gui::style_var_window_rounding, 0.0f);
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_frame_padding, fan::vec2(0, 0));

    if (gui::begin("##FullscreenStream", nullptr,
      gui::window_flags_no_docking |
      gui::window_flags_no_saved_settings |
      gui::window_flags_no_move |
      gui::window_flags_no_collapse |
      gui::window_flags_no_resize |
      gui::window_flags_no_title_bar |
      gui::window_flags_no_scrollbar |
      gui::window_flags_no_scroll_with_mouse |
      gui::window_flags_no_background)) {

      fan::vec2 window_content_size = gui::get_content_region_avail();

      gui::set_viewport(engine.orthographic_render_view);

      fan::vec2 stream_area = window_content_size;
      bool panel_open = This->stream_settings.show_in_stream_view;
      bool panel_shown = panel_open && !fullscreen_panel_hidden;
      if (panel_shown) {
        stream_area.x -= This->stream_settings.settings_panel_width;
      }

      auto rt = get_render_thread();
      if (!rt) {
        gui::pop_style_var(5);
        gui::end();
        return;
      }

      This->network_frame_helper.setup_network_frame(stream_area, true);
      This->render_fps_counter(stream_area.x);
      This->draw_host_cursor();

      gui::pop_style_var(5);

      if (panel_shown) {
        // the left edge of the panel can be dragged to make it wider or narrower, and to the screen's edge to hide it
        gui::set_cursor_pos(fan::vec2(stream_area.x - 3.f, 0));
        gui::invisible_button("##fullscreen_panel_edge", fan::vec2(6.f, window_content_size.y));
        bool edge_active = gui::is_item_active();
        if (gui::is_item_hovered() || edge_active) {
          gui::set_mouse_cursor(gui::mouse_cursor_resize_ew);
        }
        if (edge_active && !fullscreen_edge_was_active) {
          fullscreen_drag_width = This->stream_settings.settings_panel_width;
        }
        if (edge_active) {
          fullscreen_drag_width -= gui::get_io().MouseDelta.x;
          This->stream_settings.settings_panel_width = std::clamp(fullscreen_drag_width, 250.f, window_content_size.x * 0.6f);
          if (fullscreen_drag_width < side_hide_below) {
            // let go here and it hides
            fan::vec2 a = gui::get_window_pos() + fan::vec2(stream_area.x, 0.f);
            fan::vec2 b = a + fan::vec2(This->stream_settings.settings_panel_width, window_content_size.y);
            auto* front = gui::get_foreground_draw_list();
            front->AddRectFilled(a, b, 0x90000000u);
            const char* note = "Release to hide";
            fan::vec2 size = gui::calc_text_size(note);
            front->AddText((a + b) * 0.5f - size * 0.5f, 0xffffffffu, note);
          }
        }
        else if (fullscreen_edge_was_active && fullscreen_drag_width < side_hide_below) {
          fullscreen_panel_hidden = true;
        }
        fullscreen_edge_was_active = edge_active;
        gui::set_cursor_pos(fan::vec2(stream_area.x, 0));
        gui::push_style_var(gui::style_var_window_padding, fan::vec2(10.f, 10.f));
        gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, 4.f));
        gui::push_style_var(gui::style_var_frame_padding, fan::vec2(6.f, 4.f));
        gui::begin_child("##fullscreen_settings_panel",
          fan::vec2(This->stream_settings.settings_panel_width, window_content_size.y),
          true);
        This->render_side_panel_content();
        gui::end_child();
        gui::pop_style_var(3);
      }

      if (panel_open && fullscreen_panel_hidden) {
        render_fullscreen_grip(window_content_size);
      }
      if (!panel_open) {
        fullscreen_panel_hidden = false;
      }

      auto& io = gui::get_io();
      auto now = std::chrono::steady_clock::now();
      f32_t frame_height = gui::get_frame_height();
      f32_t zone = frame_height * 3.f;
      bool mouse_near = io.MousePos.x >= window_pos.x && io.MousePos.x <= window_pos.x + window_size.x &&
        io.MousePos.y >= window_pos.y + window_size.y - zone && io.MousePos.y <= window_pos.y + window_size.y;
      bool has_picture = rt->network_frame.get_image() != engine.default_texture;
      // a new unread message shows the bar for a moment, it doesn't keep it up
      bool unread = This->chat_unread();
      static bool was_unread = false;
      bool new_unread = unread && !was_unread;
      was_unread = unread;
      if (mouse_near || !has_picture || new_unread) {
        fullscreen_toolbar_visible_until = now + std::chrono::milliseconds(1500);
      }
      f32_t target = now < fullscreen_toolbar_visible_until ? 1.f : 0.f;
      fullscreen_toolbar_alpha += (target - fullscreen_toolbar_alpha) * std::min(1.f, io.DeltaTime * 14.f);
      if (target == 0.f && fullscreen_toolbar_alpha < 0.02f) {
        fullscreen_toolbar_alpha = 0.f;
      }
      else {
        gui::push_style_var(gui::style_var_alpha, std::max(fullscreen_toolbar_alpha, 0.02f));

        gui::push_style_var(gui::style_var_frame_padding, fan::vec2(12, 8));

        stream_button_renderer_t::render_centered_stream_button(This->selected_channel_id, stream_area, window_content_size);

        gui::pop_style_var(1);

        f32_t icon_size = 24.0f;
        f32_t button_spacing = 10.0f;
        f32_t margin = 20.0f;

        f32_t settings_x = stream_area.x - (icon_size + 12.f) * 3 - button_spacing * 2 - margin;
        f32_t settings_y = window_size.y - icon_size - margin;
        gui::set_cursor_pos(fan::vec2(settings_x, settings_y));

        This->render_side_chat_button("##btn_fullscreen_chat", icon_size + 12.f);
        gui::same_line(0, button_spacing);
        This->render_side_settings_button("##btn_fullscreen_settings", icon_size + 12.f);

        gui::same_line(0, button_spacing);

        if (icon_button("##btn_exit_fullscreen", icon_e::exit_fullscreen, "Leave full screen", icon_size + 12.f, icon_style_e::overlay)) {
          is_fullscreen_stream = false;
          engine.window.set_windowed();
        }

        gui::pop_style_var();
      }
    }
    else {
      gui::pop_style_var(5);
    }

    gui::end();
  }

  // the hidden panel's tab on the right edge: seen with the mouse near it, a click shows the panel, dragging it out
  // shows it as wide as it was dragged
  void render_fullscreen_grip(fan::vec2 area) {
    f32_t width = side_grip_size;
    fan::vec2 origin = gui::get_window_pos();
    gui::set_cursor_pos(fan::vec2(area.x - width, 0.f));
    bool clicked = gui::invisible_button("##fullscreen_panel_grip", fan::vec2(width, area.y));
    bool active = gui::is_item_active();
    bool hot = gui::is_item_hovered() || active;
    fan::vec2 mouse = gui::get_io().MousePos;
    f32_t dragged = origin.x + area.x - mouse.x;
    if (hot) {
      gui::set_mouse_cursor(gui::mouse_cursor_resize_ew);
      if (!active) {
        gui::set_tooltip("Show the panel (click or drag to size)");
      }
    }
    if (active && gui::is_mouse_dragging(0, 4.f)) {
      f32_t shown = std::clamp(dragged, 250.f, area.x * 0.6f);
      fan::vec2 a = origin + fan::vec2(area.x - shown, 0.f);
      fan::vec2 b = origin + area;
      auto* front = gui::get_foreground_draw_list();
      front->AddRectFilled(a, b, accent_alpha(accent_hover_u32, 0x40), 8.f);
      front->AddRect(a, b, accent_hover_u32, 8.f, 0, 2.f);
    }
    else if (fullscreen_grip_was_active && !active) {
      if (dragged > width * 2.f) {
        This->stream_settings.settings_panel_width = std::clamp(dragged, 250.f, area.x * 0.6f);
      }
      fullscreen_panel_hidden = false;
    }
    else if (clicked) {
      fullscreen_panel_hidden = false;
    }
    fullscreen_grip_was_active = active;

    // the tab: faded in with the mouse near the edge
    bool mouse_close = hot || (mouse.x > origin.x + area.x - 80.f && mouse.y >= origin.y && mouse.y <= origin.y + area.y);
    fullscreen_grip_alpha += ((mouse_close ? 1.f : 0.f) - fullscreen_grip_alpha) * std::min(1.f, gui::get_io().DeltaTime * 12.f);
    if (fullscreen_grip_alpha < 0.01f) {
      return;
    }
    auto faded = [&](std::uint32_t color) {
      return (color & 0x00ffffffu) | ((std::uint32_t)((color >> 24) * fullscreen_grip_alpha) << 24);
    };
    auto* list = gui::get_window_draw_list();
    fan::vec2 c = origin + fan::vec2(area.x - width * 0.5f, area.y * 0.5f);
    f32_t half_short = width * 0.5f - 1.f;
    list->AddRectFilled(c - fan::vec2(half_short, 36.f), c + fan::vec2(half_short, 36.f),
      faded(hot ? accent_alpha(accent_u32, 0x90) : 0x28ffffffu), half_short);
    f32_t arm = std::min(half_short * 0.75f, 4.f);
    fan::vec2 tip = c + fan::vec2(-arm * 0.5f, 0.f);
    std::uint32_t ink = faded(hot ? 0xffffffffu : 0xb0ffffffu);
    list->AddLine(tip, tip + fan::vec2(arm, -arm), ink, 1.8f);
    list->AddLine(tip, tip + fan::vec2(arm, arm), ink, 1.8f);
  }

  void render_fps_counter(f32_t stream_width, f32_t font_size = 15.f) {
    auto rt = get_render_thread();
    if (!rt || rt->network_frame.get_image() == engine.default_texture) {
      return;
    }

    f32_t decoder_fps = rt->displayed_fps;
    std::string fps_text = fan::to_string(decoder_fps, 1);

    gui::push_font(gui::get_font(font_size));

    fan::vec2 text_size = gui::calc_text_size(fps_text);
    fan::vec2 bg_padding = fan::vec2(gui::get_font_size() * 0.4f, gui::get_font_size() * 0.2f);
    fan::vec2 fps_pos = fan::vec2(stream_width - text_size.x - 20, 10);

    gui::set_cursor_pos(fps_pos);

    fan::vec2 bg_min = gui::get_cursor_screen_pos() - bg_padding;
    fan::vec2 bg_max = bg_min + text_size + bg_padding * 2;

    gui::get_window_draw_list()->AddRectFilled(
      bg_min, bg_max,
      fan::color(0, 0, 0, 0.7f).get_gui_color(),
      gui::get_font_size() * 0.2f
    );

    gui::text(fan::color(0.5f, 1, 0.5f, 1), fps_text);
    gui::pop_font();
  }

  // what the host tells the channel it allows: mouse and keyboard while streaming, MIDI whenever
  std::uint8_t wanted_channel_flags() const {
    std::uint8_t flags = 0;
    if (stream_settings.input_control != 0) {
      flags |= ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl;
    }
    if (allow_midi) {
      flags |= ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl;
    }
    return flags;
  }

  // sends the flags of every channel we host when they changed, and again every 10 s in case the server lost them
  void update_channel_flags() {
    auto now = std::chrono::steady_clock::now();
    bool refresh = now - flags_sent_at > std::chrono::seconds(10);
    std::uint8_t wanted = wanted_channel_flags();
    for (const auto& channel : ecps_backend.channel_info) {
      if (!ecps_backend.is_hosting_channel(channel.channel_id)) {
        continue;
      }
      auto sent = sent_channel_flags.find((std::uint32_t)channel.channel_id.i);
      if (!refresh && sent != sent_channel_flags.end() && sent->second == wanted) {
        continue;
      }
      sent_channel_flags[(std::uint32_t)channel.channel_id.i] = wanted;
      backend_queue([channel_id = channel.channel_id, session_id = channel.session_id, wanted]() -> fan::event::task_t {
        ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag_t rest;
        rest.ChannelID = channel_id;
        rest.ChannelSessionID = session_id;
        rest.Flag = wanted;
        (void)co_await ecps_expected(ecps_backend.tcp_write(
          ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag,
          &rest,
          sizeof(rest)
        ));
      });
    }
    if (refresh) {
      flags_sent_at = now;
    }
  }

  void send_input_control_flag() {
    if (stream_settings.input_control == 0) {
      backend_queue([]() -> fan::event::task_t {
        host_input.close();
        co_return;
      });
    }
    // forgetting what was sent makes the next frame send it
    sent_channel_flags.clear();
  }

// Part of ecps_gui_t, included inside the struct by gui.h: full screen, the fps counter, the channel flags.

  void request_idr_reset() {
    backend_queue([]() -> fan::event::task_t {
      co_await ecps_backend.request_idr_reset();
      });
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
      if (This->stream_settings.show_in_stream_view) {
        stream_area.x -= This->stream_settings.settings_panel_width;
      }

      auto rt = get_render_thread();
      if (!rt) {
        gui::pop_style_var(5);
        gui::end();
        return;
      }

      This->network_frame_helper.setup_network_frame(stream_area);
      This->render_fps_counter(stream_area.x);
      This->draw_host_cursor();

      gui::pop_style_var(5);

      if (This->stream_settings.show_in_stream_view) {
        // the left edge of the panel can be dragged to make it wider or narrower
        gui::set_cursor_pos(fan::vec2(stream_area.x - 3.f, 0));
        gui::invisible_button("##fullscreen_panel_edge", fan::vec2(6.f, window_content_size.y));
        if (gui::is_item_hovered() || gui::is_item_active()) {
          gui::set_mouse_cursor(gui::mouse_cursor_resize_ew);
        }
        if (gui::is_item_active()) {
          This->stream_settings.settings_panel_width = std::clamp(This->stream_settings.settings_panel_width - gui::get_io().MouseDelta.x,
            250.f, window_content_size.x * 0.6f);
        }
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
        try {
          ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag_t rest;
          rest.ChannelID = channel_id;
          rest.ChannelSessionID = session_id;
          rest.Flag = wanted;
          co_await ecps_backend.tcp_write(
            ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag,
            &rest,
            sizeof(rest)
          );
        }
        catch (...) {}
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

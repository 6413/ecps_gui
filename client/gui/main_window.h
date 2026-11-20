// Part of ecps_gui_t, included inside the struct by gui.h: the main window: the channels tab, the stream view and its settings panel.

  struct channel_list_window_t {
#undef This
#define This OFFSETLESS(this, ecps_gui_t, window_handler)

    void render() {
      if (!p_open) return;

      auto* viewport = gui::get_main_viewport();
      fan::vec2 window_pos = fan::vec2(viewport->WorkPos.x, viewport->WorkPos.y);
      fan::vec2 window_size = fan::vec2(viewport->WorkSize.x, viewport->WorkSize.y);
      gui::set_next_window_pos(window_pos);
      gui::set_next_window_size(window_size);
      gui::set_next_window_bg_alpha(0.99);
      This->push_theme();
      if (gui::begin("Channel Browser", &p_open,
        gui::window_flags_no_docking |
        gui::window_flags_no_saved_settings |
        gui::window_flags_no_move |
        gui::window_flags_no_collapse |
        gui::window_flags_no_background |
        gui::window_flags_no_resize |
        gui::window_flags_no_title_bar)
        ) {

        // Stream View with the interface hidden: the stream gets the whole window, the bar and the tabs come back
        // near the top edge (drawn over the stream by render_autohide_bar)
        // a tab change that was asked for but not done yet (main_tab is 0 or 2) keeps the tabs on screen until it is
        // done: the tab bar needs a frame or two to switch, and without it the Stream View would hide the tabs again
        // a tab asked for elsewhere (main_tab) takes effect here
        if (main_tab != -1) {
          current_tab = main_tab;
          main_tab = -1;
        }
        if (This->ui_autohide && current_tab == 1) {
          This->report_display_sizes();
          updater.tick();
          updater.tick_restart();
          render_stream_view_content();
          This->render_about_popup();
          This->render_password_popup();
        }
        else {
        This->render_top_bar();

        static bool first_open = true;
        if (first_open && p_open) {
          first_open = false;
          This->backend_queue([=]() -> fan::event::task_t {
            try {
              co_await ecps_backend.request_channel_list();
              if (This->selected_channel_id.i != (uint16_t)-1) {
                co_await ecps_backend.request_channel_session_list(This->selected_channel_id);
              }
            }
            catch (...) {
              fan::print("Failed to auto-refresh channel list");
            }
            });
        }

        if (!p_open) {
          first_open = true;
        }

        // the bar above holds the tabs, the one chosen fills the rest
        switch (current_tab) {
          case 0: render_channels_content(); break;
          case 1: render_stream_view_content(); break;
          case 2: This->render_debug_tab(); break;
          default: current_tab = 0; break;
        }
        }
      }
      gui::end();
      if (This->ui_autohide && current_tab == 1) {
        This->render_autohide_bar(window_pos, window_size);
      }
      This->pop_theme();
    }

    void render_channels_content() {
      if (auto rt = get_render_thread(); rt) {
        rt->network_frame.set_size(0);
      }
      engine.viewport_set(engine.orthographic_render_view.viewport, 0, engine.window.get_size());

      fan::vec2 avail_size = gui::get_content_region_avail();
      if (avail_size.x > 0 && avail_size.y > 0) {

        f32_t left_window_padding = 8.0f;
        f32_t right_window_padding = 12.0f;
        f32_t splitter_spacing = 12.0f;
        f32_t splitter_width = 2.0f;

        f32_t total_spacing = left_window_padding + splitter_spacing + splitter_width + splitter_spacing + right_window_padding;

        static f32_t channel_details_width = 0.0f;
        static f32_t unclamped_details_width = 0.0f;
        static bool initialized = false;
        if (!initialized) {
          f32_t available_content_width = avail_size.x - total_spacing;
          channel_details_width = available_content_width * 0.5f;
          unclamped_details_width = channel_details_width;
          initialized = true;
        }

        f32_t min_details_width = 300.0f;
        f32_t max_details_width = avail_size.x * 0.7f;
        channel_details_width = std::clamp(
          unclamped_details_width,
          min_details_width,
          max_details_width
        );

        f32_t channel_list_width = avail_size.x - channel_details_width - total_spacing;

        gui::dummy(fan::vec2(left_window_padding, 0));
        gui::same_line(0, 0);

        gui::begin_child("##channel_list_panel", fan::vec2(channel_list_width, avail_size.y), true);

        gui::spacing();
        gui::spacing();

        gui::spacing();
        gui::spacing();

        gui::push_item_width(150);
        gui::input_text("Search", &search_filter);
        gui::pop_item_width();

        gui::spacing();

        gui::spacing();
        gui::spacing();

        gui::table_flags_t table_flags = gui::table_flags_row_bg
          | gui::table_flags_borders_inner
          | gui::table_flags_borders_outer
          | gui::table_flags_resizable
          | gui::table_flags_sizing_fixed_same;
        if (gui::begin_child("ChannelList", fan::vec2(0, -60), true)) {
          if (gui::begin_table("ChannelTable", 4, table_flags)) {

            gui::table_setup_column("ID", gui::table_column_flags_width_fixed, 50.f, 0);
            gui::table_setup_column("Stream Name", gui::table_column_flags_width_stretch, 0.f, 0);
            gui::table_setup_column("Viewers");
            gui::table_setup_column("Live Time");

            gui::table_headers_row();

            int row_index = 0;
            for (const auto& channel : ecps_backend.available_channels) {
              gui::push_id(row_index);
              if (!search_filter.empty()) {
                std::string name_lower = channel.name;
                std::string filter_lower = search_filter;
                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
                if (name_lower.find(filter_lower) == std::string::npos) {
                  continue;
                }
              }

              bool is_selected = (This->selected_channel_id.i == channel.channel_id.i);

              bool is_host_of_channel = ecps_backend.is_current_user_host_of_channel(channel.channel_id);

              std::string display_name = channel.name;
              if (is_host_of_channel) {
                display_name = "⭐ " + channel.name;
              }
              if (channel.is_password_protected) {
                display_name += "  (password)";
              }

              bool already_joined = false;
              for (const auto& joined_channel : ecps_backend.channel_info) {
                if (joined_channel.channel_id.i == channel.channel_id.i) {
                  already_joined = true;
                  break;
                }
              }
              gui::table_next_row();
              {
                gui::table_set_column_index(0);
                gui::text(std::to_string(channel.channel_id.i));
              }

              {
                gui::table_set_column_index(1);
                gui::push_style_color(gui::col_header_active, gui::get_style().Colors[gui::col_header_hovered]);

                if (gui::selectable(std::string_view(display_name), is_selected, gui::selectable_flags_span_all_columns)) {
                  This->selected_channel_id = channel.channel_id;

                  This->backend_queue([channel_id = channel.channel_id]() -> fan::event::task_t {
                    try {
                      co_await ecps_backend.request_channel_session_list(channel_id);
                    }
                    catch (...) {
                      fan::print("Failed to request session list");
                    }
                    });
                }
                gui::pop_style_color();
              }
              {
                gui::table_set_column_index(2);
                // everyone but the host. A channel stays after its host leaves, then all in it are viewers (and it can be empty)
                bool host_here = channel.host_session_id.i != decltype(channel.host_session_id.i)(-1);
                gui::text(std::to_string(channel.user_count - (host_here && channel.user_count > 0 ? 1 : 0)));
              }
              {
                gui::table_set_column_index(3);
                uint64_t stream_time_ns = ecps_backend.get_channel_stream_time(channel.channel_id);
                double stream_time_s = stream_time_ns / 1e+9;
                uint64_t hours = stream_time_s / 3600;
                uint64_t minutes = fmod(stream_time_s, 3600.0) / 60.0;
                uint64_t seconds = fmod(stream_time_s, 60.0);
                if (!stream_time_ns) {
                  hours = minutes = seconds = 0;
                }
                gui::text(fan::format("{:02}:{:02}:{:02}", hours, minutes, seconds));
              }
              row_index++;
              gui::pop_id();
            }
            gui::end_table();
          }

          if (ecps_backend.available_channels.empty()) {
            if (ecps_backend.channel_list_received) {
              gui::text("No channels available, create a new channel.");
            }
            else {
              gui::text("Click 'Refresh Channel List' to load channels");
            }
          }
        }
        gui::end_child();

        gui::spacing();

        bool has_selection = (This->selected_channel_id.i != (uint16_t)-1);
        bool already_in_selected = false;
        bool is_host_of_selected = false;

        if (has_selection) {
          for (const auto& joined_channel : ecps_backend.channel_info) {
            if (joined_channel.channel_id.i == This->selected_channel_id.i) {
              already_in_selected = true;

              is_host_of_selected = ecps_backend.is_current_user_host_of_channel(This->selected_channel_id);
              break;
            }
          }
        }

        if (has_selection && !is_host_of_selected) {
          if (already_in_selected) {
            if (icon_button("##leave_channel", icon_e::leave, "Leave channel", gui::get_frame_height() * 1.15f, icon_style_e::danger)) {
              // a channel joined by itself would be joined again at once, leaving it stops that
              for (const auto& channel : ecps_backend.available_channels) {
                if (channel.channel_id.i == This->selected_channel_id.i && This->auto_join_host == channel.name) {
                  This->set_auto_join(std::string());
                }
              }
              This->backend_queue([channel_id = This->selected_channel_id]() -> fan::event::task_t {
                try {
                  co_await ecps_backend.channel_quit(channel_id);
                  co_await ecps_backend.request_channel_list();
                }
                catch (...) {
                  fan::print("Failed to leave channel");
                }
                });
            }
            gui::same_line();
          }
          else {
            if (icon_button("##join_channel", icon_e::join, "Join channel", gui::get_frame_height() * 1.15f, icon_style_e::accent)) {
              This->backend_queue([channel_id = This->selected_channel_id]() -> fan::event::task_t {
                try {
                  co_await ecps_backend.channel_join(channel_id);
                  co_await ecps_backend.request_channel_list();
                  co_await ecps_backend.request_channel_session_list(channel_id);
                }
                catch (...) {
                  fan::print("Failed to join channel");
                }
                });
            }
            gui::same_line();
          }
        }

        if (icon_button("##add_channel", icon_e::plus, "New channel", gui::get_frame_height() * 1.15f)) {
          This->backend_queue([this]() -> fan::event::task_t {
            try {
              auto channel_id = co_await ecps_backend.channel_create();
              co_await ecps_backend.channel_join(channel_id, true);
              co_await ecps_backend.request_channel_list();
              co_await ecps_backend.request_channel_session_list(channel_id);
              This->selected_channel_id = channel_id;
            }
            catch (...) {
              fan::print("Failed to create channel");
            }
            });
        }
        gui::same_line();
        
        if (icon_button("##connect_server", icon_e::server, "Connect to a server", gui::get_frame_height() * 1.15f)) {
          This->drop_down_server.toggle_render_server_connect = true;
        }

        gui::end_child();

        gui::same_line(0, splitter_spacing);

        static bool is_resizing_channels = false;
        splitter_renderer_t::render_vertical_splitter("##channel_splitter_hitbox", avail_size.y,
          unclamped_details_width, channel_details_width, min_details_width, max_details_width, is_resizing_channels);

        gui::same_line(0, splitter_spacing);

        gui::begin_child("##channel_details_panel", fan::vec2(channel_details_width, avail_size.y), true);

        gui::dummy(fan::vec2(12, 0));
        gui::same_line(0, 0);
        gui::begin_group();

        if (This->selected_channel_id.i != (uint16_t)-1) {
          std::string channel_name = "Channel " + std::to_string(This->selected_channel_id.i);
          for (const auto& channel : ecps_backend.available_channels) {
            if (channel.channel_id.i == This->selected_channel_id.i) {
              channel_name = channel.name;
              break;
            }
          }
          gui::text("Channel: " + channel_name + (ecps_backend.is_hosting_channel(This->selected_channel_id) ? "  (yours)" : ""));
          if (!ecps_backend.is_hosting_channel(This->selected_channel_id) && !channel_name.empty()) {
            gui::same_line(0, 24.f);
            bool auto_join_on = This->auto_join_host == channel_name;
            if (gui::checkbox("Join automatically", &auto_join_on)) {
              This->set_auto_join(auto_join_on ? channel_name : std::string());
            }
            if (gui::is_item_hovered()) {
              gui::set_tooltip("Joins this channel by itself whenever its host is online, also after a restart");
            }
          }

          gui::separator();

          gui::spacing();

          if (gui::begin_tab_bar("ChannelDetailTabs")) {
            gui::spacing();
            gui::spacing();
            if (gui::begin_tab_item("Users", nullptr, detail_tab == 0 ? gui::tab_item_flags_set_selected : 0)) {
              if (detail_tab == 0) detail_tab = -1;

              gui::spacing();
              gui::spacing();
              if (icon_button("##refresh_users", icon_e::refresh, "Refresh")) {
                This->backend_queue([channel_id = This->selected_channel_id]() -> fan::event::task_t {
                  try {
                    co_await ecps_backend.request_channel_session_list(channel_id);
                  }
                  catch (...) {
                    fan::print("Failed to refresh session list");
                  }
                  });
              }

              gui::spacing();
              gui::spacing();
              gui::spacing();

              // the host's buttons live in their own scrolling child below the list, so they can't run off the panel
              f32_t host_controls_height = is_host_of_selected ? gui::get_frame_height() * (This->stream_settings.share_audio ? 17.f : 12.f) : 0.f;
              if (already_in_selected) {
                This->render_voice_switch(This->selected_channel_id);
              }
              if (gui::begin_child("SessionList", fan::vec2(0, -host_controls_height), true)) {
                auto it = ecps_backend.channel_sessions.find(This->selected_channel_id.i);
                if (it != ecps_backend.channel_sessions.end()) {
                  auto sharing_now = ecps_backend.streamers_of(This->selected_channel_id);
                  auto in_voice = ecps_backend.voice_members_of(This->selected_channel_id);
                  for (const auto& session : it->second) {
                    bool is_sharing = std::find(sharing_now.begin(), sharing_now.end(), (std::uint32_t)session.session_id.i) != sharing_now.end();
                    bool is_me = session.session_id.i == ecps_backend.session_id.i;
                    bool has_buttons = already_in_selected && !is_me;
                    gui::push_id((int)session.session_id.i);

                    f32_t button_size = gui::get_frame_height();
                    f32_t row_height = button_size + 8.f;
                    f32_t row_width = gui::get_content_region_avail().x;
                    fan::vec2 row_at = gui::get_cursor_screen_pos();
                    if (gui::is_mouse_hovering_rect(row_at, row_at + fan::vec2(row_width, row_height))) {
                      gui::get_window_draw_list()->AddRectFilled(row_at, row_at + fan::vec2(row_width, row_height), 0xff0b0706u, 6.f);
                    }

                    gui::set_cursor_screen_pos(row_at + fan::vec2(10.f, (row_height - gui::get_font_size()) * 0.5f));
                    gui::text(fan::color(0.9f, 0.9f, 0.9f, 1.f), session.username);
                    if (is_me) {
                      gui::same_line(0, 6.f);
                      gui::text(fan::color(0.4f, 0.4f, 0.45f, 1.f), "(you)");
                    }
                    if (session.is_host) {
                      gui::same_line(0, 10.f);
                      badge("host", 0x50ff5919u, 0xffffa070u);
                    }
                    if (is_sharing) {
                      gui::same_line(0, 8.f);
                      badge("live", 0x600a0affu, 0xff6a6affu);
                    }
                    if (std::find(in_voice.begin(), in_voice.end(), (std::uint32_t)session.session_id.i) != in_voice.end()) {
                      std::uint32_t speaker = (std::uint32_t)session.session_id.i;
                      // fast up, slow down, so the meter doesn't flicker between packets
                      f32_t target = voice_chat.talk_level(speaker);
                      f32_t& shown = This->voice_meter_levels[speaker];
                      f32_t dt = std::min(gui::get_io().DeltaTime, 0.1f);
                      shown = target > shown ? shown + (target - shown) * std::min(dt * 30.f, 1.f) : std::max(target, shown - dt * 2.5f);
                      gui::same_line(0, 8.f);
                      level_badge("voice", shown, 0x40909090u, 0xa030c040u, shown > 0.01f ? 0xffb0ffb0u : 0xffb0b0b0u);
                    }
                    gui::same_line(0, 10.f);
                    gui::text(fan::color(0.3f, 0.3f, 0.34f, 1.f), "#" + std::to_string(session.session_id.i));

                    if (has_buttons) {
                      f32_t x = row_at.x + row_width - 8.f - button_size;
                      gui::set_cursor_screen_pos(fan::vec2(x, row_at.y + 4.f));
                      if (icon_button("##send_file", icon_e::send_file, "Send a file")) {
                        This->send_file_dialog(This->selected_channel_id, session.session_id);
                      }
                      if (is_sharing) {
                        gui::set_cursor_screen_pos(fan::vec2(x - button_size - 4.f, row_at.y + 4.f));
                        if (icon_button("##watch", icon_e::eye, "Watch in Stream View")) {
                          This->set_watch(This->selected_channel_id, (std::uint32_t)session.session_id.i);
                          // to the picture, watching from here would otherwise change nothing you can see
                          main_tab = 1;
                        }
                      }
                    }
                    gui::set_cursor_screen_pos(row_at + fan::vec2(0, row_height + 2.f));
                    gui::dummy(fan::vec2(0, 0));
                    gui::pop_id();
                  }
                }
                else {
                  gui::text("No user data loaded.");
                  gui::text("Select a channel to view users.");
                }
              }
              gui::end_child();

              gui::spacing();
              if (is_host_of_selected && gui::begin_child("HostControls", fan::vec2(0, 0), 0)) {
                gui::text(ecps_backend.is_streaming_to_any_channel() ? "You are sharing your screen" : "Your screen is not shared");
                text_wrapped_disabled("Use \"Share screen\" at the top. Others join this channel to watch you.");
                if (gui::checkbox("Preview own stream", &This->show_own_stream)) {
                  if (auto rt = get_render_thread(); rt) {
                    if (This->show_own_stream) {
                      rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
                    }
                    else {
                      rt->network_frame.set_image(engine.default_texture);
                    }
                  }
                }
                if (!This->show_own_stream && ecps_backend.is_streaming_to_any_channel()) {
                  // the server doesn't send a share back to its host, without this sharing looks broken
                  text_wrapped_disabled("Sharing - viewers in the channel see your screen. Tick the preview to see it here.");
                }
                This->render_share_audio_checkbox();
                This->render_channel_password_controls(This->selected_channel_id);
              }
              if (is_host_of_selected) {
                gui::end_child();
              }

              gui::end_tab_item();
            }

            if (gui::begin_tab_item("Chat", nullptr, 0)) {
              This->render_chat_tab(This->selected_channel_id);
              gui::end_tab_item();
            }

            if (gui::begin_tab_item("Files", nullptr, 0)) {
              This->render_file_transfers();
              gui::end_tab_item();
            }

            if (gui::begin_tab_item("MIDI", nullptr, 0)) {
              This->render_midi_tab();
              gui::end_tab_item();
            }

            if (gui::begin_tab_item("Logs", nullptr, 0)) {
              This->render_logs_tab();
              gui::end_tab_item();
            }

            if (is_host_of_selected) {
              if (gui::begin_tab_item("Stream Settings", nullptr, detail_tab == 1 ? gui::tab_item_flags_set_selected : 0)) {
                if (detail_tab == 1) detail_tab = -1;

                render_stream_settings_content_compact();
                gui::end_tab_item();
              }
            }

            gui::end_tab_bar();
          }
        }
        else {
          gui::spacing();
          gui::text("Select a channel.");
        }

        gui::end_group();
        gui::end_child();
      }
    }

    // The panel beside the stream. Sections for what you share, what you watch and how it shows, then the details
    // folded away. Each setting is one row, its name at the left and its control at the right, and what is on or off
    // is a switch
    void render_stream_settings_content_compact() {
      gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, 8.f));
      gui::push_style_var(gui::style_var_frame_rounding, 5.f);
      bool streaming = ecps_backend.is_streaming_to_any_channel();
      bool viewing = ecps_backend.is_viewing_any_channel();
      auto* rt = get_render_thread();

      // ---- what you share
      settings_section("SHARING");
      This->render_capture_source();
      This->render_camera_source();
      if (streaming && rt) {
        This->resolution_controls.render_resolution_controls();

        settings_row("Framerate", [&] {
          gui::slider("##framerate_compact", &This->stream_settings.framerate, 3.f, 144.f);
          if (gui::is_item_deactivated_after_edit()) {
            rt->screen_encoder.config_.frame_rate = This->stream_settings.framerate;
            std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
            rt->screen_encoder.update_flags |= codec_update_e::frame_rate;
          }
        });

        settings_row("Bitrate", [&] {
          const char* bitrate_modes[] = { "Automatic", "Manual" };
          if (gui::combo("##bitrate_mode", &This->stream_settings.bitrate_mode, bitrate_modes, 2)) {
            This->stream_settings.use_adaptive_bitrate = This->stream_settings.bitrate_mode == 0;
          }
        });
        if (This->stream_settings.bitrate_mode == 1) {
          settings_row("Mbit/s", [&] {
            if (gui::slider("##bitrate_compact", &This->stream_settings.bitrate_mbps, 0.5f, 50.0f, gui::slider_flags_always_clamp)) {
              rt->screen_encoder.config_.bitrate = This->stream_settings.bitrate_mbps * 1000000;
              std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
              rt->screen_encoder.update_flags |= codec_update_e::rate_control;
            }
          });
        }

        bool remote_control = This->stream_settings.input_control != 0;
        if (settings_switch("Remote control", &remote_control,
          "Viewers can use your mouse and keyboard")) {
          This->stream_settings.input_control = remote_control ? 1 : 0;
          This->send_input_control_flag();
        }

        This->render_share_audio_checkbox();
      }
      else if (!streaming) {
        text_wrapped_disabled("Quality, audio and remote control show here while you share.");
      }

      // ---- what you watch
      if (viewing) {
        settings_section("WATCHING");
        This->render_play_audio_controls();
      }

      // ---- on the graphics card or on the processor
      This->render_video_choice();

      // ---- how it shows
      settings_section("DISPLAY");
      settings_switch("Stats over the picture", &This->show_stream_stats,
        "Drag the box to move it");

      // ---- the details, folded away
      gui::dummy(fan::vec2(0, 6.f));
      if (settings_fold("Advanced", &advanced_open)) {
        if (streaming && rt) {
          render_encoder_row(rt);
          render_scaling_row(rt);
        }
        if (viewing) {
          render_decoder_row();
        }
        if (streaming) {
          if (gui::button("Reset quality settings")) {
            This->stream_settings.selected_resolution = 0;
            This->stream_settings.framerate = 30;
            This->stream_settings.bitrate_mbps = 5;
            This->stream_settings.input_control = 0;
            This->send_input_control_flag();
          }
          if (gui::is_item_hovered()) {
            gui::set_tooltip("30 fps, 5 Mbit/s, remote control off");
          }
        }
        else if (!viewing) {
          text_wrapped_disabled("Encoder and decoder settings show while you share or watch.");
        }
      }
      if (settings_fold("Network", &network_open)) {
        render_network_details();
      }
      gui::pop_style_var(2);
    }
    bool advanced_open = false;
    bool network_open = false;

    void render_encoder_row(render_thread_t* rt) {
      static auto encoder_names = [] {
        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) {
          return std::vector<std::string>{"libx264"};
        }
        return rt->screen_encoder.get_encoders();
      }();
      static auto encoder_options = [] {
        std::vector<const char*> names;
        for (const auto& name : encoder_names) {
          names.push_back(name.c_str());
        }
        return names;
      }();

      settings_row("Encoder", [&] {
        if (!gui::combo("##encoder_compact", &This->stream_settings.selected_encoder, encoder_options.data(), encoder_options.size())) {
          return;
        }
        std::string encoder_name = encoder_names[This->stream_settings.selected_encoder];
        codec_config_t::codec_type_e codec_type = codec_config_t::H264;
        if (encoder_name.find("265") != std::string::npos || encoder_name.find("hevc") != std::string::npos) {
          codec_type = codec_config_t::H265;
        }
        else if (encoder_name.find("av1") != std::string::npos) {
          codec_type = codec_config_t::AV1;
        }
        rt->screen_encoder.config_.codec = codec_type;
        rt->screen_encoder.new_codec = This->stream_settings.selected_encoder;
        rt->screen_encoder.update_flags |= codec_update_e::codec;
        rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
      });
    }

    void render_scaling_row(render_thread_t* rt) {
      static auto quality_options = []() {
        std::vector<const char*> options;
        for (int i = 0; i < 5; ++i) {
          options.push_back(fan::graphics::get_scaling_quality_name(static_cast<codec_config_t::scaling_quality_e>(i)));
        }
        return options;
      }();
      settings_row("Scaling", [&] {
        if (gui::combo("##scaling_quality", &This->stream_settings.selected_scaling_quality, quality_options.data(), quality_options.size())) {
          rt->screen_encoder.update_scaling_quality(static_cast<codec_config_t::scaling_quality_e>(This->stream_settings.selected_scaling_quality));
        }
      });
    }

    void render_decoder_row() {
      static auto decoder_names = [] {
        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) {
          return std::vector<std::string>{"h264_cuvid", "auto-detect", "libx264", "libx265", "libaom-av1"};
        }
        return rt->screen_decoder.get_decoders();
      }();
      static bool decoder_inited = false;
      if (!decoder_inited) {
        decoder_inited = true;
        if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
          for (int i = 0; i < decoder_names.size(); i++) {
            if (decoder_names[i] == rt->screen_decoder.name) {
              This->stream_settings.selected_decoder = i;
              break;
            }
          }
          if (This->stream_settings.selected_decoder >= decoder_names.size()) {
            for (int i = 0; i < decoder_names.size(); i++) {
              if (decoder_names[i] == "auto-detect") {
                This->stream_settings.selected_decoder = i;
                break;
              }
            }
          }
        }
      }
      static auto decoder_options = [] {
        std::vector<const char*> names;
        for (const auto& name : decoder_names) {
          names.push_back(name.c_str());
        }
        return names;
      }();

      settings_row("Decoder", [&] {
        if (!gui::combo("##decoder_compact", &This->stream_settings.selected_decoder, decoder_options.data(), decoder_options.size())) {
          return;
        }
        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) {
          return;
        }
        std::string decoder_name = decoder_names[This->stream_settings.selected_decoder];
        std::string codec_type = "h264";
        if (decoder_name.find("hevc") != std::string::npos) {
          codec_type = "hevc";
        }
        else if (decoder_name.find("av1") != std::string::npos) {
          codec_type = "av1";
        }
        rt->screen_decoder.new_codec = This->stream_settings.selected_decoder;
        rt->screen_decoder.update_flags |= codec_update_e::codec;
        rt->screen_decoder.name = codec_type;
        This->request_idr_reset();
      });
    }

    // a name at the left and a value at the right, for the network details
    static void stat_row(const std::string& label, const std::string& value, fan::color color = fan::color(0.9f, 0.9f, 0.9f, 1.f)) {
      f32_t start = gui::get_cursor_pos_x();
      f32_t right = start + gui::get_content_region_avail().x;
      gui::text(fan::color(0.55f, 0.55f, 0.6f, 1.f), label);
      f32_t label_end = start + gui::calc_text_size(label).x + 10.f;
      gui::same_line();
      gui::set_cursor_pos_x(std::max(label_end, right - gui::calc_text_size(value).x));
      gui::text(color, value);
    }

    void render_network_details() {
      auto& stats = ecps_backend.view.m_stats;
      fan::color good(0.5f, 1.f, 0.6f, 1.f);
      fan::color bad(1.f, 0.5f, 0.5f, 1.f);

      stat_row("Ping", fan::to_string(ecps_backend.ping_ms, 1) + " ms");
      if (auto* rt = get_render_thread(); rt && ecps_backend.is_streaming_to_any_channel()) {
        stat_row("Encode path", rt->screen_encoder.encoder_.uses_gpu_frames() ? "GPU" : "CPU");
      }
      stat_row("Capture", fan::to_string(stage_times.capture.ms.load(), 2) + " ms");
      stat_row("Encode", fan::to_string(stage_times.encode.ms.load(), 2) + " ms");
      stat_row("Decode", fan::to_string(stage_times.decode.ms.load(), 2) + " ms");
      stat_row("Upload", fan::to_string(stage_times.upload.ms.load(), 2) + " ms");

      gui::separator();
      stat_row("Frames", std::to_string(stats.Frame_Total));
      stat_row("Frames dropped", std::to_string(stats.Frame_Drop));
      if (stats.Frame_Total > 0) {
        double rate = (double)stats.Frame_Drop * 100.0 / stats.Frame_Total;
        stat_row("Frame drop rate", fan::to_string(rate, 2) + " %", rate > 5.0 ? bad : good);
      }
      std::uint64_t packet_drops = stats.Packet_HeadDrop + stats.Packet_BodyDrop;
      stat_row("Packets", std::to_string(stats.Packet_Total));
      stat_row("Packets lost", std::to_string(packet_drops));
      if (gui::is_item_hovered()) {
        gui::set_tooltip(std::to_string(stats.Packet_HeadDrop) + " first parts, " + std::to_string(stats.Packet_BodyDrop) + " other parts");
      }
      if (stats.Packet_Total > 0) {
        double rate = (double)packet_drops * 100.0 / stats.Packet_Total;
        stat_row("Packet loss", fan::to_string(rate, 2) + " %", rate > 2.0 ? bad : good);
      }

      if (ecps_backend.is_streaming_to_any_channel()) {
        gui::separator();
        auto& flow = ecps_backend.share.m_NetworkFlow;
        double fill = flow.BucketSize ? (double)flow.Bucket * 100.0 / flow.BucketSize : 0.0;
        stat_row("Send budget", fan::to_string(fill, 0) + " %", fill < 20.0 ? bad : good);
        if (gui::is_item_hovered()) {
          gui::set_tooltip("Low: the stream is held back");
        }
      }

      if (gui::button("Reset counters")) {
        stats.Frame_Drop = 0;
        stats.Frame_Total = 0;
        stats.Packet_Total = 0;
        stats.Packet_HeadDrop = 0;
        stats.Packet_BodyDrop = 0;
      }
    }

    // The stream and the side panel (settings or chat). The panel sits on the right, at the bottom or on the left:
    // drag its Settings or Chat tab over the stream and drop it near an edge. The line between them resizes it
    void process_stream_settings() {
      This->load_side_panel_layout();
      fan::vec2 avail = gui::get_content_region_avail();
      f32_t left_padding = 8.f;
      f32_t right_padding = 12.f;
      f32_t handle = 14.f;
      fan::vec2 origin = gui::get_cursor_screen_pos() + fan::vec2(left_padding, 0);
      fan::vec2 area(avail.x - left_padding - right_padding, avail.y - gui::get_style().ItemSpacing.y);
      if (area.x < 100.f || area.y < 100.f) {
        return;
      }
      This->side_view_min = origin;
      This->side_view_max = origin + area;
      int side = This->side_panel_side;
      fan::vec2 stream_at, stream_size, handle_at, handle_size, panel_at, panel_size;
      if (side == 1) {
        f32_t height = std::clamp(This->side_panel_height, 150.f, area.y * 0.7f);
        stream_at = origin;
        stream_size = fan::vec2(area.x, area.y - height - handle);
        handle_at = origin + fan::vec2(0, stream_size.y);
        handle_size = fan::vec2(area.x, handle);
        panel_at = origin + fan::vec2(0, stream_size.y + handle);
        panel_size = fan::vec2(area.x, height);
      }
      else {
        f32_t width = std::clamp(This->stream_settings.settings_panel_width, 250.f, area.x * 0.6f);
        f32_t stream_width = area.x - width - handle;
        stream_at = side == 0 ? origin : origin + fan::vec2(width + handle, 0);
        stream_size = fan::vec2(stream_width, area.y);
        handle_at = origin + fan::vec2(side == 0 ? stream_width : width, 0);
        handle_size = fan::vec2(handle, area.y);
        panel_at = side == 0 ? origin + fan::vec2(stream_width + handle, 0) : origin;
        panel_size = fan::vec2(width, area.y);
      }

      gui::set_cursor_screen_pos(stream_at);
      render_stream_area("##stream_display_split", stream_size);

      gui::set_cursor_screen_pos(handle_at);
      This->render_side_panel_handle(handle_size, side);

      gui::set_cursor_screen_pos(panel_at);
      gui::begin_child("##stream_settings_panel", panel_size, true);
      This->render_side_panel_content();
      gui::end_child();

      This->render_side_panel_drop();
    }

    void process_no_stream_settings() {
      fan::vec2 avail_size = gui::get_content_region_avail();
      // the toolbar floats over the picture now, the stream gets the whole height
      f32_t content_height = avail_size.y - gui::get_style().ItemSpacing.y;
      f32_t side_padding = 12.0f;

      gui::dummy(fan::vec2(side_padding, 0));
      gui::same_line(0, 0);

      if (ecps_backend.channel_info.empty()) {
        if (auto rt = get_render_thread(); rt) {
          rt->network_frame.set_image(engine.default_texture);
        }
      }
      render_stream_area("##stream_display", fan::vec2(avail_size.x - (side_padding * 2), content_height));
    }

    // the picture, the pointer over it, the text when there is none, and the toolbar
    void render_stream_area(const char* id, fan::vec2 size) {
      gui::set_next_window_bg_alpha(0);
      gui::begin_child(id, size, false);
      gui::set_viewport(engine.orthographic_render_view);
      This->stream_area_min = gui::get_window_pos();
      This->stream_area_max = This->stream_area_min + size;
      This->stream_area_shown = true;

      if (auto rt = get_render_thread(); rt) {
        fan::vec2 stream_area = gui::get_content_region_avail();
        This->network_frame_helper.setup_network_frame(stream_area);
        This->render_fps_counter(size.x);
        This->draw_host_cursor();

        if (rt->network_frame.get_image() == engine.default_texture) {
          This->render_stream_empty_hint(stream_area);
        }
        else {
          fan::vec2 half = stream_area_calculator_t::calculate_fitted_size(stream_area, rt->screen_decoder.decoded_size);
          fan::vec2 center = gui::get_window_pos() + stream_area * 0.5f;
          This->picture_min = center - half;
          This->picture_max = center + half;
          This->picture_shown = true;
        }
#if ecps_debug_prints >= 3
        static int debug_counter = 0;
        if (++debug_counter % 300 == 1) {
          fan::vec2 fitted_size = stream_area_calculator_t::calculate_fitted_size(stream_area, rt->screen_decoder.decoded_size);
          f32_t frame_aspect = 16.0f / 9.0f;
          if (rt->screen_decoder.decoded_size.x > 0 && rt->screen_decoder.decoded_size.y > 0) {
            frame_aspect = static_cast<f32_t>(rt->screen_decoder.decoded_size.x) / static_cast<f32_t>(rt->screen_decoder.decoded_size.y);
          }
          fan::printf("VIEWER: stream_area={}x{}, decoded={}x{}, aspect={:.3f}, display_size={}x{}",
            stream_area.x, stream_area.y,
            rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y,
            frame_aspect, fitted_size.x * 2, fitted_size.y * 2);
        }
#endif
      }

      This->render_stream_toolbar();
      gui::end_child();
    }

    void render_stream_view_content() {
      gui::spacing();
      gui::spacing();

      fan::vec2 avail_size = gui::get_content_region_avail();

      // the toolbar floats over the picture now, the stream gets the whole height
      f32_t content_height = avail_size.y - gui::get_style().ItemSpacing.y;

      if (avail_size.x > 0 && content_height > 0) {
        if (This->stream_settings.show_in_stream_view) {
          process_stream_settings();
        }
        else {
          process_no_stream_settings();
        }
      }

      gui::spacing();
    }

    int main_tab = 0;
    int detail_tab = 0;
    int current_tab = 0;
    bool p_open = true;
    bool auto_refresh = true;
    int refresh_interval = 1;
    std::string search_filter;
  }window_handler;

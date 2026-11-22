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
          // 0 was the Channels tab: now the channels panel of the main view
          if (main_tab == 0) {
            This->open_side_panel(side_channels);
            main_tab = 1;
          }
          current_tab = main_tab;
          main_tab = -1;
        }
        if (This->ui_autohide && current_tab == 1) {
          This->report_display_sizes();
          updater.tick();
          updater.tick_restart();
          render_stream_view_content();
          This->top_bar.render_about_popup();
          This->render_password_popup();
        }
        else {
        This->top_bar.render_top_bar();

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

        // the bar above holds the tabs, the one chosen fills the rest. The channels are a panel of the main view
        switch (current_tab) {
          case 1: render_stream_view_content(); break;
          case 2: This->render_debug_tab(); break;
          default: current_tab = 1; break;
        }
        }
      }
      gui::end();
      if (This->ui_autohide && current_tab == 1) {
        This->top_bar.render_autohide_bar(window_pos, window_size);
      }
      This->pop_theme();
    }

    // the channels panel, made for a side bar: the buttons and the search on top, the channels as rows, the selected
    // channel's details under them at the full width. The line between the list and the details moves
    f32_t channels_list_height = 0.f; // 0: as tall as the rows need
    bool channels_list_resizing = false;

    static std::string channel_live_time(std::uint64_t ns) {
      std::uint64_t s = ns / 1000000000ull;
      return s >= 3600 ? fan::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60) : fan::format("{}:{:02}", s / 60, s % 60);
    }

    void render_channel_row(const ecps_backend_t::channel_list_info_t& channel, f32_t width) {
      bool is_selected = This->selected_channel_id.i == channel.channel_id.i;
      bool mine = ecps_backend.is_current_user_host_of_channel(channel.channel_id);
      bool joined = ecps_backend.is_channel_joined(channel.channel_id);
      std::uint64_t live_ns = ecps_backend.get_channel_stream_time(channel.channel_id);
      bool host_here = channel.host_session_id.i != decltype(channel.host_session_id.i)(-1);
      int viewers = (int)channel.user_count - (host_here && channel.user_count > 0 ? 1 : 0);

      f32_t row_height = gui::get_frame_height() + 10.f;
      fan::vec2 at = gui::get_cursor_screen_pos();
      gui::push_id((int)channel.channel_id.i);
      bool clicked = gui::invisible_button("##channel_row", fan::vec2(width, row_height));
      bool hovered = gui::is_item_hovered();
      gui::pop_id();
      if (clicked) {
        This->selected_channel_id = channel.channel_id;
        // a joined channel opens its own view, the users in it
        if (joined) {
          channels_browsing = false;
        }
        This->backend_queue([channel_id = channel.channel_id]() -> fan::event::task_t {
          try {
            co_await ecps_backend.request_channel_session_list(channel_id);
          }
          catch (...) {
            fan::print("Failed to request session list");
          }
        });
      }
      auto* list = gui::get_window_draw_list();
      if (is_selected || hovered) {
        list->AddRectFilled(at, at + fan::vec2(width, row_height), is_selected ? 0xff3a170cu : 0xff1c0f09u, 6.f);
      }
      if (is_selected) {
        list->AddRectFilled(at + fan::vec2(0, 6.f), at + fan::vec2(3.f, row_height - 6.f), 0xffff7a33u, 1.5f);
      }
      f32_t text_y = at.y + (row_height - gui::get_font_size()) * 0.5f;
      // a red dot while someone shares, grey otherwise
      f32_t dot = 4.f;
      list->AddCircleFilled(fan::vec2(at.x + 14.f, at.y + row_height * 0.5f), dot, live_ns ? 0xff4040ffu : 0x40ffffffu, 12);

      // at the right: how many watch, and for how long it has been live
      std::string right = std::to_string(std::max(viewers, 0)) + (viewers == 1 ? " viewer" : " viewers");
      if (live_ns) {
        right = channel_live_time(live_ns) + "   " + right;
      }
      f32_t right_width = gui::calc_text_size(right).x;
      f32_t name_x = at.x + 28.f;
      f32_t room = std::max(width - (name_x - at.x) - right_width - 18.f, 30.f);
      std::string name = gui::fit_title(channel.name, room);
      std::uint32_t name_ink = is_selected || joined ? 0xffffffffu : 0xffd0d0d0u;
      list->AddText(fan::vec2(name_x, text_y), name_ink, name.c_str());
      f32_t after_name = name_x + gui::calc_text_size(name).x + 8.f;
      // small tags after the name: yours, joined, a password
      auto tag = [&](const char* text, std::uint32_t fill, std::uint32_t ink) {
        fan::vec2 size = gui::calc_text_size(text);
        if (after_name + size.x + 12.f > at.x + width - right_width - 12.f) {
          return;
        }
        fan::vec2 a(after_name, text_y - 1.f);
        list->AddRectFilled(a, a + size + fan::vec2(12.f, 2.f), fill, (size.y + 2.f) * 0.5f);
        list->AddText(a + fan::vec2(6.f, 1.f), ink, text);
        after_name += size.x + 18.f;
      };
      if (mine) {
        tag("yours", accent_alpha(accent_u32, 0x50), accent_hover_u32);
      }
      else if (joined) {
        tag("joined", 0x5040a040u, 0xff90ff90u);
      }
      if (channel.is_password_protected) {
        tag("password", 0x40909090u, 0xffc0c0c0u);
      }
      list->AddText(fan::vec2(at.x + width - right_width - 10.f, text_y), 0xff909090u, right.c_str());
    }

    // the server's channels (true) or the joined channel's own view (false). Joining a channel shows its view, the
    // back button in it the channels again
    bool channels_browsing = false;
    std::size_t joined_seen = 0;

    // "< Channels": back to the server's channels from a joined channel's view
    bool render_back_button(f32_t height) {
      const char* text = "Channels";
      f32_t chevron = height * 0.22f;
      fan::vec2 size(gui::calc_text_size(text).x + chevron * 2.f + 26.f, height);
      fan::vec2 at = gui::get_cursor_screen_pos();
      bool clicked = gui::invisible_button("##channels_back", size);
      bool hovered = gui::is_item_hovered();
      if (hovered) {
        gui::set_tooltip("Back to all the channels of the server");
      }
      auto* list = gui::get_window_draw_list();
      list->AddRectFilled(at, at + size, hovered ? 0xff2a1a12u : 0xff1c0f09u, height * 0.3f);
      std::uint32_t ink = hovered ? 0xffffffffu : 0xffc8c8c8u;
      fan::vec2 c = at + fan::vec2(10.f + chevron * 0.5f, height * 0.5f);
      list->AddLine(c + fan::vec2(chevron * 0.5f, -chevron), c + fan::vec2(-chevron * 0.5f, 0), ink, 2.f);
      list->AddLine(c + fan::vec2(-chevron * 0.5f, 0), c + fan::vec2(chevron * 0.5f, chevron), ink, 2.f);
      list->AddText(at + fan::vec2(chevron * 2.f + 16.f, (height - gui::get_font_size()) * 0.5f), ink, text);
      return clicked;
    }

    void leave_channel(ecps_backend_t::Protocol_ChannelID_t channel_id) {
      // a channel joined by itself would be joined again at once, leaving it stops that
      for (const auto& channel : ecps_backend.available_channels) {
        if (channel.channel_id.i == channel_id.i && This->auto_join_host == channel.name) {
          This->set_auto_join(std::string());
        }
      }
      This->backend_queue([channel_id]() -> fan::event::task_t {
        try {
          co_await ecps_backend.channel_quit(channel_id);
          co_await ecps_backend.request_channel_list();
        }
        catch (...) {
          fan::print("Failed to leave channel");
        }
      });
    }

    // a joined channel: the way back to the channels, leaving, and the channel's users and tabs over all the panel
    void render_joined_channel_view() {
      f32_t button = gui::get_frame_height();
      if (render_back_button(button)) {
        channels_browsing = true;
      }
      if (!ecps_backend.is_current_user_host_of_channel(This->selected_channel_id)) {
        gui::same_line();
        gui::set_cursor_pos_x(gui::get_cursor_pos_x() + std::max(gui::get_content_region_avail().x - button, 0.f));
        if (icon_button("##leave_joined_channel", icon_e::leave, "Leave this channel", button, icon_style_e::danger)) {
          leave_channel(This->selected_channel_id);
          channels_browsing = true;
        }
      }
      gui::spacing();
      render_channel_details(gui::get_content_region_avail());
    }

    void render_channels_content() {
      fan::vec2 avail_size = gui::get_content_region_avail();
      if (avail_size.x <= 0 || avail_size.y <= 0) {
        return;
      }
      // a channel joined since the last frame (here, by itself or from elsewhere): its view
      if (ecps_backend.channel_info.size() > joined_seen) {
        This->selected_channel_id = ecps_backend.channel_info.back().channel_id;
        channels_browsing = false;
      }
      joined_seen = ecps_backend.channel_info.size();
      bool has_selection = This->selected_channel_id.i != (uint16_t)-1;
      bool already_in_selected = has_selection && ecps_backend.is_channel_joined(This->selected_channel_id);
      bool is_host_of_selected = already_in_selected && ecps_backend.is_current_user_host_of_channel(This->selected_channel_id);
      if (already_in_selected && !channels_browsing) {
        render_joined_channel_view();
        return;
      }

      // ---- the top row: the search over the width, the buttons at its right
      f32_t button = gui::get_frame_height();
      int buttons = 2 + (has_selection && !is_host_of_selected ? 1 : 0);
      f32_t buttons_width = buttons * (button + gui::get_style().ItemSpacing.x);
      fan::vec2 search_at = gui::get_cursor_screen_pos();
      gui::push_item_width(std::max(avail_size.x - buttons_width, 80.f));
      gui::input_text("##channel_search", &search_filter);
      bool search_active = gui::is_item_active();
      gui::pop_item_width();
      if (search_filter.empty() && !search_active) {
        gui::get_window_draw_list()->AddText(search_at + fan::vec2(gui::get_style().FramePadding.x, gui::get_style().FramePadding.y), 0xff808080u, "Search channels");
      }
      if (has_selection && !is_host_of_selected) {
        gui::same_line();
        if (already_in_selected) {
          if (icon_button("##leave_channel", icon_e::leave, "Leave the selected channel", button, icon_style_e::danger)) {
            leave_channel(This->selected_channel_id);
          }
        }
        else if (icon_button("##join_channel", icon_e::join, "Join the selected channel", button, icon_style_e::accent)) {
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
      }
      gui::same_line();
      if (icon_button("##add_channel", icon_e::plus, "New channel", button)) {
        This->new_channel_popup_requested = true;
      }
      gui::same_line();
      if (icon_button("##connect_server", icon_e::server, "Connect to a server", button)) {
        This->drop_down_server.toggle_render_server_connect = true;
      }
      gui::spacing();

      // ---- the channels
      std::vector<const ecps_backend_t::channel_list_info_t*> shown;
      for (const auto& channel : ecps_backend.available_channels) {
        if (!search_filter.empty()) {
          std::string name_lower = channel.name;
          std::string filter_lower = search_filter;
          std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
          std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
          if (name_lower.find(filter_lower) == std::string::npos) {
            continue;
          }
        }
        shown.push_back(&channel);
      }
      f32_t rest = gui::get_content_region_avail().y;
      f32_t row_height = gui::get_frame_height() + 10.f + 2.f;
      f32_t padding = gui::get_style().WindowPadding.y * 2.f;
      f32_t fitted = std::max<std::size_t>(shown.size(), 1) * row_height + padding + 4.f;
      f32_t list_height = channels_list_height > 0.f ? channels_list_height : fitted;
      list_height = std::clamp(list_height, std::min(row_height * 2.f + padding, rest * 0.3f), rest * 0.6f);
      f32_t handle = 12.f;

      gui::push_style_var(gui::style_var_item_spacing, fan::vec2(gui::get_style().ItemSpacing.x, 2.f));
      if (gui::begin_child("##channel_list_panel", fan::vec2(avail_size.x, list_height), true)) {
        f32_t width = gui::get_content_region_avail().x;
        for (const auto* channel : shown) {
          render_channel_row(*channel, width);
        }
        if (ecps_backend.available_channels.empty()) {
          gui::text_wrapped_disabled(ecps_backend.channel_list_received
            ? "No channels yet. Make one with +"
            : "Loading the channels...");
        }
        else if (shown.empty()) {
          gui::text_disabled("No channel matches the search");
        }
      }
      gui::end_child();
      gui::pop_style_var();

      // the line between the list and the details: dragged, the list gets taller or shorter
      {
        fan::vec2 at = gui::get_cursor_screen_pos();
        gui::invisible_button("##channel_stack_handle", fan::vec2(avail_size.x, handle));
        bool hot = gui::is_item_hovered() || gui::is_item_active();
        if (hot) {
          gui::set_mouse_cursor(gui::mouse_cursor_resize_ns);
          gui::set_tooltip("Drag to make the list taller or shorter, double click to fit it to the channels");
        }
        if (gui::is_item_active()) {
          channels_list_height = list_height + gui::get_io().MouseDelta.y;
          channels_list_resizing = true;
        }
        else if (channels_list_resizing) {
          channels_list_resizing = false;
          This->save_side_panel_layout();
        }
        if (hot && gui::is_mouse_double_clicked(0)) {
          channels_list_height = 0.f;
          This->save_side_panel_layout();
        }
        fan::vec2 c = at + fan::vec2(avail_size.x * 0.5f, handle * 0.5f);
        gui::get_window_draw_list()->AddLine(fan::vec2(c.x - 24.f, c.y), fan::vec2(c.x + 24.f, c.y), hot ? 0xffff7a33u : 0x30ffffffu, 3.f);
      }

      render_channel_details(fan::vec2(avail_size.x, std::max(gui::get_content_region_avail().y, 60.f)));
    }

    // the selected channel: its name, the users in it and its tabs
    void render_channel_details(fan::vec2 size) {
      bool has_selection = This->selected_channel_id.i != (uint16_t)-1;
      bool already_in_selected = has_selection && ecps_backend.is_channel_joined(This->selected_channel_id);
      bool is_host_of_selected = already_in_selected && ecps_backend.is_current_user_host_of_channel(This->selected_channel_id);
      gui::begin_child("##channel_details_panel", size, true);

      gui::dummy(fan::vec2(6, 0));
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
          {
            gui::font_scope_t title(gui::get_font_size() * 1.15f, gui::font::bold);
            gui::text(channel_name);
          }
          if (ecps_backend.is_hosting_channel(This->selected_channel_id)) {
            gui::same_line(0, 10.f);
            gui::badge("yours", accent_alpha(accent_u32, 0x50), accent_hover_u32);
          }
          // at the right of the name: the users over the stream on or off, and refreshing them
          {
            f32_t button = gui::get_frame_height();
            gui::same_line();
            gui::set_cursor_pos_x(gui::get_cursor_pos_x() + std::max(gui::get_content_region_avail().x - button * 2.f - 8.f, 0.f));
            if (icon_button("##toggle_stream_users", icon_e::users,
              This->show_stream_users ? "Hide the users over the stream" : "Show the users over the stream", button,
              This->show_stream_users ? icon_style_e::accent : icon_style_e::plain)) {
              This->show_stream_users = !This->show_stream_users;
            }
            gui::same_line(0, 4.f);
            if (icon_button("##refresh_users", icon_e::refresh, "Refresh the users", button)) {
              This->backend_queue([channel_id = This->selected_channel_id]() -> fan::event::task_t {
                try {
                  co_await ecps_backend.request_channel_session_list(channel_id);
                }
                catch (...) {
                  fan::print("Failed to refresh session list");
                }
              });
            }
          }
          if (!ecps_backend.is_hosting_channel(This->selected_channel_id) && !channel_name.empty()) {
            bool auto_join_on = This->auto_join_host == channel_name;
            if (gui::settings_switch("Join automatically", &auto_join_on,
              "Joins this channel by itself whenever its host is online, also after a restart")) {
              This->set_auto_join(auto_join_on ? channel_name : std::string());
            }
          }

          if (already_in_selected) {
            This->render_voice_switch(This->selected_channel_id);
          }
          gui::spacing();

          if (gui::begin_tab_bar("ChannelDetailTabs")) {
            if (gui::begin_tab_item("Users", nullptr, detail_tab == 0 ? gui::tab_item_flags_set_selected : 0)) {
              if (detail_tab == 0) detail_tab = -1;

              gui::spacing();

              if (gui::begin_child("SessionList", fan::vec2(0, 0), true)) {
                bool open_user_volume = false;
                auto it = ecps_backend.channel_sessions.find(This->selected_channel_id.i);
                if (it != ecps_backend.channel_sessions.end()) {
                  auto sharing_now = ecps_backend.streamers_of(This->selected_channel_id);
                  auto in_voice = ecps_backend.voice_members_of(This->selected_channel_id);
                  for (const auto& session : it->second) {
                    bool is_sharing = std::find(sharing_now.begin(), sharing_now.end(), (std::uint32_t)session.session_id.i) != sharing_now.end();
                    bool is_me = session.session_id.i == ecps_backend.session_id.i;
                    bool has_buttons = already_in_selected && !is_me;
                    gui::push_id((int)session.session_id.i);

                    f32_t button_size = gui::get_frame_height() * 0.8f;
                    f32_t row_height = button_size + 6.f;
                    f32_t row_width = gui::get_content_region_avail().x;
                    fan::vec2 row_at = gui::get_cursor_screen_pos();
                    // the voice mixer has one gain per speaker id, keep it in step with the name it is saved under
                    if (!is_me) {
                      voice_chat.set_user_volume((std::uint32_t)session.session_id.i, This->user_volume(session.username));
                    }
                    f32_t hit_width = row_width - (has_buttons ? button_size + 12.f + (is_sharing ? button_size + 4.f : 0.f) : 0.f);
                    fan::vec2 hit_end = row_at + fan::vec2(std::max(hit_width, 0.f), row_height);
                    if (gui::is_mouse_hovering_rect(row_at, hit_end)) {
                      gui::get_window_draw_list()->AddRectFilled(row_at, hit_end, 0xff0b0706u, 6.f);
                      if (!is_me && fan::window::is_mouse_clicked(1)) {
                        This->volume_user_session = (std::uint32_t)session.session_id.i;
                        This->volume_user_name = session.username;
                        This->volume_user_value = This->user_volume(session.username);
                        // open after pop_id(): a popup's id includes the id stack, and begin_popup runs outside it
                        open_user_volume = true;
                      }
                    }

                    // in a narrow panel the name and badges stop where the buttons start, they don't run under them
                    gui::push_clip_rect(row_at, fan::vec2(row_at.x + std::max(hit_width - 4.f, 0.f), row_at.y + row_height), true);
                    gui::set_cursor_screen_pos(row_at + fan::vec2(10.f, (row_height - gui::get_font_size()) * 0.5f));
                    gui::text(fan::color(0.9f, 0.9f, 0.9f, 1.f), session.username);
                    if (is_me) {
                      gui::same_line(0, 6.f);
                      gui::text(fan::color(0.4f, 0.4f, 0.45f, 1.f), "(you)");
                    }
                    if (session.is_host) {
                      gui::same_line(0, 10.f);
                      gui::badge("host", accent_alpha(accent_u32, 0x50), accent_hover_u32);
                    }
                    if (is_sharing) {
                      gui::same_line(0, 8.f);
                      gui::badge("live", 0x600a0affu, 0xff6a6affu);
                    }
                    if (std::find(in_voice.begin(), in_voice.end(), (std::uint32_t)session.session_id.i) != in_voice.end()) {
                      std::uint32_t speaker = (std::uint32_t)session.session_id.i;
                      // fast up, slow down, so the meter doesn't flicker between packets
                      f32_t target = voice_chat.talk_level(speaker);
                      f32_t& shown = This->voice_meter_levels[speaker];
                      f32_t dt = std::min(gui::get_io().DeltaTime, 0.1f);
                      shown = target > shown ? shown + (target - shown) * std::min(dt * 30.f, 1.f) : std::max(target, shown - dt * 2.5f);
                      gui::same_line(0, 8.f);
                      // a click mutes: yourself for the others, someone else just for you
                      // "mic off": they muted their own microphone, the server says so. "muted": you muted them
                      bool self_muted = is_me ? voice_chat.muted.load() : ecps_backend.voice_muted_by_self(This->selected_channel_id, speaker);
                      bool locally_muted = !is_me && This->user_volume(session.username) <= 0.f;
                      bool badge_muted = self_muted || locally_muted;
                      if (badge_muted) {
                        gui::badge(locally_muted ? "muted" : "mic off", 0x600a0affu, 0xff8080ffu);
                      }
                      else {
                        gui::level_badge("voice", shown, 0x40909090u, 0xa030c040u, shown > 0.01f ? 0xffb0ffb0u : 0xffb0b0b0u);
                      }
                      if (gui::is_item_hovered()) {
                        gui::set_mouse_cursor(gui::mouse_cursor_hand);
                        gui::set_tooltip(is_me
                          ? (badge_muted ? "Your microphone is muted. Click to unmute it" : "Click to mute your microphone")
                          : (locally_muted ? "Click to hear " + session.username + " again"
                            : self_muted ? session.username + " muted their microphone. Click to mute them for you too"
                            : "Click to mute " + session.username + " for you"));
                      }
                      if (gui::is_item_clicked()) {
                        if (is_me) {
                          This->toggle_mic_mute();
                        }
                        else {
                          This->toggle_user_mute(speaker, session.username);
                        }
                      }
                    }
                    gui::same_line(0, 10.f);
                    gui::text(fan::color(0.3f, 0.3f, 0.34f, 1.f), "#" + std::to_string(session.session_id.i));
                    gui::pop_clip_rect();

                    if (has_buttons) {
                      f32_t x = row_at.x + row_width - 8.f - button_size;
                      f32_t button_y = row_at.y + (row_height - button_size) * 0.5f;
                      gui::set_cursor_screen_pos(fan::vec2(x, button_y));
                      if (icon_button("##send_file", icon_e::send_file, "Send a file", button_size)) {
                        This->send_file_dialog(This->selected_channel_id, session.session_id);
                      }
                      if (is_sharing) {
                        gui::set_cursor_screen_pos(fan::vec2(x - button_size - 4.f, button_y));
                        if (icon_button("##watch", icon_e::eye, "Watch in Stream View", button_size)) {
                          This->set_watch(This->selected_channel_id, (std::uint32_t)session.session_id.i);
                          // to the picture, watching from here would otherwise change nothing you can see
                          main_tab = 1;
                        }
                      }
                    }
                    gui::set_cursor_screen_pos(row_at + fan::vec2(0, row_height + 1.f));
                    gui::dummy(fan::vec2(0, 0));
                    gui::pop_id();
                  }
                }
                else {
                  gui::text("No user data loaded.");
                  gui::text("Select a channel to view users.");
                }
                if (open_user_volume) {
                  gui::open_popup("##user_volume");
                }
                This->render_user_volume_popup();
              }
              gui::end_child();

              gui::end_tab_item();
            }

            if (is_host_of_selected && gui::begin_tab_item("Manage", nullptr, 0)) {
              gui::spacing();
              if (gui::begin_child("HostControls", fan::vec2(0, 0), gui::child_flags_always_use_window_padding)) {
                gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, 10.f));
                if (gui::settings_switch("Preview own stream", &This->show_own_stream, "Shows your own shared screen here")) {
                  if (auto rt = get_render_thread(); rt) {
                    if (This->show_own_stream) {
                      rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
                    }
                    else {
                      rt->network_frame.set_image(engine.default_texture);
                    }
                  }
                }
                This->render_share_audio_checkbox();
                This->render_channel_password_controls(This->selected_channel_id);
                gui::dummy(fan::vec2(0, 8.f));
                This->render_delete_channel_button(This->selected_channel_id);
                gui::pop_style_var();
              }
              gui::end_child();
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
      gui::settings_section("SHARING");
      This->render_capture_source();
      This->render_camera_source();
      if (streaming && rt) {
        This->resolution_controls.render_resolution_controls();

        gui::settings_row("Framerate", [&] {
          gui::slider("##framerate_compact", &This->stream_settings.framerate, 3.f, 144.f);
          if (gui::is_item_deactivated_after_edit()) {
            rt->screen_encoder.config_.frame_rate = This->stream_settings.framerate;
            std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
            rt->screen_encoder.update_flags |= codec_update_e::frame_rate;
          }
        });

        gui::settings_row("Bitrate", [&] {
          const char* bitrate_modes[] = { "Automatic", "Manual" };
          if (gui::combo("##bitrate_mode", &This->stream_settings.bitrate_mode, bitrate_modes, 2)) {
            This->stream_settings.use_adaptive_bitrate = This->stream_settings.bitrate_mode == 0;
          }
        });
        if (This->stream_settings.bitrate_mode == 1) {
          gui::settings_row("Mbit/s", [&] {
            if (gui::slider("##bitrate_compact", &This->stream_settings.bitrate_mbps, 0.5f, 50.0f, gui::slider_flags_always_clamp)) {
              rt->screen_encoder.config_.bitrate = This->stream_settings.bitrate_mbps * 1000000;
              std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
              rt->screen_encoder.update_flags |= codec_update_e::rate_control;
            }
          });
        }

        bool remote_control = This->stream_settings.input_control != 0;
        if (gui::settings_switch("Remote control", &remote_control,
          "Viewers can use your mouse and keyboard")) {
          This->stream_settings.input_control = remote_control ? 1 : 0;
          This->send_input_control_flag();
        }

        This->render_share_audio_checkbox();
      }
      else if (!streaming) {
        gui::text_wrapped_disabled("Quality, audio and remote control show here while you share.");
      }

      // ---- what you watch
      if (viewing) {
        gui::settings_section("WATCHING");
        This->render_play_audio_controls();
      }

      // ---- on the graphics card or on the processor
      This->render_video_choice();

      // ---- how it shows
      gui::settings_section("DISPLAY");
      gui::settings_switch("Stats over the picture", &This->show_stream_stats,
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
          gui::text_wrapped_disabled("Encoder and decoder settings show while you share or watch.");
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

      gui::settings_row("Encoder", [&] {
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
      gui::settings_row("Scaling", [&] {
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

      gui::settings_row("Decoder", [&] {
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

    // The stream and its panels: Channels, Settings and Chat each on the right, at the bottom or on the left (more
    // on one side share a panel with a tab each). The lines between them resize them
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
      bool right = !This->panels_on(0).empty();
      bool bottom = !This->panels_on(1).empty();
      bool left = !This->panels_on(2).empty();
      // a side pushed against its edge: a grip there brings it back
      f32_t grip = This->side_grip_size;
      bool right_grip = This->side_hidden[0] && !This->panels_docked(0).empty();
      bool bottom_grip = This->side_hidden[1] && !This->panels_docked(1).empty();
      bool left_grip = This->side_hidden[2] && !This->panels_docked(2).empty();
      f32_t height = bottom ? std::clamp(This->side_panel_height, 150.f, area.y * 0.7f) : 0.f;
      f32_t top_height = area.y - (bottom ? height + handle : bottom_grip ? grip : 0.f);
      // the left one starts wide, the channels in it want room
      if (This->side_left_width <= 0.f) {
        This->side_left_width = std::max(area.x * 0.42f, 420.f);
      }
      f32_t share = left && right ? 0.4f : 0.7f;
      f32_t right_width = std::clamp(This->stream_settings.settings_panel_width, 250.f, std::max(250.f, area.x * share));
      f32_t left_width = std::clamp(This->side_left_width, 250.f, std::max(250.f, area.x * share));
      auto panel = [&](int side, fan::vec2 at, fan::vec2 size) {
        gui::set_cursor_screen_pos(at);
        gui::begin_child(("##side_panel_" + std::to_string(side)).c_str(), size, true);
        This->render_side_panel_content(side);
        gui::end_child();
      };
      f32_t x = origin.x;
      if (left) {
        panel(2, origin, fan::vec2(left_width, top_height));
        gui::set_cursor_screen_pos(fan::vec2(origin.x + left_width, origin.y));
        This->render_side_panel_handle(fan::vec2(handle, top_height), 2);
        x += left_width + handle;
      }
      else if (left_grip) {
        gui::set_cursor_screen_pos(origin);
        This->render_side_grip(fan::vec2(grip, top_height), 2);
        x += grip;
      }
      f32_t stream_width = area.x - (x - origin.x) - (right ? right_width + handle : right_grip ? grip : 0.f);
      gui::set_cursor_screen_pos(fan::vec2(x, origin.y));
      render_stream_area("##stream_display_split", fan::vec2(stream_width, top_height));
      if (right) {
        gui::set_cursor_screen_pos(fan::vec2(x + stream_width, origin.y));
        This->render_side_panel_handle(fan::vec2(handle, top_height), 0);
        panel(0, fan::vec2(x + stream_width + handle, origin.y), fan::vec2(right_width, top_height));
      }
      else if (right_grip) {
        gui::set_cursor_screen_pos(fan::vec2(x + stream_width, origin.y));
        This->render_side_grip(fan::vec2(grip, top_height), 0);
      }
      if (bottom) {
        gui::set_cursor_screen_pos(fan::vec2(origin.x, origin.y + top_height));
        This->render_side_panel_handle(fan::vec2(area.x, handle), 1);
        panel(1, fan::vec2(origin.x, origin.y + top_height + handle), fan::vec2(area.x, height));
      }
      else if (bottom_grip) {
        gui::set_cursor_screen_pos(fan::vec2(origin.x, origin.y + top_height));
        This->render_side_grip(fan::vec2(area.x, grip), 1);
      }
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
          fan::vec2 half = gui::fit_size(stream_area, rt->screen_decoder.decoded_size) *
            This->network_frame_helper.zoom;
          fan::vec2 center = gui::get_window_pos() + stream_area * 0.5f + This->network_frame_helper.offset;
          This->picture_min = center - half;
          This->picture_max = center + half;
          This->picture_shown = true;
        }
#if ecps_debug_prints >= 3
        static int debug_counter = 0;
        if (++debug_counter % 300 == 1) {
          fan::vec2 fitted_size = gui::fit_size(stream_area, rt->screen_decoder.decoded_size);
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
        if (This->any_side_panel_open()) {
          process_stream_settings();
        }
        else {
          process_no_stream_settings();
        }
      }

      gui::spacing();
    }

    int main_tab = -1;
    int detail_tab = 0;
    int current_tab = 1; // 1 the main view, 2 Debug
    bool p_open = true;
    bool auto_refresh = true;
    int refresh_interval = 1;
    std::string search_filter;
  }window_handler;

// Main window implementation, out of line.
#include "gui_std.h" // precompiled, first
#include "ecps_app.h"


void render_thread_t::ecps_gui_t::channel_list_window_t::render() {
  auto& gui = *OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui.owner->engine;

      if (!p_open) return;

      auto* viewport = gui::get_main_viewport();
      fan::vec2 window_pos = fan::vec2(viewport->WorkPos.x, viewport->WorkPos.y);
      fan::vec2 window_size = fan::vec2(viewport->WorkSize.x, viewport->WorkSize.y);
      gui::set_next_window_pos(window_pos);
      gui::set_next_window_size(window_size);
      gui::set_next_window_bg_alpha(0.99);
      gui.push_theme();
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
            gui.open_side_panel(side_channels);
            main_tab = 1;
          }
          current_tab = main_tab;
          main_tab = -1;
        }
        if (gui.ui_autohide && current_tab == 1) {
          gui.report_display_sizes();
          updater.tick();
          updater.tick_restart();
          render_stream_view_content();
          gui.top_bar.render_about_popup();
        }
        else {
        gui.top_bar.render_top_bar();

        static bool first_open = true;
        if (first_open && p_open) {
          first_open = false;
          gui.backend_queue([&]() -> fan::event::task_t {
            if (auto listed = co_await ecps_expected(ecps_backend.request_channel_list()); !listed) {
              fan::print("Failed to auto-refresh channel list: " + listed.error());
              co_return;
            }
            if (gui.selected_channel_id.i != (uint16_t)-1) {
              (void)co_await ecps_expected(ecps_backend.request_channel_session_list(gui.selected_channel_id));
            }
            });
        }

        if (!p_open) {
          first_open = true;
        }

        // the bar above holds the tabs, the one chosen fills the rest. The channels are a panel of the main view
        switch (current_tab) {
          case 1: render_stream_view_content(); break;
          case 2: gui.render_debug_tab(); break;
          default: current_tab = 1; break;
        }
        }
        gui.render_password_popup();
      }
      gui::end();
      if (gui.ui_autohide && current_tab == 1) {
        gui.top_bar.render_autohide_bar(window_pos, window_size);
      }
      gui.pop_theme();
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_channel_row(const ecps_backend_t::channel_list_info_t& channel, f32_t width) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      bool is_selected = gui_ptr->selected_channel_id.i == channel.channel_id.i;
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
      bool right_clicked = gui::is_item_clicked(1);
      if (hovered && gui::is_mouse_double_clicked(0) && !joined) {
        join_channel(channel.channel_id);
      }
      if (hovered && !gui::is_popup_open("##channel_menu")) {
        gui::set_tooltip(joined ? "Click to open, right-click for more" : "Double click to join, right-click for more");
      }
      if (right_clicked) {
        gui::open_popup("##channel_menu");
      }
      render_channel_menu(channel);
      gui::pop_id();
      if (clicked) {
        gui_ptr->selected_channel_id = channel.channel_id;
        // a joined channel opens its own view, the users in it
        if (joined) {
          channels_browsing = false;
        }
        gui_ptr->backend_queue([channel_id = channel.channel_id]() -> fan::event::task_t {
          if (auto sessions = co_await ecps_expected(ecps_backend.request_channel_session_list(channel_id)); !sessions) {
            fan::print("Failed to request session list: " + sessions.error());
          }
        });
      }
      auto* list = gui::get_window_draw_list();
      if (is_selected || hovered) {
        list->AddRectFilled(at, at + fan::vec2(width, row_height), is_selected ? gui::widget_palette.accent_surface_hover : gui::widget_palette.surface, 6.f);
      }
      if (is_selected) {
        list->AddRectFilled(at + fan::vec2(0, 6.f), at + fan::vec2(3.f, row_height - 6.f), accent_hover_u32, 1.5f);
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
        // a lock says it without a word, and takes less room than a tag
        f32_t lock = gui::get_font_size() * 0.82f;
        if (after_name + lock + 10.f <= at.x + width - right_width - 12.f) {
          draw_icon(list, icon_e::lock, fan::vec2(after_name + lock * 0.5f, at.y + row_height * 0.5f), lock, 0xff40b8f0u);
          after_name += lock + 10.f;
        }
      }
      if (voice_chat.is_wanted(channel.channel_id)) {
        // voice and the microphone's state stay visible even with the member list closed
        bool muted = voice_chat.muted.load();
        f32_t mic = gui::get_font_size() * 0.8f;
        if (after_name + mic + 10.f <= at.x + width - right_width - 12.f) {
          draw_icon(list, muted ? icon_e::mic_muted : icon_e::mic, fan::vec2(after_name + mic * 0.5f, at.y + row_height * 0.5f),
            mic, muted ? 0xff8080ffu : 0xff90ff90u);
        }
      }
      list->AddText(fan::vec2(at.x + width - right_width - 10.f, text_y), 0xff909090u, right.c_str());
    
}

bool render_thread_t::ecps_gui_t::channel_list_window_t::render_back_button(f32_t height) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

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
      list->AddRectFilled(at, at + size, hovered ? gui::widget_palette.surface_hover : gui::widget_palette.surface, height * 0.3f);
      std::uint32_t ink = hovered ? 0xffffffffu : 0xffc8c8c8u;
      fan::vec2 c = at + fan::vec2(10.f + chevron * 0.5f, height * 0.5f);
      list->AddLine(c + fan::vec2(chevron * 0.5f, -chevron), c + fan::vec2(-chevron * 0.5f, 0), ink, 2.f);
      list->AddLine(c + fan::vec2(-chevron * 0.5f, 0), c + fan::vec2(chevron * 0.5f, chevron), ink, 2.f);
      list->AddText(at + fan::vec2(chevron * 2.f + 16.f, (height - gui::get_font_size()) * 0.5f), ink, text);
      return clicked;
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::leave_channel(ecps_backend_t::Protocol_ChannelID_t channel_id) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      // a channel joined by itself would be joined again at once, leaving it stops that
      for (const auto& channel : ecps_backend.available_channels) {
        if (channel.channel_id.i == channel_id.i && gui_ptr->auto_join_host == channel.name) {
          gui_ptr->set_auto_join(std::string());
        }
      }
      gui_ptr->backend_queue([channel_id]() -> fan::event::task_t {
        if (auto quit = co_await ecps_expected(ecps_backend.channel_quit(channel_id)); !quit) {
          fan::print("Failed to leave channel: " + quit.error());
          co_return;
        }
        (void)co_await ecps_expected(ecps_backend.request_channel_list());
      });
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::join_channel(ecps_backend_t::Protocol_ChannelID_t channel_id) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  gui_ptr->selected_channel_id = channel_id;
  gui_ptr->backend_queue([channel_id]() -> fan::event::task_t {
    if (auto joined = co_await ecps_expected(ecps_backend.channel_join(channel_id)); !joined) {
      fan::print("Failed to join channel: " + joined.error());
      co_return;
    }
    if (auto listed = co_await ecps_expected(ecps_backend.request_channel_list()); !listed) {
      co_return;
    }
    (void)co_await ecps_expected(ecps_backend.request_channel_session_list(channel_id));
  });
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_channel_menu(const ecps_backend_t::channel_list_info_t& channel) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  if (!gui::begin_popup("##channel_menu")) {
    return;
  }
  bool joined = ecps_backend.is_channel_joined(channel.channel_id);
  bool mine = ecps_backend.is_current_user_host_of_channel(channel.channel_id);
  gui::text_disabled(channel.name);
  gui::separator();
  if (joined) {
    if (gui::menu_item("Open")) {
      gui_ptr->selected_channel_id = channel.channel_id;
      channels_browsing = false;
    }
    if (gui::menu_item("Leave")) {
      leave_channel(channel.channel_id);
    }
  }
  else if (gui::menu_item("Join")) {
    join_channel(channel.channel_id);
  }
  if (!mine) {
    bool auto_join_on = gui_ptr->auto_join_host == channel.name;
    if (gui::menu_item("Join automatically", {}, auto_join_on)) {
      gui_ptr->set_auto_join(auto_join_on ? std::string() : channel.name);
    }
  }
  gui::end_popup();
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_joined_channel_view() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      f32_t button = gui::get_frame_height();
      if (render_back_button(button)) {
        channels_browsing = true;
      }
      // your own channel too: the server keeps it for the others, and you are its host again when you come back
      {
        bool own = ecps_backend.is_current_user_host_of_channel(gui_ptr->selected_channel_id);
        gui::same_line();
        gui::set_cursor_pos_x(gui::get_cursor_pos_x() + std::max(gui::get_content_region_avail().x - button, 0.f));
        if (icon_button("##leave_joined_channel", icon_e::leave,
          own ? "Leave your channel. It stays for the others, join it again to host" : "Leave this channel", button, icon_style_e::danger)) {
          leave_channel(gui_ptr->selected_channel_id);
          channels_browsing = true;
        }
      }
      gui::spacing();
      render_channel_details(gui::get_content_region_avail());
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_channels_content() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      fan::vec2 avail_size = gui::get_content_region_avail();
      if (avail_size.x <= 0 || avail_size.y <= 0) {
        return;
      }
      // a channel joined since the last frame (here, by itself or from elsewhere): its view
      if (ecps_backend.channel_info.size() > joined_seen) {
        gui_ptr->selected_channel_id = ecps_backend.channel_info.back().channel_id;
        channels_browsing = false;
      }
      joined_seen = ecps_backend.channel_info.size();
      bool has_selection = gui_ptr->selected_channel_id.i != (uint16_t)-1;
      bool already_in_selected = has_selection && ecps_backend.is_channel_joined(gui_ptr->selected_channel_id);
      if (already_in_selected && !channels_browsing) {
        render_joined_channel_view();
        return;
      }

      // ---- the top row: the search over the width, the buttons at its right
      f32_t button = gui::get_frame_height();
      int buttons = 2 + (has_selection ? 1 : 0);
      f32_t buttons_width = buttons * (button + gui::get_style().ItemSpacing.x);
      fan::vec2 search_at = gui::get_cursor_screen_pos();
      gui::push_item_width(std::max(avail_size.x - buttons_width, 80.f));
      gui::input_text("##channel_search", &search_filter);
      bool search_active = gui::is_item_active();
      gui::pop_item_width();
      if (search_filter.empty() && !search_active) {
        gui::get_window_draw_list()->AddText(search_at + fan::vec2(gui::get_style().FramePadding.x, gui::get_style().FramePadding.y), 0xff808080u, "Search channels");
      }
      if (has_selection) {
        gui::same_line();
        if (already_in_selected) {
          if (icon_button("##leave_channel", icon_e::leave, "Leave the selected channel", button, icon_style_e::danger)) {
            leave_channel(gui_ptr->selected_channel_id);
          }
        }
        else if (icon_button("##join_channel", icon_e::join, "Join the selected channel", button, icon_style_e::accent)) {
          join_channel(gui_ptr->selected_channel_id);
        }
      }
      gui::same_line();
      if (icon_button("##add_channel", icon_e::plus, "New channel", button)) {
        gui_ptr->new_channel_popup_requested = true;
      }
      gui::same_line();
      if (icon_button("##connect_server", icon_e::server, "Connect to a server", button)) {
        gui_ptr->drop_down_server.toggle_render_server_connect = true;
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
          if (!ecps_backend.connected) {
            gui::text_wrapped_disabled("Not connected to a server");
            if (gui::button("Connect to a server")) {
              gui_ptr->drop_down_server.toggle_render_server_connect = true;
            }
          }
          else if (ecps_backend.channel_list_received) {
            gui::text_wrapped_disabled("No channels yet");
            if (gui::button("Create a channel")) {
              gui_ptr->new_channel_popup_requested = true;
            }
          }
          else {
            gui::text_wrapped_disabled("Loading the channels...");
          }
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
          gui::set_tooltip("Drag to resize, double-click to fit");
        }
        if (gui::is_item_active()) {
          channels_list_height = list_height + gui::get_io().MouseDelta.y;
          channels_list_resizing = true;
        }
        else if (channels_list_resizing) {
          channels_list_resizing = false;
          gui_ptr->save_side_panel_layout();
        }
        if (hot && gui::is_mouse_double_clicked(0)) {
          channels_list_height = 0.f;
          gui_ptr->save_side_panel_layout();
        }
        fan::vec2 c = at + fan::vec2(avail_size.x * 0.5f, handle * 0.5f);
        gui::get_window_draw_list()->AddLine(fan::vec2(c.x - 24.f, c.y), fan::vec2(c.x + 24.f, c.y), hot ? accent_hover_u32 : 0x30ffffffu, 3.f);
      }

      render_channel_details(fan::vec2(avail_size.x, std::max(gui::get_content_region_avail().y, 60.f)));
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_channel_details(fan::vec2 size) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      bool has_selection = gui_ptr->selected_channel_id.i != (uint16_t)-1;
      bool already_in_selected = has_selection && ecps_backend.is_channel_joined(gui_ptr->selected_channel_id);
      bool is_host_of_selected = already_in_selected && ecps_backend.is_current_user_host_of_channel(gui_ptr->selected_channel_id);
      gui::begin_child("##channel_details_panel", size, true);

      gui::dummy(fan::vec2(6, 0));
      gui::same_line(0, 0);
      gui::begin_group();

        if (gui_ptr->selected_channel_id.i != (uint16_t)-1) {
          std::string channel_name = "Channel " + std::to_string(gui_ptr->selected_channel_id.i);
          for (const auto& channel : ecps_backend.available_channels) {
            if (channel.channel_id.i == gui_ptr->selected_channel_id.i) {
              channel_name = channel.name;
              break;
            }
          }
          {
            gui::font_scope_t title(gui::get_font_size() * 1.15f, gui::font::bold);
            gui::text(channel_name);
          }
          if (ecps_backend.is_hosting_channel(gui_ptr->selected_channel_id)) {
            gui::same_line(0, 10.f);
            gui::badge("yours", accent_alpha(accent_u32, 0x50), accent_hover_u32);
          }
          if (!ecps_backend.is_hosting_channel(gui_ptr->selected_channel_id) && !channel_name.empty()) {
            bool auto_join_on = gui_ptr->auto_join_host == channel_name;
            if (gui::settings_switch("Join automatically", &auto_join_on,
              "Joins this channel by itself whenever its host is online, also after a restart")) {
              gui_ptr->set_auto_join(auto_join_on ? channel_name : std::string());
            }
          }

          if (already_in_selected) {
            gui_ptr->render_voice_switch(gui_ptr->selected_channel_id);
          }
          gui::spacing();

          if (gui::begin_tab_bar("ChannelDetailTabs")) {
            if (gui::begin_tab_item("Users", nullptr, detail_tab == 0 ? gui::tab_item_flags_set_selected : 0)) {
              if (detail_tab == 0) detail_tab = -1;

              gui::spacing();

              // the list's own controls, out of the channel's title line
              {
                f32_t button = gui::get_frame_height() * 0.9f;
                f32_t row = gui::get_content_region_avail().x;
                gui::set_cursor_pos_x(gui::get_cursor_pos_x() + std::max(row - button * 2.f - 8.f, 0.f));
                if (icon_button("##toggle_stream_users", icon_e::users,
                  gui_ptr->show_stream_users ? "Hide the users over the stream" : "Show the users over the stream", button,
                  gui_ptr->show_stream_users ? icon_style_e::accent : icon_style_e::plain)) {
                  gui_ptr->show_stream_users = !gui_ptr->show_stream_users;
                }
                gui::same_line(0, 6.f);
                if (icon_button("##refresh_users", icon_e::refresh, "Refresh the users", button)) {
                  gui_ptr->backend_queue([channel_id = gui_ptr->selected_channel_id]() -> fan::event::task_t {
                    if (auto sessions = co_await ecps_expected(ecps_backend.request_channel_session_list(channel_id)); !sessions) {
                      fan::print("Failed to refresh session list: " + sessions.error());
                    }
                  });
                }
              }
              gui::spacing();

              if (gui::begin_child("SessionList", fan::vec2(0, 0), true)) {
                bool open_user_volume = false;
                auto it = ecps_backend.channel_sessions.find(gui_ptr->selected_channel_id.i);
                if (it != ecps_backend.channel_sessions.end()) {
                  auto sharing_now = ecps_backend.streamers_of(gui_ptr->selected_channel_id);
                  auto in_voice = ecps_backend.voice_members_of(gui_ptr->selected_channel_id);
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
                      voice_chat.set_user_volume((std::uint32_t)session.session_id.i, gui_ptr->user_volume(session.username));
                    }
                    f32_t hit_width = row_width - (has_buttons ? button_size + 12.f + (is_sharing ? button_size + 4.f : 0.f) : 0.f);
                    fan::vec2 hit_end = row_at + fan::vec2(std::max(hit_width, 0.f), row_height);
                    if (gui::is_mouse_hovering_rect(row_at, hit_end)) {
                      gui::get_window_draw_list()->AddRectFilled(row_at, hit_end, gui::widget_palette.surface, 6.f);
                      if (!is_me && !gui::is_any_item_hovered() && gui::is_window_hovered()) {
                        gui::set_tooltip("Right-click for volume, mute, watch and send a file");
                      }
                      if (!is_me && fan::window::is_mouse_clicked(1)) {
                        gui_ptr->volume_user_session = (std::uint32_t)session.session_id.i;
                        gui_ptr->volume_user_name = session.username;
                        gui_ptr->volume_user_value = gui_ptr->user_volume(session.username);
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
                      f32_t& shown = gui_ptr->voice_meter_levels[speaker];
                      f32_t dt = std::min(gui::get_io().DeltaTime, 0.1f);
                      shown = target > shown ? shown + (target - shown) * std::min(dt * 30.f, 1.f) : std::max(target, shown - dt * 2.5f);
                      gui::same_line(0, 8.f);
                      // a click mutes: yourself for the others, someone else just for you
                      // "mic off": they muted their own microphone, the server says so. "muted": you muted them
                      bool self_muted = is_me ? voice_chat.muted.load() : ecps_backend.voice_muted_by_self(gui_ptr->selected_channel_id, speaker);
                      bool locally_muted = !is_me && gui_ptr->user_volume(session.username) <= 0.f;
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
                          gui_ptr->toggle_mic_mute();
                        }
                        else {
                          gui_ptr->toggle_user_mute(speaker, session.username);
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
                        gui_ptr->send_file_dialog(gui_ptr->selected_channel_id, session.session_id);
                      }
                      if (is_sharing) {
                        gui::set_cursor_screen_pos(fan::vec2(x - button_size - 4.f, button_y));
                        if (icon_button("##watch", icon_e::eye, "Watch in Stream View", button_size)) {
                          gui_ptr->set_watch(gui_ptr->selected_channel_id, (std::uint32_t)session.session_id.i);
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
                  gui::text_disabled("Loading the users...");
                }
                if (open_user_volume) {
                  gui::open_popup("##user_volume");
                }
                gui_ptr->render_user_volume_popup();
              }
              gui::end_child();

              gui::end_tab_item();
            }

            if (is_host_of_selected && gui::begin_tab_item("Manage", nullptr, 0)) {
              gui::spacing();
              if (gui::begin_child("HostControls", fan::vec2(0, 0), gui::child_flags_always_use_window_padding)) {
                gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, 10.f));
                if (gui::settings_switch("Preview own stream", &gui_ptr->show_own_stream, "Shows your own shared screen here")) {
                  if (auto rt = get_render_thread(); rt) {
                    if (gui_ptr->show_own_stream) {
                      rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
                    }
                    else {
                      rt->network_frame.set_image(engine.default_texture);
                    }
                  }
                }
                gui_ptr->render_share_audio_checkbox();
                gui_ptr->render_channel_password_controls(gui_ptr->selected_channel_id);
                gui::dummy(fan::vec2(0, 8.f));
                gui_ptr->render_delete_channel_button(gui_ptr->selected_channel_id);
                gui::pop_style_var();
              }
              gui::end_child();
              gui::end_tab_item();
            }

            if (gui::begin_tab_item("MIDI", nullptr, 0)) {
              gui_ptr->render_midi_tab();
              gui::end_tab_item();
            }

            if (gui::begin_tab_item("Logs", nullptr, 0)) {
              gui_ptr->render_logs_tab();
              gui::end_tab_item();
            }

            gui::end_tab_bar();
          }
        }
        else {
          gui::spacing();
          gui::text_wrapped_disabled("Pick a channel above to see who is in it. Double click one to join it.");
        }

      gui::end_group();
      gui::end_child();
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_stream_settings_content_compact() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, settings_space(3.f)));
      gui::push_style_var(gui::style_var_frame_padding, fan::vec2(gui::get_style().FramePadding.x,
        std::max(gui::get_style().FramePadding.y, gui::get_font_size() * 0.22f)));
      gui::push_style_var(gui::style_var_frame_rounding, 5.f);
      bool streaming = ecps_backend.is_streaming_to_any_channel();
      bool viewing = ecps_backend.is_viewing_any_channel();
      auto* rt = get_render_thread();

      // ---- what you share
      settings_section_icon(icon_e::monitor, "SHARING");
      gui::indent(settings_space(2.f));
      gui_ptr->render_capture_source();
      if (streaming && rt) {
        gui_ptr->resolution_controls.render_resolution_controls(false);

        gui::settings_row("Framerate", [&] {
          gui::slider("##framerate_compact", &gui_ptr->stream_settings.framerate, 3.f, 244.f);
          if (gui::is_item_deactivated_after_edit()) {
            rt->screen_encoder.config_.frame_rate = gui_ptr->stream_settings.framerate;
            std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
            rt->screen_encoder.update_flags |= codec_update_e::frame_rate;
            // Automatic: the bitrate follows the framerate too, not only the resolution
            if (!gui_ptr->lossless && gui_ptr->stream_settings.bitrate_mode == 0) {
              rt->screen_encoder.config_.bitrate = dynamic_config_t::get_adaptive_bitrate();
              rt->screen_encoder.update_flags |= codec_update_e::rate_control;
            }
          }
        });

        gui_ptr->render_share_audio_checkbox();
      }
      gui::unindent(settings_space(2.f));

      // ---- what you watch
      if (viewing) {
        settings_section_icon(icon_e::speaker, "WATCHING");
        gui::indent(settings_space(2.f));
        gui_ptr->render_play_audio_controls();
        gui::unindent(settings_space(2.f));
      }

      // ---- on the graphics card or on the processor
      gui_ptr->render_video_choice();

      // ---- how it shows
      settings_section_icon(icon_e::eye, "DISPLAY");
      gui::indent(settings_space(2.f));
      gui::settings_switch("Stats over the picture", &gui_ptr->show_stream_stats,
        "Drag the box to move it");
      if (gui_ptr->show_stream_stats) {
        settings_hint("FPS and bitrate are drawn over the picture");
      }
      if (gui::settings_switch("Always show the bottom bar", &gui_ptr->keep_stream_toolbar,
        "Off: it hides unless the mouse is near the bottom")) {
        gui_ptr->save_ui_config();
      }
      bool remote_control = gui_ptr->stream_settings.input_control != 0;
      if (gui::settings_switch("Remote control", &remote_control,
        "Viewers can use your mouse and keyboard")) {
        gui_ptr->stream_settings.input_control = remote_control ? 1 : 0;
        gui_ptr->send_input_control_flag();
      }
      if (remote_control) {
        settings_warning("Viewers can use your mouse and keyboard");
      }
      gui::unindent(settings_space(2.f));

      // ---- the details, folded away
      gui::dummy(fan::vec2(0, 6.f));
      if (settings_fold("Advanced", &advanced_open)) {
        if (streaming && rt) {
          gui_ptr->resolution_controls.render_aspect_control();
          gui_ptr->render_hdr_light();
          if (gui::settings_switch("Lossless", &gui_ptr->lossless,
            "HEVC 4:4:4 at QP 0. Very fast link, 1440p or lower, HEVC 4:4:4 viewers")) {
            gui_ptr->apply_lossless();
            gui_ptr->save_ui_config();
          }
          if (gui_ptr->lossless) {
            settings_warning("Very high bitrate; 1440p or lower and a very fast link");
          }
          else {
            gui::settings_row("Bitrate", [&] {
              const char* bitrate_modes[] = { "Automatic", "Manual" };
              if (gui::combo("##bitrate_mode", &gui_ptr->stream_settings.bitrate_mode, bitrate_modes, 2)) {
                gui_ptr->stream_settings.use_adaptive_bitrate = gui_ptr->stream_settings.bitrate_mode == 0;
              }
            });
            if (gui_ptr->stream_settings.bitrate_mode == 1) {
              gui::settings_row("Mbit/s", [&] {
                if (gui::slider("##bitrate_compact", &gui_ptr->stream_settings.bitrate_mbps, 0.5f, 50.0f, gui::slider_flags_always_clamp)) {
                  rt->screen_encoder.config_.bitrate = gui_ptr->stream_settings.bitrate_mbps * 1000000;
                  std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
                  rt->screen_encoder.update_flags |= codec_update_e::rate_control;
                }
              });
            }
          }
          render_encoder_row(rt);
          render_scaling_row(rt);
        }
        if (viewing) {
          render_decoder_row();
        }
        if (streaming) {
          if (gui::button("Reset quality settings")) {
            gui_ptr->stream_settings.selected_resolution = 0;
            gui_ptr->stream_settings.framerate = 30;
            gui_ptr->stream_settings.bitrate_mbps = 5;
            gui_ptr->stream_settings.input_control = 0;
            gui_ptr->send_input_control_flag();
            if (gui_ptr->lossless) {
              gui_ptr->lossless = false;
              gui_ptr->apply_lossless();
              gui_ptr->save_ui_config();
            }
          }
          if (gui::is_item_hovered()) {
            gui::set_tooltip("30 fps, 5 Mbit/s, remote control off");
          }
        }
      }
      if (settings_fold("Network", &network_open)) {
        render_network_details();
      }
      gui::pop_style_var(3);
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_encoder_row(render_thread_t* rt) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

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

      // the encoder in use is the one shown: the Codec choice (VIDEO) changes it too, and a saved pick from before
      // stayed in this list while another one encoded
      {
        std::string in_use = rt->screen_encoder.encoder_.get_codec_name();
        for (int i = 0; i < (int)encoder_names.size(); ++i) {
          if (encoder_names[i] == in_use) {
            gui_ptr->stream_settings.selected_encoder = i;
          }
        }
      }
      gui::settings_row("Encoder", [&] {
        if (!gui::combo("##encoder_compact", &gui_ptr->stream_settings.selected_encoder, encoder_options.data(), encoder_options.size())) {
          return;
        }
        std::string encoder_name = encoder_names[gui_ptr->stream_settings.selected_encoder];
        codec_config_t::codec_type_e codec_type = codec_config_t::H264;
        if (encoder_name.find("265") != std::string::npos || encoder_name.find("hevc") != std::string::npos) {
          codec_type = codec_config_t::H265;
        }
        else if (encoder_name.find("av1") != std::string::npos) {
          codec_type = codec_config_t::AV1;
        }
        rt->screen_encoder.config_.codec = codec_type;
        // the Codec choice in VIDEO follows, and is saved
        gui_ptr->video_codec = (int)codec_type;
        gui_ptr->save_ui_config();
        rt->screen_encoder.new_codec = gui_ptr->stream_settings.selected_encoder;
        rt->screen_encoder.update_flags |= codec_update_e::codec;
        rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
      });
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_scaling_row(render_thread_t* rt) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      static auto quality_options = []() {
        std::vector<const char*> options;
        for (int i = 0; i < 5; ++i) {
          options.push_back(fan::graphics::get_scaling_quality_name(static_cast<codec_config_t::scaling_quality_e>(i)));
        }
        return options;
      }();
      gui::settings_row("Scaling", [&] {
        if (gui::combo("##scaling_quality", &gui_ptr->stream_settings.selected_scaling_quality, quality_options.data(), quality_options.size())) {
          rt->screen_encoder.update_scaling_quality(static_cast<codec_config_t::scaling_quality_e>(gui_ptr->stream_settings.selected_scaling_quality));
        }
      });
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_decoder_row() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

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
              gui_ptr->stream_settings.selected_decoder = i;
              break;
            }
          }
          if (gui_ptr->stream_settings.selected_decoder >= decoder_names.size()) {
            for (int i = 0; i < decoder_names.size(); i++) {
              if (decoder_names[i] == "auto-detect") {
                gui_ptr->stream_settings.selected_decoder = i;
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
        if (!gui::combo("##decoder_compact", &gui_ptr->stream_settings.selected_decoder, decoder_options.data(), decoder_options.size())) {
          return;
        }
        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) {
          return;
        }
        std::string decoder_name = decoder_names[gui_ptr->stream_settings.selected_decoder];
        std::string codec_type = "h264";
        if (decoder_name.find("hevc") != std::string::npos) {
          codec_type = "hevc";
        }
        else if (decoder_name.find("av1") != std::string::npos) {
          codec_type = "av1";
        }
        rt->screen_decoder.new_codec = gui_ptr->stream_settings.selected_decoder;
        rt->screen_decoder.update_flags |= codec_update_e::codec;
        rt->screen_decoder.name = codec_type;
        gui_ptr->request_idr_reset();
      });
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_network_details() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

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

void render_thread_t::ecps_gui_t::channel_list_window_t::process_stream_settings() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      gui_ptr->load_side_panel_layout();
      fan::vec2 avail = gui::get_content_region_avail();
      f32_t left_padding = 8.f;
      f32_t right_padding = 12.f;
      f32_t handle = 14.f;
      fan::vec2 origin = gui::get_cursor_screen_pos() + fan::vec2(left_padding, 0);
      fan::vec2 area(avail.x - left_padding - right_padding, avail.y - gui::get_style().ItemSpacing.y);
      if (area.x < 100.f || area.y < 100.f) {
        return;
      }
      gui_ptr->side_view_min = origin;
      gui_ptr->side_view_max = origin + area;
      bool right = !gui_ptr->panels_on(0).empty();
      bool bottom = !gui_ptr->panels_on(1).empty();
      bool left = !gui_ptr->panels_on(2).empty();
      // a side pushed against its edge: a grip there brings it back
      f32_t grip = gui_ptr->side_grip_size;
      bool right_grip = gui_ptr->side_hidden[0] && !gui_ptr->panels_docked(0).empty();
      bool bottom_grip = gui_ptr->side_hidden[1] && !gui_ptr->panels_docked(1).empty();
      bool left_grip = gui_ptr->side_hidden[2] && !gui_ptr->panels_docked(2).empty();
      f32_t height = bottom ? std::clamp(gui_ptr->side_panel_height, 150.f, area.y * 0.7f) : 0.f;
      f32_t top_height = area.y - (bottom ? height + handle : bottom_grip ? grip : 0.f);
      // the left one starts wide, the channels in it want room
      if (gui_ptr->side_left_width <= 0.f) {
        gui_ptr->side_left_width = std::max(area.x * 0.42f, 420.f);
      }
      f32_t share = left && right ? 0.4f : 0.7f;
      f32_t right_width = std::clamp(gui_ptr->stream_settings.settings_panel_width, 250.f, std::max(250.f, area.x * share));
      f32_t left_width = std::clamp(gui_ptr->side_left_width, 250.f, std::max(250.f, area.x * share));
      auto panel = [&](int side, fan::vec2 at, fan::vec2 size) {
        gui::set_cursor_screen_pos(at);
        gui::begin_child(("##side_panel_" + std::to_string(side)).c_str(), size, true);
        gui_ptr->render_side_panel_content(side);
        gui::end_child();
      };
      f32_t x = origin.x;
      if (left) {
        panel(2, origin, fan::vec2(left_width, top_height));
        gui::set_cursor_screen_pos(fan::vec2(origin.x + left_width, origin.y));
        gui_ptr->render_side_panel_handle(fan::vec2(handle, top_height), 2);
        x += left_width + handle;
      }
      else if (left_grip) {
        gui::set_cursor_screen_pos(origin);
        gui_ptr->render_side_grip(fan::vec2(grip, top_height), 2);
        x += grip;
      }
      f32_t stream_width = area.x - (x - origin.x) - (right ? right_width + handle : right_grip ? grip : 0.f);
      gui::set_cursor_screen_pos(fan::vec2(x, origin.y));
      render_stream_area("##stream_display_split", fan::vec2(stream_width, top_height));
      if (right) {
        gui::set_cursor_screen_pos(fan::vec2(x + stream_width, origin.y));
        gui_ptr->render_side_panel_handle(fan::vec2(handle, top_height), 0);
        panel(0, fan::vec2(x + stream_width + handle, origin.y), fan::vec2(right_width, top_height));
      }
      else if (right_grip) {
        gui::set_cursor_screen_pos(fan::vec2(x + stream_width, origin.y));
        gui_ptr->render_side_grip(fan::vec2(grip, top_height), 0);
      }
      if (bottom) {
        gui::set_cursor_screen_pos(fan::vec2(origin.x, origin.y + top_height));
        gui_ptr->render_side_panel_handle(fan::vec2(area.x, handle), 1);
        panel(1, fan::vec2(origin.x, origin.y + top_height + handle), fan::vec2(area.x, height));
      }
      else if (bottom_grip) {
        gui::set_cursor_screen_pos(fan::vec2(origin.x, origin.y + top_height));
        gui_ptr->render_side_grip(fan::vec2(area.x, grip), 1);
      }
      gui_ptr->render_side_panel_drop();
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::process_no_stream_settings() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

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

void render_thread_t::ecps_gui_t::channel_list_window_t::render_stream_area(const char* id, fan::vec2 size) {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      gui::set_next_window_bg_alpha(0);
      gui::begin_child(id, size, false);
      // the picture and its bars stay black; only what is outside the stream area follows the theme backdrop
      {
        auto* list = gui::get_window_draw_list();
        fan::vec2 black_min = gui::get_window_pos();
        list->AddRectFilled(black_min, black_min + gui::get_window_size(), 0xff000000u);
      }
      gui::set_viewport(engine.orthographic_render_view);
      gui_ptr->stream_area_min = gui::get_window_pos();
      gui_ptr->stream_area_max = gui_ptr->stream_area_min + size;
      gui_ptr->stream_area_shown = true;

      if (auto rt = get_render_thread(); rt) {
        fan::vec2 stream_area = gui::get_content_region_avail();
        gui_ptr->network_frame_helper.setup_network_frame(stream_area);
        gui_ptr->render_fps_counter(size.x);
        gui_ptr->draw_host_cursor();

        if (rt->network_frame.get_image() == engine.default_texture) {
          gui_ptr->render_stream_empty_hint(stream_area);
        }
        else {
          // the place setup_network_frame gave it (pixel for pixel near its own size, on whole pixels)
          fan::vec2 half = gui_ptr->network_frame_helper.shown_half;
          fan::vec2 center = gui::get_window_pos() + gui_ptr->network_frame_helper.shown_position;
          gui_ptr->picture_min = center - half;
          gui_ptr->picture_max = center + half;
          gui_ptr->picture_shown = true;
          // frames stopped coming for a moment: the picture stays, a note at its top says it is waiting
          if (gui_ptr->picture_stalled) {
            const char* note = "Connection is slow, waiting for the stream...";
            fan::vec2 text = gui::calc_text_size(note);
            fan::vec2 at(std::max(center.x - half.x, gui::get_window_pos().x) + 0.f, std::max(center.y - half.y, gui::get_window_pos().y) + 12.f);
            at.x = center.x - text.x * 0.5f;
            auto* list = gui::get_window_draw_list();
            list->AddRectFilled(at - fan::vec2(12.f, 6.f), at + text + fan::vec2(12.f, 6.f), 0xc0000000u, (text.y + 12.f) * 0.5f);
            list->AddText(at, 0xffd0d0d0u, note);
          }
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

      gui_ptr->render_stream_toolbar();
      gui::end_child();
    
}

void render_thread_t::ecps_gui_t::channel_list_window_t::render_stream_view_content() {
  auto* gui_ptr = &*OFFSETLESS(this, ecps_gui_t, window_handler);
  auto& engine = gui_ptr->owner->engine;

      gui::spacing();
      gui::spacing();

      fan::vec2 avail_size = gui::get_content_region_avail();

      // the toolbar floats over the picture now, the stream gets the whole height
      f32_t content_height = avail_size.y - gui::get_style().ItemSpacing.y;

      if (avail_size.x > 0 && content_height > 0) {
        if (gui_ptr->any_side_panel_open()) {
          process_stream_settings();
        }
        else {
          process_no_stream_settings();
        }
      }

      gui::spacing();
    
}

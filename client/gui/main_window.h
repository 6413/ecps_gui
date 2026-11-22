// Part of ecps_gui_t, included inside the struct by gui.h: the main window: the channels tab, the stream view and its settings panel.

  struct channel_list_window_t {
#undef This
#define This OFFSETLESS(this, ecps_gui_t, window_handler)

    void render();


    // the channels panel, made for a side bar: the buttons and the search on top, the channels as rows, the selected
    // channel's details under them at the full width. The line between the list and the details moves
    f32_t channels_list_height = 0.f; // 0: as tall as the rows need
    bool channels_list_resizing = false;

    static std::string channel_live_time(std::uint64_t ns) {
      std::uint64_t s = ns / 1000000000ull;
      return s >= 3600 ? fan::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60) : fan::format("{}:{:02}", s / 60, s % 60);
    }

    void render_channel_row(const ecps_backend_t::channel_list_info_t& channel, f32_t width);


    // the server's channels (true) or the joined channel's own view (false). Joining a channel shows its view, the
    // back button in it the channels again
    bool channels_browsing = false;
    std::size_t joined_seen = 0;

    // "< Channels": back to the server's channels from a joined channel's view
    bool render_back_button(f32_t height);


    void leave_channel(ecps_backend_t::Protocol_ChannelID_t channel_id);


    // a joined channel: the way back to the channels, leaving, and the channel's users and tabs over all the panel
    void render_joined_channel_view();


    void render_channels_content();


    // the selected channel: its name, the users in it and its tabs
    void render_channel_details(fan::vec2 size);


    // The panel beside the stream. Sections for what you share, what you watch and how it shows, then the details
    // folded away. Each setting is one row, its name at the left and its control at the right, and what is on or off
    // is a switch
    void render_stream_settings_content_compact();

    bool advanced_open = false;
    bool network_open = false;

    void render_encoder_row(render_thread_t* rt);


    void render_scaling_row(render_thread_t* rt);


    void render_decoder_row();


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

    void render_network_details();


    // The stream and its panels: Channels, Settings and Chat each on the right, at the bottom or on the left (more
    // on one side share a panel with a tab each). The lines between them resize them
    void process_stream_settings();


    void process_no_stream_settings();


    // the picture, the pointer over it, the text when there is none, and the toolbar
    void render_stream_area(const char* id, fan::vec2 size);


    void render_stream_view_content();


    int main_tab = -1;
    int detail_tab = 0;
    int current_tab = 1; // 1 the main view, 2 Debug
    bool p_open = true;
    bool auto_refresh = true;
    int refresh_interval = 1;
    std::string search_filter;
  }window_handler;

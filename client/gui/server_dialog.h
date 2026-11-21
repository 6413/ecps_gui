// Part of ecps_gui_t, included inside the struct by gui.h: the server connect dialog and the stream settings the panel edits.

  struct drop_down_server_t {
#undef This
#define This OFFSETLESS(this, ecps_gui_t, drop_down_server)

    void render() {

      bool contains_server = This->config.contains("server");
      static std::string ip = contains_server && This->config["server"].contains("ip") ? This->config["server"]["ip"] : "";
      static std::string port = contains_server && This->config["server"].contains("port") ? This->config["server"]["port"] : "";

      fan::json server_json = This->config.contains("server") ? This->config["server"] : fan::json();

      gui::push_style_var(gui::style_var_window_padding, fan::vec2(13.0000, 20.0000));
      gui::push_style_var(gui::style_var_item_spacing, fan::vec2(14, 16));
      if (toggle_render_server_connect) {
        gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
        gui::open_popup("server_connect");
      }

      if (gui::begin_popup("server_connect")) {

        gui::push_item_width(300);

        popup_size = gui::get_window_size();

        bool enter_pressed = gui::input_text("ip", &ip, gui::input_text_flags_enter_returns_true);
        enter_pressed |= gui::input_text("port", &port, gui::input_text_flags_enter_returns_true);

        if (gui::button("Connect") || enter_pressed) {
          {
            if (ip.size()) {
              server_json["ip"] = ip;
            }
            if (port.size()) {
              server_json["port"] = port;
            }
            if (ip.size() && port.size()) {
              ecps_backend.ip = ip;
              ecps_backend.port = std::stoul(port);
              This->backend_queue([=]() -> fan::event::task_t {
                try {
                  bool did_connect = co_await ecps_backend.connect(ip, string_to_number(port));
                  if (did_connect) {
                    co_await ecps_backend.login();
                    co_await ecps_backend.request_channel_list();
                  }
                }
                catch (...) {
                }
                });
            }
          }

          This->write_to_config("server", server_json);
          gui::close_current_popup();
          toggle_render_server_connect = false;
        }
        gui::same_line();
        if (gui::button("Cancel")) {
          gui::close_current_popup();
          toggle_render_server_connect = false;
        }
        gui::pop_item_width();
        gui::end_popup();
      }

      gui::pop_style_var(2);
    }

    bool toggle_render_server_connect = false;
  }drop_down_server;

  struct stream_settings_t {
    int selected_scaling_quality = 1;
    int selected_resolution = 0;
    int framerate = 30;
    int selected_encoder = 0;
    int selected_decoder = 0;
    int input_control = 0;
    bool show_in_stream_view = false;
    bool share_audio = false;
    bool play_audio = true;
    f32_t audio_volume = 1.f;

    std::string voice_device;
    f32_t voice_gain = 1.f;
    f32_t voice_threshold = 0.02f;
    f32_t voice_volume = 1.f;
    // chat message, join and leave sounds, 0 is off
    f32_t notify_volume = 1.f;
    std::string voice_output;
    bool voice_echo_cancel = false;
    bool voice_noise_suppress = true;
    // evens out how loud each of the others is, so a quiet one and a loud one come to about the same level
    bool voice_normalize = false;

    bool share_camera = false;
    std::string camera_device;

    struct audio_device_settings_t {
      f32_t gain = 1.f;
      bool exclude_discord = true;
    };
    // loopback device to record, empty follows the default output
    std::string audio_device;
    // remembered per device id
    std::map<std::string, audio_device_settings_t> audio_devices;

    f32_t bitrate_mbps = 10;
    bool use_adaptive_bitrate = true;
    int bitrate_mode = 0;

    f32_t settings_panel_width = 350.0f;
    bool is_resizing = false;

    bool p_open = false;
  }stream_settings;

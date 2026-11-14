// Part of ecps_gui_t, included inside the struct by gui.h: mouse and keyboard to the host.

  void send_host_key(int key, bool state) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostKeyboard_t rest;
    rest.Scancode = fan::window::input::convert_fan_to_scancode(key);
    rest.State = state;
    queue_host_input(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostKeyboard, rest);
  }

  // the host's mouse and keyboard can only be driven while the host's own screen is the one watched
  bool watching_host_of(ecps_backend_t::Protocol_ChannelID_t channel_id) const {
    if (!watch_active || watch_channel != (std::uint32_t)channel_id.i) {
      return true;
    }
    for (const auto& channel : ecps_backend.available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        return (std::uint32_t)channel.host_session_id.i == watch_source;
      }
    }
    return false;
  }

  bool can_control_host() {
    return std::any_of(ecps_backend.channel_info.begin(), ecps_backend.channel_info.end(), [this](const auto& channel) {
      return channel.is_viewing && watching_host_of(channel.channel_id) &&
        (channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl);
    });
  }

  // nothing the viewer holds down may stay down on the host
  void release_host_input() {
    for (int key : input_forward.held_keys) {
      send_host_key(key, false);
    }
    for (std::uint8_t button : input_forward.held_buttons) {
      send_host_button(button, false, fan::vec2si(-1));
    }
    input_forward = {};
  }

  // clicking the stream gives it the mouse and keyboard, clicking elsewhere takes them back
  void forward_input() {
    auto rt = get_render_thread();
    bool stream_shown = is_fullscreen_stream || window_handler.current_tab == 1;
    if (!rt || !stream_shown || !can_control_host() || rt->network_frame.get_image() == engine.default_texture) {
      release_host_input();
      return;
    }

    fan::vec2 mouse_position = engine.get_mouse_position();
    fan::vec2 frame_size = rt->network_frame.get_size() * 2;
    fan::vec2 frame_min = fan::vec2(rt->network_frame.get_position()) - frame_size / 2;
    fan::vec2 frame_max = frame_min + frame_size;
    fan::vec2ui stream_size = rt->screen_decoder.decoded_size;

    bool over_frame = frame_size.x > 0 && frame_size.y > 0 && stream_size.x > 0 && stream_size.y > 0 &&
      mouse_position.x >= frame_min.x && mouse_position.x <= frame_max.x &&
      mouse_position.y >= frame_min.y && mouse_position.y <= frame_max.y &&
      !gui::is_any_item_hovered();

    fan::vec2si position(-1);
    if (over_frame) {
      position.x = std::min((int)((mouse_position.x - frame_min.x) / frame_size.x * stream_size.x), (int)stream_size.x - 1);
      position.y = std::min((int)((mouse_position.y - frame_min.y) / frame_size.y * stream_size.y), (int)stream_size.y - 1);
    }

    // the order the host's mouse api numbers them
    static constexpr int buttons[] = {fan::mouse_left, fan::mouse_right, fan::mouse_middle, fan::mouse_scroll_up, fan::mouse_scroll_down};
    static constexpr std::uint8_t first_scroll_button = 3;

    for (std::uint8_t button = 0; button < first_scroll_button; ++button) {
      auto& held = input_forward.held_buttons;
      auto it = std::find(held.begin(), held.end(), button);
      if (it != held.end() && engine.is_mouse_released(buttons[button])) {
        held.erase(it);
        send_host_button(button, false, position);
      }
      else if (engine.is_mouse_clicked(buttons[button])) {
        if (!over_frame) {
          release_host_input();
          return;
        }
        input_forward.captured = true;
        held.push_back(button);
        send_host_button(button, true, position);
      }
    }

    if (!input_forward.captured) {
      return;
    }

    if (over_frame) {
      if (position != input_forward.last_position) {
        input_forward.last_position = position;
        send_host_position(position);
      }
      for (std::uint8_t button = first_scroll_button; button < std::size(buttons); ++button) {
        if (engine.is_mouse_clicked(buttons[button])) {
          send_host_button(button, true, position);
        }
      }
    }

    auto& held_keys = input_forward.held_keys;
    for (auto it = held_keys.begin(); it != held_keys.end();) {
      if (engine.is_key_released(*it)) {
        send_host_key(*it, false);
        it = held_keys.erase(it);
      }
      else {
        ++it;
      }
    }

    // typing into our own text fields stays here
    if (!gui::get_io().WantTextInput) {
      for (int key = fan::key_first; key <= fan::key_last; ++key) {
        if (engine.is_key_clicked(key) && fan::window::input::convert_fan_to_scancode(key) != fan::input::key_invalid) {
          held_keys.push_back(key);
          send_host_key(key, true);
        }
      }
    }
  }

  void update_input_control() {
    // every stream starts with control off, the host opts in each time
    if (!ecps_backend.is_streaming_to_any_channel() && stream_settings.input_control != 0) {
      stream_settings.input_control = 0;
      send_input_control_flag();
    }
    forward_input();
    update_channel_flags();
    update_host_cursor();
    update_username();
    update_stream_settings_persistence();
    update_selected_channel();
    update_stream_state();
    update_watch();
    update_auto_show();
    update_debug_sample();
    update_auto_join();
    update_midi_forward();
    remote_logs.tick([this](const std::function<fan::event::task_t()>& task) {
      backend_queue(task);
    });
  }

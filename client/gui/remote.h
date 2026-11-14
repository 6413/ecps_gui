// Part of ecps_gui_t, included inside the struct by gui.h: what goes to the host: pointer, buttons, MIDI, and the piano.

  void queue_host_input(int command, auto rest) {
    backend_queue([command, rest]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          if (channel.is_viewing) {
            auto packet = rest;
            packet.ChannelID = channel.channel_id;
            packet.ChannelSessionID = channel.session_id;
            co_await ecps_backend.tcp_write(command, &packet, sizeof(packet));
            break;
          }
        }
      }
      catch (...) {}
    });
  }

  void send_host_position(const fan::vec2si& position) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseCoordinate_t rest;
    rest.pos = position;
    queue_host_input(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseCoordinate, rest);
  }

  void send_host_button(std::uint8_t button, bool state, const fan::vec2si& position) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseButton_t rest;
    rest.key = button;
    rest.state = state;
    rest.pos = position;
    queue_host_input(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseButton, rest);
  }

  void send_host_midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMidi_t rest;
    rest.Status = status;
    rest.Data1 = data1;
    rest.Data2 = data2;
    backend_queue([rest]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          // any channel we are in, but don't host, whose host lets MIDI in. No stream needed
          if (!ecps_backend.is_hosting_channel(channel.channel_id) &&
            (channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl)) {
            auto packet = rest;
            packet.ChannelID = channel.channel_id;
            packet.ChannelSessionID = channel.session_id;
            co_await ecps_backend.tcp_write(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMidi, &packet, sizeof(packet));
          }
        }
      }
      catch (...) {}
    });
  }

  // the keys of our own keyboard, for the piano in the MIDI tab
  void track_local_note(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    std::uint8_t type = status & 0xF0;
    if (data1 >= local_down.size() || (status & 0x0F) == 9) {
      return;
    }
    if (type == 0x90 && data2 != 0) {
      local_down[data1] = true;
    }
    else if (type == 0x80 || type == 0x90) {
      local_down[data1] = false;
    }
  }

  static constexpr std::uint32_t midi_col(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
  }

  // 88 keys A0..C8: ours light up blue, the ones playing here from the other side orange
  void render_midi_piano() {
    static constexpr int first_note = 21;
    static constexpr int last_note = 108;
    static constexpr int white_keys = 52;
    auto is_black = [](int note) {
      int n = note % 12;
      return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    };
    fan::vec2 avail = gui::get_content_region_avail();
    f32_t width = std::max(avail.x - 4.f, 120.f);
    f32_t white_width = width / white_keys;
    f32_t height = std::clamp(white_width * 6.f, 50.f, 130.f);
    fan::vec2 origin = gui::get_cursor_screen_pos();
    gui::dummy(fan::vec2(width, height + 4.f));

    auto* list = gui::get_window_draw_list();
    auto color_of = [&](int note, bool black) -> std::uint32_t {
      bool mine = local_down[note].load(std::memory_order_relaxed);
      bool theirs = midi_host.synth.is_down((std::uint8_t)note);
      if (mine) {
        return midi_col(70, 140, 255, 255);
      }
      if (theirs) {
        return midi_col(255, 150, 40, 255);
      }
      return black ? midi_col(25, 25, 25, 255) : midi_col(235, 235, 235, 255);
    };

    int white_index = 0;
    for (int note = first_note; note <= last_note; ++note) {
      if (is_black(note)) {
        continue;
      }
      fan::vec2 min(origin.x + white_index * white_width, origin.y);
      fan::vec2 max(min.x + white_width - 1.f, origin.y + height);
      list->AddRectFilled(min, max, color_of(note, false));
      ++white_index;
    }
    white_index = 0;
    for (int note = first_note; note <= last_note; ++note) {
      if (!is_black(note)) {
        ++white_index;
        continue;
      }
      // a black key sits on the seam after the white key before it
      f32_t center = origin.x + white_index * white_width;
      fan::vec2 min(center - white_width * 0.3f, origin.y);
      fan::vec2 max(center + white_width * 0.3f, origin.y + height * 0.62f);
      list->AddRectFilled(min, max, color_of(note, true));
    }
  }

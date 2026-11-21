// Part of ecps_gui_t, included inside the struct by gui.h: the logs and MIDI tabs.

  // ---- log sharing on or off, per server ------------------------------------------------------------------------
  // config.json "log_servers": {"ip:port": true}. Set again on each connect, off for a server it was never turned on for
  void save_server_logs(bool allow) {
    if (ecps_backend.ip.empty()) {
      return;
    }
    fan::json servers = config.contains("log_servers") && config["log_servers"].is_object() ? config["log_servers"] : fan::json::object();
    servers[server_voice_key()] = allow;
    write_to_config("log_servers", servers);
  }

  // from the update loop: the first time a server is connected (and after switching servers), its saved choice
  void update_server_logs() {
    if (!ecps_backend.connected || ecps_backend.ip.empty()) {
      return;
    }
    std::string key = server_voice_key();
    if (key == logs_applied_server) {
      return;
    }
    logs_applied_server = key;
    bool allow = false;
    if (config.contains("log_servers") && config["log_servers"].is_object() && config["log_servers"].contains(key) &&
      config["log_servers"][key].is_boolean()) {
      allow = config["log_servers"][key].get<bool>();
    }
    remote_logs.allow = allow;
  }
  std::string logs_applied_server;

  void render_logs_tab() {
    gui::spacing();
    bool allow = remote_logs.allow.load();
    if (gui::checkbox("Let others in this channel read my logs", &allow)) {
      remote_logs.allow = allow;
      save_server_logs(allow);
    }
    gui::text_wrapped_disabled("They see everything this program prints. It passes through the server.");
    gui::separator();

    gui::text("This computer");
    gui::same_line();
    if (remote_logs.local.load()) {
      if (gui::button("Stop##local_logs")) {
        remote_logs.stop_local();
      }
    }
    else if (gui::button("Read logs##local_logs")) {
      remote_logs.start_local([this](const std::function<fan::event::task_t()>& task) { backend_queue(task); });
    }

    bool in_channel = selected_channel_id.i != (uint16_t)-1;
    auto it = ecps_backend.channel_sessions.find(selected_channel_id.i);
    bool any_peer = false;
    if (in_channel && it != ecps_backend.channel_sessions.end()) {
      for (const auto& session : it->second) {
        if (session.session_id.i == ecps_backend.session_id.i) {
          continue;
        }
        any_peer = true;
        bool is_watched = remote_logs.watching.load() &&
          remote_logs.watch_peer.i == session.session_id.i && remote_logs.watch_channel.i == selected_channel_id.i;
        gui::push_id((int)session.session_id.i);
        gui::text(session.username + (session.is_host ? " (Host)" : "") + "  ID: " + std::to_string(session.session_id.i));
        gui::same_line();
        auto queue = [this](const std::function<fan::event::task_t()>& task) { backend_queue(task); };
        if (is_watched) {
          if (gui::button("Stop")) {
            remote_logs.stop_watch(queue);
          }
        }
        else if (gui::button("Read logs")) {
          remote_logs.start_watch(queue, selected_channel_id, session.session_id);
        }
        gui::pop_id();
      }
    }
    if (!any_peer) {
      gui::text_wrapped_disabled("Nobody else in this channel");
    }

    const char* status = "";
    switch (remote_logs.state.load()) {
      case remote_logs_t::state_e::asking: status = "Asking for access..."; break;
      case remote_logs_t::state_e::live: status = "Live"; break;
      case remote_logs_t::state_e::denied: status = "Denied: that user has not allowed log sharing"; break;
      case remote_logs_t::state_e::stopped: status = "Stopped by the other side"; break;
      default: break;
    }
    if (*status) {
      gui::text_wrapped(status);
    }

    gui::separator();
    gui::checkbox("stdout", &logs_show_out);
    gui::same_line();
    gui::checkbox("stderr", &logs_show_err);
    gui::same_line();
    gui::checkbox("Follow", &logs_follow);
    if (gui::button("Copy all")) {
      engine.window.set_clipboard(remote_logs.all_text(logs_show_out, logs_show_err));
    }
    gui::same_line();
    if (gui::button("Clear")) {
      remote_logs.clear_lines();
    }
    gui::text_wrapped_disabled("Click a line to copy it");

    // the whole history stays in memory, only the rows on screen are drawn
    std::uint64_t revision = remote_logs.current_revision();
    if (gui::begin_child("##remote_log", fan::vec2(0, 0), true, gui::window_flags_horizontal_scrollbar)) {
      if (gui::is_window_hovered() && gui::get_io().MouseWheel > 0.f) {
        logs_follow = false; // scrolling up means reading, don't pull it back down
      }
      f32_t pitch = gui::get_text_line_height_with_spacing();
      f32_t gap = pitch - gui::get_text_line_height();
      f32_t view_height = gui::get_window_size().y;
      f32_t scroll = gui::get_scroll_y();
      remote_logs.visit(logs_show_out, logs_show_err, [&](std::size_t count, auto line_at) {
        std::size_t first = std::min<std::size_t>(count, (std::size_t)std::max(0.f, scroll / pitch));
        std::size_t last = std::min<std::size_t>(count, first + (std::size_t)(view_height / pitch) + 3);
        if (first > 0) {
          gui::dummy(fan::vec2(0, first * pitch - gap));
        }
        for (std::size_t i = first; i < last; ++i) {
          const auto& line = line_at(i);
          bool is_err = line.stream == 2;
          if (is_err) {
            gui::push_style_color(gui::col_text, fan::vec4(1.f, 0.45f, 0.4f, 1.f));
          }
          gui::push_id((int)i);
          if (gui::selectable(line.text.empty() ? " " : line.text)) {
            engine.window.set_clipboard(line.text);
          }
          gui::pop_id();
          if (is_err) {
            gui::pop_style_color();
          }
        }
        if (last < count) {
          gui::dummy(fan::vec2(0, (count - last) * pitch - gap));
        }
        if (logs_follow && revision != logs_seen_revision) {
          gui::set_scroll_y(count * pitch);
        }
      });
      logs_seen_revision = revision;
    }
    gui::end_child();
  }

  // any channel we are in but don't host
  bool is_in_other_channel() const {
    return std::any_of(ecps_backend.channel_info.begin(), ecps_backend.channel_info.end(), [](const auto& channel) {
      return !ecps_backend.is_hosting_channel(channel.channel_id);
    });
  }

  // a host we are connected to lets MIDI in, no stream needed
  bool can_midi_host() const {
    return std::any_of(ecps_backend.channel_info.begin(), ecps_backend.channel_info.end(), [](const auto& channel) {
      return !ecps_backend.is_hosting_channel(channel.channel_id) &&
        (channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl);
    });
  }

  void render_midi_tab() {
    // the keyboards are listed and opened on a worker thread, a busy driver must not stop the window
    auto now = std::chrono::steady_clock::now();
    if (now >= midi_scan_at) {
      midi_scan_at = now + std::chrono::seconds(3);
      midi_io.request_scan();
    }

    gui::simple_section("This computer");
    auto devices = midi_io.devices();
    if (!midi_io.has_scanned()) {
      gui::text_wrapped_disabled("Looking for MIDI keyboards...");
    }
    else if (devices.empty()) {
      gui::text_wrapped_disabled("No MIDI keyboard found on this computer");
    }
    for (const auto& name : devices) {
      gui::text_wrapped(name);
    }
    if (midi_to_host.load() || midi_to_viewers.load()) {
      gui::text_wrapped_disabled(midi_io.is_open() ? "Keyboard connected" : midi_io.is_busy() ? "Connecting to the keyboard..." : "Keyboard not connected");
    }
    else {
      gui::text_wrapped_disabled("The keyboard is opened when you send");
    }

    gui::simple_section("Hear MIDI on this computer");
    gui::checkbox("Play MIDI from the host", &play_host_midi);
    if (gui::checkbox("Let viewers play MIDI here", &allow_midi)) {
      sent_channel_flags.clear();
    }
    gui::text_wrapped_disabled("Other users' keyboards play through this computer's speakers. No stream needed.");
    if (gui::slider("Volume", &midi_volume, 0.f, 2.f, gui::slider_flags_always_clamp)) {
      midi_host.player.set_volume(midi_volume);
    }

    gui::simple_section("Send my keyboard");
    gui::checkbox("To the host", &send_midi);
    if (send_midi) {
      if (!is_in_other_channel()) {
        gui::text_wrapped_disabled("Join another user's channel first");
      }
      else if (!can_midi_host()) {
        gui::text_wrapped_disabled("Waiting for the host to tick \"Let viewers play MIDI here\"");
      }
      else {
        gui::text_wrapped_disabled("Sending to the host");
      }
    }
    gui::checkbox("To everyone in my channel", &send_midi_viewers);
    if (send_midi_viewers) {
      gui::text_wrapped_disabled(ecps_backend.is_hosting_any_channel()
        ? "Sending to the viewers in your channel"
        : "You don't have a channel of your own yet");
    }

    gui::simple_section("Piano");
    gui::checkbox("Show piano", &show_midi_piano);
    if (show_midi_piano) {
      render_midi_piano();
    }
  }

  // the host's keyboard goes to everyone in the channels it hosts, streaming or not
  void send_viewers_midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMidi_t rest;
    rest.Status = status;
    rest.Data1 = data1;
    rest.Data2 = data2;
    backend_queue([rest]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          if (ecps_backend.is_hosting_channel(channel.channel_id)) {
            auto packet = rest;
            packet.ChannelID = channel.channel_id;
            packet.ChannelSessionID = channel.session_id;
            co_await ecps_backend.tcp_write(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMidi, &packet, sizeof(packet));
          }
        }
      }
      catch (...) {}
    });
  }

  // one keyboard feeds both directions: to the host while it allows MIDI, to the viewers of our channel.
  // when a direction ends nothing may keep ringing on the other side
  void update_midi_forward() {
    bool to_host = send_midi && can_midi_host();
    bool to_viewers = send_midi_viewers && ecps_backend.is_hosting_any_channel();
    bool was_to_host = midi_to_host.exchange(to_host);
    bool was_to_viewers = midi_to_viewers.exchange(to_viewers);
    if (!midi_io_started) {
      midi_io_started = true;
      midi_io.start([this](std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
        track_local_note(status, data1, data2);
        if (midi_to_host.load()) {
          send_host_midi(status, data1, data2);
        }
        if (midi_to_viewers.load()) {
          send_viewers_midi(status, data1, data2);
        }
      });
    }
    bool want = to_host || to_viewers;
    midi_io.set_wanted(want);
    if (!want) {
      for (auto& key : local_down) {
        key = false;
      }
    }
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
      if (was_to_host && !to_host) {
        send_host_midi(0xB0 | channel, 64, 0);
        send_host_midi(0xB0 | channel, 123, 0);
      }
      if (was_to_viewers && !to_viewers) {
        send_viewers_midi(0xB0 | channel, 64, 0);
        send_viewers_midi(0xB0 | channel, 123, 0);
      }
    }
  }

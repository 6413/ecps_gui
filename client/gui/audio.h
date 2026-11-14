// Part of ecps_gui_t, included inside the struct by gui.h: sharing and playing sound.

  void render_share_audio_checkbox() {
    if (!fan::audio::loopback_capture_t::is_supported()) {
      return;
    }
    if (settings_switch("Share audio", &stream_settings.share_audio, "Send your computer's sound")) {
      save_audio_settings();
    }
    if (!stream_settings.share_audio) {
      return;
    }

    auto& devices = refresh_loopback_devices(false);
    static bool combo_was_open = false;
    std::string preview = stream_settings.audio_device.empty() ? devices.front().name : "(unplugged device)";
    for (const auto& device : devices) {
      if (!stream_settings.audio_device.empty() && device.id == stream_settings.audio_device) {
        preview = device.name;
      }
    }

    bool combo_open = false;
    settings_row("Device", [&] {
    combo_open = gui::begin_combo("##audio_device", preview);
    if (combo_open) {
      // re-list on open so newly plugged devices show up
      if (!combo_was_open) {
        refresh_loopback_devices(true);
      }
      for (const auto& device : devices) {
        gui::push_id(device.id.empty() ? std::string("default") : device.id);
        if (gui::selectable(device.name, device.id == stream_settings.audio_device)) {
          stream_settings.audio_device = device.id;
          save_audio_settings();
        }
        gui::pop_id();
      }
      gui::end_combo();
    }
    });
    combo_was_open = combo_open;

    auto& settings = stream_settings.audio_devices[stream_settings.audio_device];
    bool can_exclude = fan::audio::loopback_capture_t::can_exclude_processes();
    bool exclude = can_exclude && settings.exclude_discord;
    if (can_exclude && settings_switch("Leave out Discord", &exclude,
      "Every app except Discord, so calls stay private")) {
      settings.exclude_discord = exclude;
      save_audio_settings();
    }
    if (exclude && !find_sonar_gaming().empty()) {
      text_wrapped_disabled("Sonar: recording Sonar - Gaming (Discord and mic stay on Chat/Mic)");
    }
    else if (exclude && audio_share.capture.is_running()) {
      text_wrapped_disabled(audio_share.capture.get_excluded_pid()
        ? "Recording all apps except Discord"
        : audio_share.exclude_failed ? "Could not exclude Discord, recording the device" : "Discord is not running");
    }

    settings_row("Level", [&] {
      gui::slider("##audio_gain", &settings.gain, 0.f, 2.f, gui::slider_flags_always_clamp);
      if (gui::is_item_deactivated_after_edit()) {
        save_audio_settings();
      }
    });
  }

  // the list is re-read at most every 10 s unless forced (it also detects Sonar starting or stopping)
  const std::vector<fan::audio::loopback_device_t>& refresh_loopback_devices(bool force) {
    auto now = std::chrono::steady_clock::now();
    if (force || loopback_devices.empty() || now - loopback_devices_listed > std::chrono::seconds(10)) {
      loopback_devices = fan::audio::get_loopback_devices();
      loopback_devices_listed = now;
    }
    return loopback_devices;
  }

  // SteelSeries Sonar re-plays its Chat (Discord) and Mic monitoring mixes from its own process, so
  // process loopback excluding Discord still records them; its Gaming device carries only game apps
  std::string find_sonar_gaming() {
    for (const auto& device : refresh_loopback_devices(false)) {
      if (device.name.find("Sonar - Gaming") != std::string::npos) {
        return device.id;
      }
    }
    return {};
  }

  fan::audio::loopback_capture_t::options_t get_capture_options() {
    auto& settings = stream_settings.audio_devices[stream_settings.audio_device];
    bool exclude = settings.exclude_discord && fan::audio::loopback_capture_t::can_exclude_processes();
    std::string device_id = stream_settings.audio_device;
    if (exclude && stream_settings.share_audio) {
      if (std::string sonar = find_sonar_gaming(); !sonar.empty()) {
        device_id = std::move(sonar);
        exclude = false;
      }
    }
    return {
      .device_id = device_id,
      .exclude_discord = exclude,
      .gain = settings.gain,
    };
  }

  std::vector<fan::audio::loopback_device_t> loopback_devices;
  std::chrono::steady_clock::time_point loopback_devices_listed;

  void render_play_audio_controls() {
    if (settings_switch("Play audio", &stream_settings.play_audio, "Play the stream's sound")) {
      save_audio_settings();
    }
    if (stream_settings.play_audio) {
      settings_row("Volume", [&] {
        gui::slider("##audio_volume", &stream_settings.audio_volume, 0.f, 2.f, gui::slider_flags_always_clamp);
        if (gui::is_item_deactivated_after_edit()) {
          save_audio_settings();
        }
      });
    }
  }

  void save_audio_settings() {
    fan::json audio;
    audio["share"] = stream_settings.share_audio;
    audio["play"] = stream_settings.play_audio;
    audio["volume"] = stream_settings.audio_volume;
    audio["device"] = stream_settings.audio_device;
    fan::json devices = fan::json::object();
    for (const auto& [id, settings] : stream_settings.audio_devices) {
      devices[id] = {{"gain", settings.gain}, {"exclude_discord", settings.exclude_discord}};
    }
    audio["devices"] = devices;
    write_to_config("audio", audio);
  }

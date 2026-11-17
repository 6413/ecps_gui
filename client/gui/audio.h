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

  bool render_voice_switch(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    if (!ecps_backend.voice_known(channel_id)) {
      return false;
    }
    bool in_voice = voice_chat.is_wanted(channel_id);
    if (settings_switch("Voice chat", &in_voice, "Talk with the others in this channel. Your microphone stays off until you join")) {
      if (in_voice) {
        voice_chat.join(channel_id);
      }
      else {
        voice_chat.leave();
      }
    }
    if (in_voice) {
      bool muted = voice_chat.muted.load();
      if (settings_switch("Mute microphone", &muted, "The others stop hearing you")) {
        voice_chat.muted.store(muted);
      }
      bool deafened = voice_chat.deafened.load();
      if (settings_switch("Mute others", &deafened, "You stop hearing the voice chat")) {
        voice_chat.deafened.store(deafened);
      }
    }
    render_voice_panel();
    return in_voice;
  }

  // the microphone, speakers and filters live in the channel panel itself, folded away until asked for
  void render_voice_panel() {
    gui::dummy(fan::vec2(0, 2.f));
    if (gui::button(voice_settings_open ? "Hide voice settings" : "Voice settings")) {
      voice_settings_open = !voice_settings_open;
    }
    if (!voice_settings_open) {
      return;
    }
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, 8.f));
    gui::push_style_var(gui::style_var_frame_rounding, 5.f);
    settings_section("VOICE SETTINGS");
    render_voice_settings();
    gui::pop_style_var(2);
    gui::dummy(fan::vec2(0, 6.f));
  }

  void render_voice_settings() {
    if (!voice_chat.is_active()) {
      text_wrapped_disabled("Join the voice chat of this channel to try these");
    }
    bool echo_cancel = stream_settings.voice_echo_cancel;
    if (settings_switch("Echo cancellation", &echo_cancel,
      "For speakers: keeps the others from hearing themselves back through your microphone. Not needed with headphones, and adds a little delay")) {
      stream_settings.voice_echo_cancel = echo_cancel;
      save_voice_settings();
    }
    bool noise_suppress = stream_settings.voice_noise_suppress;
    if (settings_switch("Noise suppression", &noise_suppress,
      "Removes steady background noise from your microphone: fans, hiss, hum. Your voice is left as it is")) {
      stream_settings.voice_noise_suppress = noise_suppress;
      save_voice_settings();
    }
    if (voice_chat.capture_failed) {
      text_wrapped_disabled("Could not open the microphone");
    }
    if (voice_chat.mixer_failed) {
      text_wrapped_disabled("Could not open the speakers");
    }

    if (voice_inputs.empty()) {
      voice_inputs = fan::audio::get_microphone_devices();
      voice_outputs = fan::audio::get_loopback_devices();
    }
    auto device_row = [&](const char* label, const char* id, int index, std::string& chosen) {
      auto& devices = index == 0 ? voice_inputs : voice_outputs;
      std::string preview = chosen.empty() ? devices.front().name : "(unplugged device)";
      for (const auto& device : devices) {
        if (!chosen.empty() && device.id == chosen) {
          preview = device.name;
        }
      }
      bool open = false;
      settings_row(label, [&] {
        open = gui::begin_combo(id, preview);
        if (open) {
          if (!voice_combo_open[index]) {
            devices = index == 0 ? fan::audio::get_microphone_devices() : fan::audio::get_loopback_devices();
          }
          for (const auto& device : devices) {
            gui::push_id(device.id.empty() ? std::string("default") : device.id);
            if (gui::selectable(device.name, device.id == chosen)) {
              chosen = device.id;
              save_voice_settings();
            }
            gui::pop_id();
          }
          gui::end_combo();
        }
      });
      voice_combo_open[index] = open;
    };
    device_row("Microphone", "##voice_device", 0, stream_settings.voice_device);
    device_row("Speakers", "##voice_output", 1, stream_settings.voice_output);

    auto saved_slider = [&](const char* label, const char* id, f32_t* value, f32_t max) {
      settings_row(label, [&] {
        gui::slider(id, value, 0.f, max, gui::slider_flags_always_clamp);
        if (gui::is_item_deactivated_after_edit()) {
          save_voice_settings();
        }
      });
    };
    saved_slider("Mic level", "##voice_gain", &stream_settings.voice_gain, 2.f);
    saved_slider("Send above", "##voice_threshold", &stream_settings.voice_threshold, voice_meter_range);

    fan::vec2 at = gui::get_cursor_screen_pos();
    f32_t width = gui::get_content_region_avail().x;
    f32_t level = std::min(voice_chat.level.load() / voice_meter_range, 1.f);
    f32_t mark = width * std::min(stream_settings.voice_threshold / voice_meter_range, 1.f);
    bool sending = voice_chat.is_talking((std::uint32_t)ecps_backend.session_id.i);
    auto* list = gui::get_window_draw_list();
    list->AddRectFilled(at, at + fan::vec2(width, 4.f), 0x30ffffffu, 2.f);
    list->AddRectFilled(at, at + fan::vec2(width * level, 4.f), sending ? 0xff60d060u : 0xff909090u, 2.f);
    list->AddRectFilled(at + fan::vec2(mark - 1.f, -2.f), at + fan::vec2(mark + 1.f, 6.f), 0xffffffffu);
    gui::dummy(fan::vec2(width, 6.f));

    saved_slider("Volume", "##voice_volume", &stream_settings.voice_volume, 2.f);
  }

  voice_chat_t::settings_t get_voice_settings() {
    return {
      .device = stream_settings.voice_device,
      .gain = stream_settings.voice_gain,
      .threshold = stream_settings.voice_threshold,
      .volume = stream_settings.voice_volume,
      .output_device = stream_settings.voice_output,
      .echo_cancel = stream_settings.voice_echo_cancel,
      .noise_suppress = stream_settings.voice_noise_suppress,
    };
  }

  void save_voice_settings() {
    fan::json voice;
    voice["device"] = stream_settings.voice_device;
    voice["output"] = stream_settings.voice_output;
    voice["gain"] = stream_settings.voice_gain;
    voice["threshold"] = stream_settings.voice_threshold;
    voice["volume"] = stream_settings.voice_volume;
    voice["echo_cancel"] = stream_settings.voice_echo_cancel;
    voice["noise_suppress"] = stream_settings.voice_noise_suppress;
    write_to_config("voice", voice);
  }

  static constexpr f32_t voice_meter_range = 0.5f;
  bool voice_settings_open = false;
  std::vector<fan::audio::loopback_device_t> voice_inputs;
  std::vector<fan::audio::loopback_device_t> voice_outputs;
  bool voice_combo_open[2] = {};

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

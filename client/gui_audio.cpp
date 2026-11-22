// Audio implementation, out of line.
#include "gui_std.h" // precompiled, first
#include "ecps_app.h"

// a gain slider shown as a percentage (1 = 100%), the way call apps show volume
static bool percent_slider(const char* id, f32_t* value, f32_t max = 2.f) {
  f32_t shown = *value * 100.f;
  f32_t shown_min = 0.f;
  f32_t shown_max = max * 100.f;
  bool changed = gui::slider_scalar(id, gui::get_imgui_data_type<f32_t>(), &shown, &shown_min, &shown_max,
    "%.0f%%", gui::slider_flags_always_clamp);
  if (changed) {
    *value = shown / 100.f;
  }
  return changed;
}

// a click on a speaker icon mutes and remembers the level, another brings it back
static void toggle_volume(f32_t* value, f32_t& previous) {
  if (*value > 0.f) {
    previous = *value;
    *value = 0.f;
  }
  else {
    *value = previous > 0.f ? previous : 1.f;
  }
}

void render_thread_t::ecps_gui_t::render_share_audio_checkbox() {
    if (!fan::audio::loopback_capture_t::is_supported()) {
      return;
    }
    if (gui::settings_switch("Share audio", &stream_settings.share_audio, "Send your computer's sound")) {
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
    gui::settings_row("Device", [&] {
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
    bool one_window = can_exclude && shared_window_pid() != 0;
    bool default_device = stream_settings.audio_device.empty();
    bool exclude = can_exclude && settings.exclude_discord;
    if (one_window) {
      settings_hint(audio_share.capture.is_running() && !audio_share.capture.get_included_pid()
        ? "Could not record only the shared window's app, recording the device"
        : "Only the shared window's app is heard");
    }
    else if (!default_device) {
      settings_hint("Records only what plays on this device");
    }
    // the notes below are about leaving out Discord, which only the default output does
    exclude = exclude && default_device && !one_window;
    if (exclude && !find_sonar_gaming().empty()) {
      settings_hint("Sonar: recording Sonar - Gaming (Discord and mic stay on Chat/Mic)");
    }
    else if (exclude && audio_share.capture.is_running()) {
      settings_hint(audio_share.capture.get_excluded_pid()
        ? "Recording all apps except Discord"
        : audio_share.exclude_failed ? "Could not exclude Discord, recording the device" : "Discord is not running");
    }

    gui::settings_row("Level", [&] {
      percent_slider("##audio_gain", &settings.gain);
      if (gui::is_item_deactivated_after_edit()) {
        save_audio_settings();
      }
    });
  }

std::string render_thread_t::ecps_gui_t::find_sonar_gaming() {
    for (const auto& device : refresh_loopback_devices(false)) {
      if (device.name.find("Sonar - Gaming") != std::string::npos) {
        return device.id;
      }
    }
    return {};
  }

void render_thread_t::ecps_gui_t::render_play_audio_controls() {
    if (gui::settings_switch("Play audio", &stream_settings.play_audio, "Play the stream's sound")) {
      save_audio_settings();
    }
    if (stream_settings.play_audio) {
      gui::settings_row("Volume", [&] {
        if (icon_button("##mute_audio_volume",
          stream_settings.audio_volume > 0.f ? icon_e::speaker : icon_e::speaker_muted,
          stream_settings.audio_volume > 0.f ? "Mute" : "Unmute")) {
          toggle_audio_volume();
        }
        gui::same_line();
        percent_slider("##audio_volume", &stream_settings.audio_volume);
        if (gui::is_item_deactivated_after_edit()) {
          save_audio_settings();
        }
      });
    }
  }

bool render_thread_t::ecps_gui_t::render_voice_switch(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    if (!ecps_backend.voice_known(channel_id)) {
      return false;
    }
    bool in_voice = voice_chat.is_wanted(channel_id);
    if (gui::settings_switch("Voice chat", &in_voice, "Talk in this channel; your mic starts muted")) {
      if (in_voice) {
        voice_chat.join(channel_id);
      }
      else {
        voice_chat.leave();
      }
      save_server_voice(in_voice);
    }
    if (in_voice) {
      // the microphone is muted by clicking your own voice badge in the list, or with its key
      bool deafened = voice_chat.deafened.load();
      if (gui::settings_switch("Mute others", &deafened, ("Mute the voice chat. Key: " + voice_key_text(action_toggle_deafen)).c_str())) {
        voice_chat.deafened.store(deafened);
        save_server_voice(true);
      }
    }
    return in_voice;
  }

void render_thread_t::ecps_gui_t::render_audio_tab() {
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, settings_space(3.f)));
    gui::push_style_var(gui::style_var_frame_padding, fan::vec2(gui::get_style().FramePadding.x,
      std::max(gui::get_style().FramePadding.y, gui::get_font_size() * 0.22f)));
    gui::push_style_var(gui::style_var_frame_rounding, 5.f);
    gui::dummy(fan::vec2(0, 2.f));
    render_voice_settings();
    gui::pop_style_var(3);
    gui::dummy(fan::vec2(0, 6.f));
  }

void render_thread_t::ecps_gui_t::render_voice_settings() {
    if (!voice_chat.is_active()) {
      settings_hint("Join the voice chat of this channel to try these");
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
      gui::settings_row(label, [&] {
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

    settings_section_icon(icon_e::mic, "INPUT");
    gui::indent(settings_space(2.f));
    device_row("Microphone", "##voice_device", 0, stream_settings.voice_device);
    gui::settings_row("Mic level", [&] {
      bool muted = voice_chat.muted.load();
      if (icon_button("##mute_mic", muted ? icon_e::mic_muted : icon_e::mic, muted ? "Unmute mic" : "Mute mic")) {
        toggle_mic_mute();
      }
      gui::same_line();
      percent_slider("##voice_gain", &stream_settings.voice_gain);
      if (gui::is_item_deactivated_after_edit()) {
        save_voice_settings();
      }
    });
    gui::settings_row("Send above", [&] {
      percent_slider("##voice_threshold", &stream_settings.voice_threshold, voice_meter_range);
      if (gui::is_item_deactivated_after_edit()) {
        save_voice_settings();
      }
    });

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
    if (voice_chat.capture_failed) {
      settings_hint("Could not open the microphone");
    }

    bool echo_cancel = stream_settings.voice_echo_cancel;
    if (gui::settings_switch("Echo cancellation", &echo_cancel,
      "Stops echo when using speakers. Adds a little delay")) {
      stream_settings.voice_echo_cancel = echo_cancel;
      save_voice_settings();
    }
    bool noise_suppress = stream_settings.voice_noise_suppress;
    if (gui::settings_switch("Noise suppression", &noise_suppress,
      "Removes background noise like fans and hum")) {
      stream_settings.voice_noise_suppress = noise_suppress;
      save_voice_settings();
    }
    bool normalize = stream_settings.voice_normalize;
    if (gui::settings_switch("Voice normalization", &normalize,
      "Evens out loud and quiet voices")) {
      stream_settings.voice_normalize = normalize;
      save_voice_settings();
    }

    gui::unindent(settings_space(2.f));

    settings_section_icon(icon_e::speaker, "OUTPUT");
    gui::indent(settings_space(2.f));
    device_row("Speakers", "##voice_output", 1, stream_settings.voice_output);
    if (voice_chat.mixer_failed) {
      settings_hint("Could not open the speakers");
    }
    gui::settings_row("Volume", [&] {
      if (icon_button("##mute_voice_volume",
        stream_settings.voice_volume > 0.f ? icon_e::speaker : icon_e::speaker_muted,
        stream_settings.voice_volume > 0.f ? "Mute" : "Unmute")) {
        toggle_volume(&stream_settings.voice_volume, premute_voice_volume);
        save_voice_settings();
      }
      gui::same_line();
      percent_slider("##voice_volume", &stream_settings.voice_volume);
      if (gui::is_item_deactivated_after_edit()) {
        save_voice_settings();
      }
    });
    // message, join and leave sounds. Letting go plays one, to hear the new level
    gui::settings_row("Notification sounds", [&] {
      if (icon_button("##mute_notify_volume",
        stream_settings.notify_volume > 0.f ? icon_e::speaker : icon_e::speaker_muted,
        stream_settings.notify_volume > 0.f ? "Mute" : "Unmute")) {
        toggle_volume(&stream_settings.notify_volume, premute_notify_volume);
        save_voice_settings();
      }
      gui::same_line();
      percent_slider("##notify_volume", &stream_settings.notify_volume);
      if (gui::is_item_deactivated_after_edit()) {
        save_voice_settings();
        notify_sounds.request(notify_sounds_t::message);
      }
    });
    gui::unindent(settings_space(2.f));
  }

void render_thread_t::ecps_gui_t::add_voice_keybinds() {
    auto& actions = owner->engine.get_input_action();
    // a saved binding, even an emptied one, wins over the default
    if (!actions.exists(action_toggle_mute)) {
      actions.add_keycombo({fan::key_left_control, fan::key_left_shift, fan::key_m}, action_toggle_mute);
    }
    if (!actions.exists(action_toggle_deafen)) {
      actions.add_keycombo({fan::key_left_control, fan::key_left_shift, fan::key_d}, action_toggle_deafen);
    }
  }

std::string render_thread_t::ecps_gui_t::voice_key_text(const char* action) {
    auto& actions = owner->engine.get_input_action();
    // the key type through the map, not by its name: clang doesn't see fan::window::input_action_t from here
    // (import fan doesn't bring in its module), MSVC does
    using key_t = typename std::remove_cvref_t<decltype(actions.input_actions)>::key_type;
    auto found = actions.input_actions.find(key_t(action));
    if (found != actions.input_actions.end()) {
      for (const auto& chord : found->second.keybinds) {
        if (!chord.empty() && !chord[0].empty()) {
          return actions.combo_to_string(chord[0]);
        }
      }
    }
    return "none, set one in Settings > Keybinds";
  }

void render_thread_t::ecps_gui_t::toggle_mic_mute() {
    voice_chat.muted.store(!voice_chat.muted.load());
    save_server_voice(voice_chat.is_active());
  }

void render_thread_t::ecps_gui_t::toggle_audio_volume() {
    toggle_volume(&stream_settings.audio_volume, premute_audio_volume);
    save_audio_settings();
  }

void render_thread_t::ecps_gui_t::toggle_deafen() {
    voice_chat.deafened.store(!voice_chat.deafened.load());
    save_server_voice(voice_chat.is_active());
  }

void render_thread_t::ecps_gui_t::update_voice_keys() {
    if (!voice_keys_added) {
      add_voice_keybinds();
      voice_keys_added = true;
    }
    if (gui::get_io().WantTextInput) {
      return;
    }
    if (auto* settings = fan_settings_menu(); settings && settings->keybind_menu.is_capturing()) {
      return;
    }
    if (!voice_chat.is_active()) {
      return;
    }
    if (owner->engine.is_active(action_toggle_mute)) {
      toggle_mic_mute();
    }
    if (owner->engine.is_active(action_toggle_deafen)) {
      toggle_deafen();
    }
  }

void render_thread_t::ecps_gui_t::toggle_user_mute(std::uint32_t session, const std::string& name) {
    f32_t now = user_volume(name);
    f32_t next = 0.f;
    if (now <= 0.f) {
      auto it = premute_user_volumes.find(name);
      next = it != premute_user_volumes.end() && it->second > 0.f ? it->second : 1.f;
    }
    else {
      premute_user_volumes[name] = now;
    }
    voice_user_volumes[name] = next;
    voice_chat.set_user_volume(session, next);
    save_voice_settings();
  }

f32_t render_thread_t::ecps_gui_t::user_volume(const std::string& name) const {
    auto it = voice_user_volumes.find(name);
    return it == voice_user_volumes.end() ? 1.f : it->second;
  }

void render_thread_t::ecps_gui_t::set_user_volume(const std::string& name, f32_t volume) {
    voice_user_volumes[name] = volume;
    voice_chat.set_user_volume(volume_user_session, volume);
    save_voice_settings();
  }

void render_thread_t::ecps_gui_t::render_user_volume_popup() {
    if (!gui::begin_popup("##user_volume")) {
      return;
    }
    gui::text("Voice volume for " + volume_user_name);
    bool muted = user_volume(volume_user_name) <= 0.f;
    if (icon_button("##mute_user_volume", muted ? icon_e::speaker_muted : icon_e::speaker, muted ? "Unmute" : "Mute")) {
      toggle_user_mute(volume_user_session, volume_user_name);
      volume_user_value = user_volume(volume_user_name);
    }
    gui::same_line();
    // heard while dragging, saved when let go
    if (percent_slider("##user_volume_slider", &volume_user_value)) {
      voice_chat.set_user_volume(volume_user_session, volume_user_value);
    }
    if (gui::is_item_deactivated_after_edit()) {
      set_user_volume(volume_user_name, volume_user_value);
    }
    if (gui::button("Reset")) {
      volume_user_value = 1.f;
      set_user_volume(volume_user_name, 1.f);
    }
    // what the row's buttons do, here too
    if (ecps_backend.is_channel_joined(selected_channel_id)) {
      gui::separator();
      auto sharing = ecps_backend.streamers_of(selected_channel_id);
      if (std::find(sharing.begin(), sharing.end(), volume_user_session) != sharing.end() && gui::menu_item("Watch")) {
        set_watch(selected_channel_id, volume_user_session);
        window_handler.main_tab = 1;
      }
      if (gui::menu_item("Send a file")) {
        ecps_backend_t::Protocol_SessionID_t peer{};
        peer.i = (decltype(peer.i))volume_user_session;
        send_file_dialog(selected_channel_id, peer);
      }
      if (gui::menu_item(user_volume(volume_user_name) <= 0.f ? "Unmute" : "Mute")) {
        toggle_user_mute(volume_user_session, volume_user_name);
        volume_user_value = user_volume(volume_user_name);
      }
    }
    gui::end_popup();
  }

std::string render_thread_t::ecps_gui_t::server_voice_key() const {
    return ecps_backend.ip + ":" + std::to_string(ecps_backend.port);
  }

void render_thread_t::ecps_gui_t::save_server_voice(bool on) {
    if (ecps_backend.ip.empty()) {
      return;
    }
    fan::json servers = config.contains("voice_servers") && config["voice_servers"].is_object() ? config["voice_servers"] : fan::json::object();
    servers[server_voice_key()] = {{"on", on}, {"muted", voice_chat.muted.load()}, {"deafened", voice_chat.deafened.load()}};
    write_to_config("voice_servers", servers);
  }

void render_thread_t::ecps_gui_t::update_server_voice() {
    auto now = std::chrono::steady_clock::now();
    if (now < next_voice_restore) {
      return;
    }
    next_voice_restore = now + std::chrono::seconds(1);
    std::uint32_t session = (std::uint32_t)ecps_backend.session_id.i;
    // the open channel, or the one joined at start (auto join does not always open it)
    auto channel_id = selected_channel_id;
    if (!ecps_backend.is_channel_joined(channel_id) && !ecps_backend.channel_info.empty()) {
      channel_id = ecps_backend.channel_info.front().channel_id;
    }
    if (!ecps_backend.connected || !ecps_backend.is_channel_joined(channel_id) || !ecps_backend.voice_known(channel_id)) {
      return;
    }
    // once per channel and connection: turning it off by hand stays off, coming back after a reconnect turns it on
    if (voice_restored_session == session && voice_restored_channel == (int)channel_id.i) {
      return;
    }
    voice_restored_session = session;
    voice_restored_channel = (int)channel_id.i;
    if (!config.contains("voice_servers") || !config["voice_servers"].is_object() || !config["voice_servers"].contains(server_voice_key())) {
      return;
    }
    fan::json saved = config["voice_servers"][server_voice_key()];
    if (!saved.is_object() || !saved.value("on", false) || voice_chat.is_wanted(channel_id)) {
      return;
    }
    voice_chat.muted.store(saved.value("muted", false));
    voice_chat.deafened.store(saved.value("deafened", false));
    voice_chat.join(channel_id);
    fan::print("voice chat: on again, as it was left on this server");
  }

void render_thread_t::ecps_gui_t::save_voice_settings() {
    fan::json voice;
    voice["device"] = stream_settings.voice_device;
    voice["output"] = stream_settings.voice_output;
    voice["gain"] = stream_settings.voice_gain;
    voice["threshold"] = stream_settings.voice_threshold;
    voice["volume"] = stream_settings.voice_volume;
    voice["notify_volume"] = stream_settings.notify_volume;
    voice["echo_cancel"] = stream_settings.voice_echo_cancel;
    voice["noise_suppress"] = stream_settings.voice_noise_suppress;
    voice["normalize"] = stream_settings.voice_normalize;
    fan::json users = fan::json::object();
    for (const auto& [name, volume] : voice_user_volumes) {
      users[name] = volume;
    }
    voice["user_volumes"] = users;
    write_to_config("voice", voice);
  }

void render_thread_t::ecps_gui_t::save_audio_settings() {
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

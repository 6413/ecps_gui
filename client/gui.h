#pragma once

inline static fan::vec2 popup_size{ 300, 100 };

struct ecps_gui_t {

#define This this
// the gui owns no engine of its own: it reaches the render thread's one through owner
#define engine (This->owner->engine)

  render_thread_t* owner;


  std::vector<std::function<fan::event::task_t()>> task_queue;

  std::mutex task_queue_mutex;
  void backend_queue(const std::function<fan::event::task_t()>& func) {
    std::lock_guard<std::mutex> lock(task_queue_mutex);
    task_queue.emplace_back(func);
  }

  #include "gui/widgets.h"
  #include "gui/top_bar_component.h"

  top_bar_t<ecps_gui_t> top_bar;

  ecps_gui_t(render_thread_t& thread) : owner(&thread), top_bar(*this) {

    setup_backdrop();
    // lines are smoothed by their own geometry: the textured way needs the font atlas sampled with filtering,
    // and without it every drawn line and icon came out jagged
    gui::get_style().AntiAliasedLinesUseTex = false;
    gui::get_style().AntiAliasedLines = true;
    gui::get_style().AntiAliasedFill = true;
    // more segments in circles and rounded corners, the default leaves their edges a little stepped
    gui::get_style().CircleTessellationMaxError = 0.12f;
    gui::get_style().CurveTessellationTol = 0.5f;

    // dropping files on the window sends them, the next frame asks to whom if it isn't obvious
    file_drop_handle = engine.window.on_drop([this](const fan::window_t::drop_data_t& d) {
      // a font dropped on its row in the settings panel becomes that font, not a chat file
      auto* settings = fan_settings_menu();
      if (settings && settings->font_drop_type >= 0 && !d.paths.empty()) {
        std::string ext = std::filesystem::path(d.paths.front()).extension().string();
        if (ext == ".ttf" || ext == ".otf") {
          settings->set_font_file(settings->font_drop_type, d.paths.front());
          settings->font_drop_type = -1;
          return;
        }
      }
      // the chat showing: the files go into it, to everyone in the channel
      if (chat_showing()) {
        chat_dropped_files.insert(chat_dropped_files.end(), d.paths.begin(), d.paths.end());
        return;
      }
      dropped_files = d.paths;
      drop_popup_requested = !dropped_files.empty();
    });

    config.load(config_path);

    ecps_backend.passwords_changed = [this] { passwords_dirty.store(true); };

    if (config.is_object()) {
      if (config.contains("username") && config["username"].is_string()) {
        ecps_backend.desired_username = config["username"].get<std::string>();
      }
      if (config.contains("channel_passwords") && config["channel_passwords"].is_object()) {
        fan::json saved = config["channel_passwords"];
        for (auto it = saved.begin(); it != saved.end(); ++it) {
          if (it.value().is_string()) {
            ecps_backend.channel_passwords[it.key()] = it.value().get<std::string>();
          }
        }
      }
      if (config.contains("channel_title") && config["channel_title"].is_string()) {
        channel_rename_input = config["channel_title"].get<std::string>();
        ecps_backend.own_channel_title = channel_rename_input;
      }
      if (config.contains("auto_host")) {
        auto_host = config.value("auto_host", false);
      }
      if (config.contains("auto_join") && config["auto_join"].is_string()) {
        auto_join_host = config["auto_join"].get<std::string>();
      }
      if (config.contains("theme") && config["theme"].is_object()) {
        load_theme(config["theme"]);
      }
      // also without a saved theme: the widgets' palette comes from it
      apply_theme();
      if (config.contains("saved_themes")) {
        load_saved_themes(config["saved_themes"]);
      }
      if (config.contains("ui") && config["ui"].is_object()) {
        fan::json ui = config["ui"];
        top_bar_compact = ui.value("compact_bar", false);
        top_bar_stats_mode = std::clamp((int)ui.value("stats_mode", 1.0), 0, 1);
        ui_autohide = ui.value("autohide", false);
        keep_stream_toolbar = ui.value("keep_toolbar", false);
        auto_update = ui.value("auto_update", false);
        mini_player_size = fan::vec2i(std::clamp((int)ui.value("mini_width", 480.0), 160, 8192),
          std::clamp((int)ui.value("mini_height", 270.0), 90, 8192));
        preview_fraction = std::clamp((f32_t)ui.value("preview_size", 1.0), 0.25f, 1.f);
        // a new name: the old one was saved by everyone at 0.8, the old default that made a shared HDR screen dark.
        // 1.0 is the screen as it looks
        hdr_exposure = std::clamp((f32_t)ui.value("hdr_light", 1.0), 0.2f, 2.f);
        preview_fps = std::clamp((int)ui.value("preview_fps", 5.0), 1, 60);
        encode_on_gpu = ui.value("encode_on_gpu", true);
        video_codec = std::clamp(ui.value("video_codec", 2), 0, 2);
        sharp_text = ui.value("sharp_text", false);
        lossless = ui.value("lossless", false);
        decode_on_gpu = ui.value("decode_on_gpu", true);
  // phone upload is out of the window: a saved "on" no longer starts its page
  phone_upload_enabled = false;
      }
      if (config.contains("stream") && config["stream"].is_object()) {
        fan::json stream = config["stream"];
        saved_stream.width = stream.value("width", 0);
        saved_stream.height = stream.value("height", 0);
        saved_stream.fps = stream.value("fps", 0);
        saved_stream.bitrate_mode = stream.value("bitrate_mode", 0);
        saved_stream.bitrate_mbps = stream.value("bitrate_mbps", 10.f);
        saved_stream.scaling_quality = stream.value("scaling_quality", -1);
        saved_stream.preset = stream_preset_from_config(stream);
        saved_stream.valid = true;
      }
    }
    // before an update restart (the old process ends at once): the window's place and size saved now, or the new
    // version opened where it was a while ago
    updater.collect_state = [this]() {
      if (auto* settings = fan_settings_menu()) {
        settings->save();
      }
      return collect_resume_state();
    };
    updater.before_restart = []() {
      // the encode and decode threads stop first, nothing may be in the middle of using what is closed below
      stop_worker_threads();
      audio_share.capture.stop();
      audio_share.player.stop();
      voice_chat.capture.stop();
      voice_chat.mixer.stop();
      if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
        close_capture(rt->screen_encoder);
        rt->screen_encoder.encoder_.close();
        rt->screen_decoder.close();
      }
      fan::print("update: closed the video encoder, decoder and audio before restarting");
    };
    load_resume_state();

    if (config.contains("camera")) {
      stream_settings.camera_device = config["camera"].value("device", std::string());
    }
    if (config.contains("voice")) {
      fan::json voice = config["voice"];
      stream_settings.voice_device = voice.value("device", std::string());
      stream_settings.voice_gain = voice.value("gain", stream_settings.voice_gain);
      stream_settings.voice_threshold = voice.value("threshold", stream_settings.voice_threshold);
      stream_settings.voice_volume = voice.value("volume", stream_settings.voice_volume);
      stream_settings.notify_volume = voice.value("notify_volume", stream_settings.notify_volume);
      stream_settings.voice_output = voice.value("output", std::string());
      stream_settings.voice_echo_cancel = voice.value("echo_cancel", stream_settings.voice_echo_cancel);
      stream_settings.voice_noise_suppress = voice.value("noise_suppress", stream_settings.voice_noise_suppress);
      stream_settings.voice_normalize = voice.value("normalize", stream_settings.voice_normalize);
      stream_settings.voice_push_to_talk = voice.value("push_to_talk", stream_settings.voice_push_to_talk);
      if (voice.contains("user_volumes") && voice["user_volumes"].is_object()) {
        fan::json users = voice["user_volumes"];
        for (auto it = users.begin(); it != users.end(); ++it) {
          if (it.value().is_number()) {
            voice_user_volumes[it.key()] = (f32_t)it.value().get<double>();
          }
        }
      }
    }
    if (config.contains("audio")) {
      fan::json audio = config["audio"];
      stream_settings.share_audio = audio.value("share", stream_settings.share_audio);
      stream_settings.play_audio = audio.value("play", stream_settings.play_audio);
      stream_settings.audio_volume = audio.value("volume", stream_settings.audio_volume);
      stream_settings.audio_level = audio.value("level", stream_settings.audio_level);
      stream_settings.audio_device = audio.value("device", std::string());
      if (audio.contains("devices") && audio["devices"].is_object()) {
        fan::json devices = audio["devices"];
        for (auto it = devices.begin(); it != devices.end(); ++it) {
          auto& settings = stream_settings.audio_devices[it.key()];
          settings.gain = it.value().value("gain", settings.gain);
          settings.exclude_discord = it.value().value("exclude_discord", settings.exclude_discord);
        }
      }
    }

    std::string ip, port;
    if (config.contains("server")) {
      if (config["server"].contains("ip")) {
        ip = config["server"]["ip"];
      }
      if (config["server"].contains("port")) {
        port = config["server"]["port"];
      }
    }
    if (ip.size() && port.size()) {
      backend_queue([=]() -> fan::event::task_t {
        auto connected = co_await ecps_expected(ecps_backend.connect(ip, string_to_number(port)));
        if (!connected || !*connected) {
          co_return;
        }
        if (auto logged = co_await ecps_expected(ecps_backend.login()); !logged) {
          co_return;
        }
        // no channel is made for you, pick one from the list or make your own with "Add new"
        (void)co_await ecps_expected(ecps_backend.request_channel_list());
        });
    }
  }
  static uint32_t string_to_number(const std::string& str) {
    uint32_t v = 0;
    auto [end, error] = std::from_chars(str.data(), str.data() + str.size(), v);
    if (error == std::errc::invalid_argument || error == std::errc::result_out_of_range || end != str.data() + str.size()) {
      fan::print(error == std::errc::result_out_of_range ? "The input was too large for unsigned long" : "The input couldn't be converted to a number");
      return (uint32_t)-1;
    }
    return v;
  }

  #include "gui/server_dialog.h"

  #include "gui/main_window.h"

  #include "gui/resolution.h"

  #include "gui/stream_window.h"

  #include "gui/files.h"
  #include "gui/chat.h"

  #include "gui/audio.h"

  #include "gui/remote.h"

  #include "gui/stream_overlay.h"

  #include "gui/capture_preview.h"
  #include "gui/capture.h"
  #include "gui/capture_linux.h"
  #include "gui/camera.h"
  #include "gui/video_choice.h"

  #include "gui/session.h"

  #include "gui/stream_presets.h"

  #include "gui/theme.h"

  #include "gui/top_bar_state.h"

  #include "gui/logs_midi.h"

  #include "gui/input.h"

  // ---- keys for the panels and full screen, changeable in Settings > Keybinds ---------------------------------
  static constexpr const char* action_toggle_chat = "Chat panel";
  static constexpr const char* action_audio_mixer = "Audio mixer";
  static constexpr const char* action_toggle_settings = "Settings panel";
  static constexpr const char* action_toggle_channels = "Channels panel";
  static constexpr const char* action_toggle_fullscreen = "Full screen stream";
  static constexpr const char* action_shortcuts = "Shortcuts";
  static constexpr const char* action_mini_player = "Mini player";
  bool is_mini_player = false;
  // the mini player's window size, changed by dragging its edges and kept in the config
  fan::vec2i mini_player_size = fan::vec2i(480, 270);
  bool panel_keys_added = false;
  bool show_shortcuts = false;
  bool title_live = false;

  // a finished update says so before it restarts, with Later to keep going (a stream, a call) and restart later
  void render_update_toast() {
    if (updater.state.load() != updater_t::state_e::ready || updater.held.load()) {
      return;
    }
    fan::vec2 display = gui::get_io().DisplaySize;
    gui::set_next_window_pos(fan::vec2(display.x - 16.f, display.y - 16.f), gui::cond_always, fan::vec2(1.f, 1.f));
    if (gui::begin("##update_toast", nullptr, gui::window_flags_no_title_bar | gui::window_flags_no_resize |
      gui::window_flags_no_move | gui::window_flags_no_saved_settings | gui::window_flags_always_auto_resize |
      gui::window_flags_no_docking | gui::window_flags_no_focus_on_appearing | gui::window_flags_no_collapse |
      // above the panels and the stream, it ended up behind them and the restart came unannounced
      gui::window_flags_topmost)) {
      gui::text("Update " + updater.latest() + " is ready");
      gui::text_disabled("Restarting in " + std::to_string(updater.seconds_to_restart()) + " s");
      if (gui::button("Restart now##update_toast")) {
        updater.restart();
      }
      gui::same_line();
      if (gui::button("Later##update_toast")) {
        updater.held.store(true);
        fan::print("update: ready, restart later from About");
      }
    }
    gui::end();
  }

  // every key set in Keybinds, in one window: F1
  void render_shortcuts() {
    if (!show_shortcuts) {
      return;
    }
    auto& actions = engine.get_input_action();
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto& [name, data] : actions.input_actions) {
      for (const auto& chord : data.keybinds) {
        if (!chord.empty() && !chord[0].empty()) {
          rows.emplace_back(std::string(std::string_view(name)), actions.combo_to_string(chord[0]));
          break;
        }
      }
    }
    std::sort(rows.begin(), rows.end());
    fan::vec2 display = gui::get_io().DisplaySize;
    gui::set_next_window_pos(fan::vec2(display.x * 0.5f, display.y * 0.5f), gui::cond_appearing, fan::vec2(0.5f, 0.5f));
    if (gui::begin("Shortcuts##ecps_shortcuts", &show_shortcuts, gui::window_flags_no_collapse |
      gui::window_flags_no_saved_settings | gui::window_flags_always_auto_resize | gui::window_flags_no_docking)) {
      f32_t name_width = 0.f;
      for (const auto& row : rows) {
        name_width = std::max(name_width, gui::calc_text_size(row.first).x);
      }
      for (const auto& row : rows) {
        gui::text(row.first);
        gui::same_line(name_width + 30.f);
        gui::text_disabled(row.second);
      }
      gui::spacing();
      gui::text_disabled("Change them in Settings > Keybinds");
    }
    gui::end();
  }

  void set_fullscreen_stream(bool on) {
    if (on == is_fullscreen_stream) {
      return;
    }
    is_fullscreen_stream = on;
    // full screen is all stream: black behind the picture and its bars. The normal view keeps the backdrop and
    // draws black only inside the stream area
    backdrop_black = on;
    apply_backdrop_color();
    if (on) {
      fullscreen_toolbar_alpha = 1.f;
      fullscreen_toolbar_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      engine.window.set_borderless();
    }
    else {
      engine.window.set_windowed();
    }
  }

  // mini player: the window shrinks to the stream alone, kept above other windows in a corner of the screen
  void set_mini_player(bool on) {
    if (on == is_mini_player) {
      return;
    }
    if (on && is_fullscreen_stream) {
      set_fullscreen_stream(false);
    }
    is_mini_player = on;
    backdrop_black = on;
    apply_backdrop_color();
    engine.window.set_compact(on, mini_player_size);
  }

  void update_panel_keys() {
    auto& actions = engine.get_input_action();
    if (!panel_keys_added) {
      panel_keys_added = true;
      // the engine's game keys (movement, attacks, debug) mean nothing here: out of the keybinds menu, and Ctrl+4 or
      // Ctrl+Shift+R no longer fire
      engine.input.remove_game_actions();
      // a saved binding, even an emptied one, wins over the default
      if (!actions.exists(action_toggle_chat)) {
        actions.add_keycombo({fan::key_left_control, fan::key_left_shift, fan::key_c}, action_toggle_chat);
      }
      if (!actions.exists(action_toggle_settings)) {
        actions.add_keycombo({fan::key_left_control, fan::key_left_shift, fan::key_s}, action_toggle_settings);
        actions.add_keycombo({fan::key_left_control, fan::key_comma}, action_toggle_settings);
      }
      if (!actions.exists(action_toggle_channels)) {
        actions.add_keycombo({fan::key_left_control, fan::key_left_shift, fan::key_l}, action_toggle_channels);
      }
      if (!actions.exists(action_toggle_fullscreen)) {
        actions.add_keycombo({fan::key_f11}, action_toggle_fullscreen);
      }
      if (!actions.exists(action_audio_mixer)) {
        actions.add_keycombo({fan::key_left_control, fan::key_left_shift, fan::key_a}, action_audio_mixer);
      }
      if (!actions.exists(action_shortcuts)) {
        actions.add_keycombo({fan::key_f1}, action_shortcuts);
      }
      if (!actions.exists(action_mini_player)) {
        actions.add_keycombo({fan::key_left_control, fan::key_left_shift, fan::key_p}, action_mini_player);
      }
    }
    if (auto* settings = fan_settings_menu(); settings && settings->keybind_menu.is_capturing()) {
      return;
    }
    // Ctrl+Shift combos type nothing, they work while typing too (Ctrl+Shift+C closes the chat from its box)
    if (engine.is_active(action_toggle_chat)) {
      toggle_side_panel(side_chat);
    }
    if (engine.is_active(action_toggle_settings)) {
      toggle_side_panel(side_settings);
    }
    if (engine.is_active(action_toggle_channels)) {
      toggle_side_panel(side_channels);
    }
    if (engine.is_active(action_audio_mixer)) {
      mixer_requested = true;
    }
    if (engine.is_active(action_toggle_fullscreen)) {
      set_mini_player(false);
      set_fullscreen_stream(!is_fullscreen_stream);
    }
    if (engine.is_active(action_mini_player)) {
      set_mini_player(!is_mini_player);
    }
    if (is_mini_player && !gui::get_io().WantTextInput && engine.window.is_key_clicked(fan::key_escape)) {
      set_mini_player(false);
    }
    if (!gui::get_io().WantTextInput && engine.is_active(action_shortcuts)) {
      show_shortcuts = !show_shortcuts;
    }
    // Esc leaves full screen, unless it is closing something being typed in
    if (is_fullscreen_stream && !gui::get_io().WantTextInput && engine.window.is_key_clicked(fan::key_escape)) {
      set_fullscreen_stream(false);
    }
  }

  void render() {
#if defined(_WIN32)
    updater.refocus_after_update((std::uintptr_t)engine.window.get_win32_handle());
#endif
    // global, not tied to the channel panel being open or to the full-screen stream view
    handle_font_keys();
    update_voice_keys();
    update_panel_keys();
    // the update check, the build and the restart into it: every frame, not only while the top bar is drawn. The
    // full-screen stream view and the auto-hidden bar skipped them, so an update never came while watching
    updater.tick();
    updater.tick_restart();
    update_automatically();
    apply_video_choice_once();
    audio_share.leveling.store(stream_settings.audio_level, std::memory_order_relaxed);
    audio_share.update(stream_settings.share_audio, stream_settings.play_audio, stream_settings.audio_volume, get_capture_options());
    voice_chat.update(get_voice_settings());
    if (auto* rt = get_render_thread()) {
      rt->screen_encoder.set_camera(stream_settings.share_camera, stream_settings.camera_device);
    }
    push_theme();
    render_shortcuts();
    render_update_toast();
    pop_theme();
    // sharing shows in the window's title too, so it's seen from the taskbar
    if (bool live = ecps_backend.is_streaming_to_any_channel(); live != title_live) {
      title_live = live;
      engine.set_window_name(std::string(live ? "\xE2\x97\x8F" " LIVE - " : "") + "ECPS - Every Communication Program Sucks");
    }
    if (is_mini_player) {
      render_mini_player();
      return;
    }
    if (is_fullscreen_stream) {
      render_fullscreen_stream();
      render_chat_viewer();
      update_input_control();
      return;
    }

    // these popups are drawn outside the panels' theme scope: without it they came out in the plain grey default
    push_theme();
    drop_down_server.render();
    render_file_offers();
    render_file_drop();
    render_username_popup();
    pop_theme();

    gui::set_next_window_pos(0);
    gui::set_next_window_size(gui::get_io().DisplaySize);
    gui::begin("A", 0, gui::window_flags_no_docking |
      gui::window_flags_no_saved_settings |
      gui::window_flags_no_focus_on_appearing |
      gui::window_flags_no_move |
      gui::window_flags_no_collapse |
      gui::window_flags_no_background |
      gui::window_flags_no_resize |
      gui::window_flags_no_title_bar |
      gui::window_flags_no_bring_to_front_on_focus |
      gui::window_flags_no_inputs |
      gui::window_flags_no_scrollbar |
      gui::window_flags_no_scroll_with_mouse);
    render_backdrop();
    picture_shown = false; // the stream view sets it again when it draws the picture
    stream_area_shown = false;

    if (window_handler.p_open) {
      window_handler.render();
    }


    gui::end();
    render_chat_viewer();

    update_input_control();
  }

  inline static const char* config_path = "config.json";
  void write_to_config(const std::string& key, auto value) {
    config.write(key, std::move(value));
  }

  bool is_fullscreen_stream = false;

  struct input_forward_t {
    bool captured = false;
    fan::vec2si last_position {-1, -1};
    std::vector<int> held_keys;
    std::vector<std::uint8_t> held_buttons;
  } input_forward;

  fan::config_store_t config;

  bool show_own_stream = false;
  ecps_backend_t::Protocol_ChannelID_t selected_channel_id{ (uint16_t)-1 };
  ecps_backend_t::Protocol_ChannelID_t input_control_channel{ (uint16_t)-1 };
  bool send_midi = false;
  bool send_midi_viewers = false;
  bool play_host_midi = true;
  bool show_midi_piano = true;
  bool logs_show_out = true;
  bool logs_show_err = true;
  bool logs_follow = true;
  std::uint64_t logs_seen_revision = 0;
  f32_t midi_volume = 1.f;
  std::array<std::atomic<bool>, 128> local_down{};
  std::atomic<bool> midi_to_host{false};
  std::atomic<bool> midi_to_viewers{false};
  midi_worker_t midi_io;
  bool midi_io_started = false;
  std::chrono::steady_clock::time_point midi_scan_at;
  // the host lets viewers play MIDI on this computer, off until switched on
  bool allow_midi = false;
  std::unordered_map<std::uint32_t, std::uint8_t> sent_channel_flags;
  std::chrono::steady_clock::time_point flags_sent_at;
  bool show_stream_stats = false;
  bool stats_moved = false;
  fan::vec2 stats_pos = fan::vec2(6.f, 6.f);
  f32_t top_overlay_alpha = 1.f;
  std::chrono::steady_clock::time_point top_overlay_visible_until;
  bool top_bar_compact = false;
  // what the chips show: 0 only the bitrate, 1 the picture size, the frame rate and the bitrate
  int top_bar_stats_mode = 1;
  bool about_popup_requested = false;
  f32_t toolbar_alpha = 1.f;
  std::chrono::steady_clock::time_point toolbar_visible_until;
  // the speaker in the toolbar: its vertical slider stays up while hovered, and goes down when the mouse leaves
  std::chrono::steady_clock::time_point stream_volume_popup_until;
  bool stream_volume_dragging = false;
  // the mixer key was pressed: the stream toolbar opens the mixer over its speaker for a few seconds
  bool mixer_requested = false;
  // on: the stream toolbar stays on screen; off: it fades out unless the mouse is near the bottom
  bool keep_stream_toolbar = false;
  f32_t fullscreen_toolbar_alpha = 1.f;
  std::chrono::steady_clock::time_point fullscreen_toolbar_visible_until;

#undef This
#undef engine
};
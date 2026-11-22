// Part of ecps_gui_t, included inside the struct by gui.h: sharing and playing sound.

  void render_share_audio_checkbox();


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
  std::string find_sonar_gaming();


  // the process of the window being shared, 0 for the whole screen (or on Linux, where the portal picks)
  std::uint32_t shared_window_pid() {
#if defined(_WIN32)
    auto* rt = get_render_thread();
    std::uintptr_t handle = rt ? rt->screen_encoder.mdscr.target_window.load() : 0;
    DWORD pid = 0;
    if (handle) {
      GetWindowThreadProcessId((HWND)handle, &pid);
    }
    return pid;
#else
    return 0;
#endif
  }

  // Sharing one window: only that app's sound. Else a device picked in the list is recorded as it is, and only the
  // default output can leave out Discord (that records the apps of every device, so a picked device would not count)
  fan::audio::loopback_capture_t::options_t get_capture_options() {
    auto& settings = stream_settings.audio_devices[stream_settings.audio_device];
    bool can_process = fan::audio::loopback_capture_t::can_exclude_processes();
    std::string device_id = stream_settings.audio_device;
    std::uint32_t only_pid = can_process ? shared_window_pid() : 0;
    bool exclude = settings.exclude_discord && can_process && device_id.empty() && !only_pid;
    if (exclude && stream_settings.share_audio) {
      if (std::string sonar = find_sonar_gaming(); !sonar.empty()) {
        device_id = std::move(sonar);
        exclude = false;
      }
    }
    return {
      .device_id = device_id,
      .exclude_discord = exclude,
      .only_pid = only_pid,
      .gain = settings.gain,
    };
  }

  std::vector<fan::audio::loopback_device_t> loopback_devices;
  std::chrono::steady_clock::time_point loopback_devices_listed;

  void render_play_audio_controls();


  bool render_voice_switch(ecps_backend_t::Protocol_ChannelID_t channel_id);


  // the microphone, speakers and filters: the Audio tab of the settings panel
  void render_audio_tab();


  void render_voice_settings();


  voice_chat_t::settings_t get_voice_settings() {
    return {
      .device = stream_settings.voice_device,
      .gain = stream_settings.voice_gain,
      .threshold = stream_settings.voice_threshold,
      .volume = stream_settings.voice_volume,
      .output_device = stream_settings.voice_output,
      .echo_cancel = stream_settings.voice_echo_cancel,
      .noise_suppress = stream_settings.voice_noise_suppress,
      .normalize = stream_settings.voice_normalize,
    };
  }

  // ---- mute keys, rebindable in fan's settings under Keybinds ---------------------------------------------
  static constexpr const char* action_toggle_mute = "Mute microphone";
  static constexpr const char* action_toggle_deafen = "Mute voice chat";
  bool voice_keys_added = false;

  void add_voice_keybinds();

  std::string voice_key_text(const char* action);

  void toggle_mic_mute();

  void toggle_audio_volume();

  void toggle_deafen();

  // the keys work anywhere in the program except while typing or picking a new key in the settings
  void update_voice_keys();


  // someone muted just for you with a click on their voice badge, the volume they had comes back on the next click
  std::map<std::string, f32_t> premute_user_volumes;
  // the levels the speaker icons muted, brought back by the next click
  f32_t premute_audio_volume = 1.f;
  f32_t premute_voice_volume = 1.f;
  f32_t premute_notify_volume = 1.f;
  void toggle_user_mute(std::uint32_t session, const std::string& name);


  // ---- one person's playback volume, opened by right clicking them in the Users list ------------------------
  // remembered by name, so it survives that person leaving and joining again under a new session id
  std::map<std::string, f32_t> voice_user_volumes;
  std::uint32_t volume_user_session = 0;
  std::string volume_user_name;
  f32_t volume_user_value = 1.f;

  f32_t user_volume(const std::string& name) const;

  void set_user_volume(const std::string& name, f32_t volume);

  void render_user_volume_popup();


  // ---- voice chat on or off, per server -----------------------------------------------------------------------
  // config.json "voice_servers": {"ip:port": {"on", "muted", "deafened"}}. Turned on once on a server, the voice chat
  // starts again in the channel you join there, with the microphone and the others muted as you left them
  std::string server_voice_key() const;


  void save_server_voice(bool on);


  // once a second from the update loop: joins the voice chat of the selected channel when this server had it on
  void update_server_voice();

  std::chrono::steady_clock::time_point next_voice_restore;
  std::uint32_t voice_restored_session = 0;
  int voice_restored_channel = -1;

  void save_voice_settings();


  static constexpr f32_t voice_meter_range = 0.5f;
  // the voice badges in the member list, shown level per session
  std::unordered_map<std::uint32_t, f32_t> voice_meter_levels;
  std::vector<fan::audio::loopback_device_t> voice_inputs;
  std::vector<fan::audio::loopback_device_t> voice_outputs;
  bool voice_combo_open[2] = {};

  void save_audio_settings();


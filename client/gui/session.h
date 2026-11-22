// Part of ecps_gui_t, included inside the struct by gui.h: names, joining by itself, resuming after an update, saved stream settings, many streamers.

  // ---- name -------------------------------------------------------------------------------------------------

  // the same rule the server applies: 1 to 24 of letters, digits, space, _ . -, no space at either end
  static bool username_is_valid(const std::string& name) {
    if (name.empty() || name.size() > 24 || name.front() == ' ' || name.back() == ' ') {
      return false;
    }
    for (char c : name) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == '-';
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  std::string username_notice_text() const;


  // looks at what the server answered about names. The answer to a change we asked for goes to the popup,
  // an answer we didn't ask for that says no is the saved name not being free here
  void update_username();


  void render_username_popup();


  // ---- channel passwords ---------------------------------------------------------------------------------------

  // The password prompt: shown when the server asks for a password the saved ones don't answer. What is typed is
  // remembered once the server lets us in, see ecps_backend_t::remember_password
  // the + button: the new channel's name first
  void render_new_channel_popup();


  void render_password_popup();


  // the host's part, in the channel's controls
  void render_channel_password_controls(ecps_backend_t::Protocol_ChannelID_t channel_id);


  // the host's last control: deleting the channel, after a question. A server without this ignores the request
  void render_delete_channel_button(ecps_backend_t::Protocol_ChannelID_t channel_id);


  // the backend runs on its own thread, so it only says that the saved passwords changed
  void save_passwords_if_changed();


  bool password_popup_open = false;
  bool password_popup_wrong = false;
  ecps_backend_t::Protocol_ChannelID_t password_popup_channel{ (uint16_t)-1 };
  std::string password_popup_name;
  std::string password_input;
  std::string channel_password_input;
  std::string channel_rename_input;
  bool new_channel_popup_requested = false;
  bool new_channel_popup_focus = false;
  bool password_prompt_show = false;
  bool channel_password_show = false;
  std::atomic<bool> passwords_dirty { false };

  // ---- joining channels by themselves --------------------------------------------------------------------------

  void set_auto_join(const std::string& host_name);


  // Channels are named after their host, and names are unique. A channel that is ticked "Join automatically",
  // or that we were in before an update, is joined as soon as it exists, and looked for again until then
  void update_auto_join();


  // ---- picking the session up again after an update ----------------------------------------------------------

  std::string channel_name_of(ecps_backend_t::Protocol_ChannelID_t channel_id) const;


  // what the Update button hands to the new process. Not streaming: sharing the screen again is not something to
  // start by itself, and neither is letting others read the logs
  std::string collect_resume_state();


  void load_resume_state();


  // ---- sharing again after an update ---------------------------------------------------------------------------
  // Once the channel shared into before is joined again (our own: joined back, or made again when the server lost
  // it), sharing starts into it. Given up after a minute
  struct resume_share_t {
    bool active = false;
    bool own = false;
    std::string channel;
    std::string token;
    std::chrono::steady_clock::time_point since;
    std::chrono::steady_clock::time_point next_try;
  } resume_share;

  void update_resume_share();


  std::string username_input;
  bool username_popup_requested = false;
  bool username_pending = false;
  bool username_pending_default = false;
  bool username_close_popup = false;
  std::string username_error;
  std::string username_notice;
  std::chrono::steady_clock::time_point username_notice_until;
  std::uint32_t username_seen_seq = 0;

  std::string auto_join_host;
  std::vector<std::string> resume_join;
  std::string resume_select;
  std::chrono::steady_clock::time_point next_auto_join;
  // the channel made when there was nothing to join, once per connection
  bool auto_created = false;
  std::uint32_t auto_join_session = 0;
  std::chrono::steady_clock::time_point next_auto_join_list;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> auto_join_tried;

  // ---- stream settings that stay -----------------------------------------------------------------------------

  struct stream_snapshot_t {
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate_mode = 0;
    f32_t bitrate_mbps = 0;
    int scaling_quality = -1;
    bool valid = false;

    bool same(const stream_snapshot_t& other) const {
      return width == other.width && height == other.height && fps == other.fps &&
        bitrate_mode == other.bitrate_mode && bitrate_mbps == other.bitrate_mbps && scaling_quality == other.scaling_quality;
    }
  };

  stream_snapshot_t current_stream_settings() {
    stream_snapshot_t now;
    if (auto* rt = get_render_thread(); rt && rt->screen_encoder.user_set_resolution) {
      // landscape either way: a portrait screen swaps them by itself and that is not a choice of yours
      now.width = (int)std::max(rt->screen_encoder.user_width, rt->screen_encoder.user_height);
      now.height = (int)std::min(rt->screen_encoder.user_width, rt->screen_encoder.user_height);
    }
    now.fps = stream_settings.framerate;
    now.bitrate_mode = stream_settings.bitrate_mode;
    now.bitrate_mbps = stream_settings.bitrate_mbps;
    now.scaling_quality = stream_settings.selected_scaling_quality;
    now.valid = true;
    return now;
  }

  // Resolution, frame rate, bitrate and scaling belong to this program, not to a channel. What was used last is
  // applied at the start, and a change is written to the config a second after the last touch
  void update_stream_settings_persistence();


  stream_snapshot_t saved_stream;
  stream_snapshot_t seen_stream;
  bool stream_applied = false;
  std::chrono::steady_clock::time_point stream_changed_at;

  // ---- many streamers in one channel -------------------------------------------------------------------------

  // Any member of a channel can share into it, and everyone in the channel can watch any sharing member, one at a
  // time. Sharing goes where the share button says, watching is picked here (the first sharer by default)

  std::string session_label(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t session) const;


  struct streamer_option_t {
    ecps_backend_t::Protocol_ChannelID_t channel;
    std::uint32_t session;
    std::string name;
  };

  // the other members that share right now, in every channel this client is in
  std::vector<streamer_option_t> streamer_options() const {
    std::vector<streamer_option_t> options;
    std::uint32_t me = (std::uint32_t)ecps_backend.session_id.i;
    for (const auto& channel : ecps_backend.channel_info) {
      for (std::uint32_t session : ecps_backend.streamers_of(channel.channel_id)) {
        if (session != me) {
          options.push_back({channel.channel_id, session, session_label(channel.channel_id, session)});
        }
      }
    }
    return options;
  }

  void set_watch(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t source);


  // keeps what is watched sensible: still sharing, else the next one (the selected channel's first)
  // The picture goes away when it stops coming: the one watched stopped sharing, or no frame came for 2 s (a
  // sharer sends one at least every 100 ms, also when its screen is still). Otherwise the last frame stayed on
  std::uint64_t picture_frames_seen = 0;
  std::chrono::steady_clock::time_point picture_last_frame_at;
  // no new frame for a moment while the stream is still on: the last picture stays, with a note over it
  bool picture_stalled = false;
  std::chrono::steady_clock::time_point picture_stall_idr_at;

  void update_stale_picture();


  void update_watch();


  // tells the server when this client starts or stops sharing into a channel, and again every 10 s
  void update_stream_state();


  std::string watch_preview(const std::vector<streamer_option_t>& options) const;


  // The top right of the stream area: the Stats tick and whose screen is watched. It fades out like the toolbar and
  // comes back when the mouse goes near the top edge, or while there is no picture
  void render_stream_top_overlay();


  // who to watch
  void render_watch_picker(f32_t width);


  bool watch_active = false;
  std::uint32_t watch_channel = 0;
  std::uint32_t watch_source = 0;
  std::unordered_map<std::uint32_t, bool> stream_state_sent;
  std::chrono::steady_clock::time_point stream_state_sent_at;

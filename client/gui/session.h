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

  std::string username_notice_text() const {
    if (!username_notice.empty() && std::chrono::steady_clock::now() < username_notice_until) {
      return username_notice;
    }
    return {};
  }

  // looks at what the server answered about names. The answer to a change we asked for goes to the popup,
  // an answer we didn't ask for that says no is the saved name not being free here
  void update_username() {
    std::uint32_t seq = ecps_backend.username_result_seq.load();
    if (seq == username_seen_seq) {
      return;
    }
    username_seen_seq = seq;
    int result = ecps_backend.username_result.load();
    std::string name;
    {
      std::lock_guard<std::mutex> lock(ecps_backend.username_mutex);
      name = ecps_backend.username_result_name;
    }
    if (username_pending) {
      username_pending = false;
      if (result == 0) {
        username_error.clear();
        ecps_backend.desired_username = username_pending_default ? std::string() : name;
        write_to_config("username", ecps_backend.desired_username);
        username_close_popup = true;
      }
      else if (result == 1) {
        username_error = "That name is taken, pick another one";
      }
      else if (result == 2) {
        username_error = "Use 1 to 24 letters, digits, spaces, _ . or -";
      }
      else {
        username_error = "Wait a second and try again";
      }
    }
    else if (result == 1 && !ecps_backend.desired_username.empty()) {
      username_notice = "\"" + ecps_backend.desired_username + "\" is taken here, you are " + name;
      username_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    }
  }

  void render_username_popup() {
    if (username_popup_requested) {
      username_popup_requested = false;
      username_input = ecps_backend.get_current_username();
      username_error.clear();
      gui::set_next_window_pos(fan::vec2(engine.window.get_size().x, engine.window.get_size().y) / 2 - fan::vec2(190, 90));
      gui::open_popup("Change name");
    }
    if (!gui::begin_popup("Change name")) {
      return;
    }
    gui::text("Your name");
    gui::push_item_width(320);
    bool enter = gui::input_text("##username_input", &username_input, gui::input_text_flags_enter_returns_true);
    gui::pop_item_width();
    gui::text_wrapped_disabled("1 to 24 letters, digits, spaces, _ . or -. Nobody else can use the same name at the same time.");
    if (!username_error.empty()) {
      gui::text(fan::color(1.f, 0.5f, 0.45f, 1.f), username_error);
    }
    if (!ecps_backend.server_new.load()) {
      gui::text(fan::color(1.f, 0.7f, 0.4f, 1.f), "This server is too old for names, update it first");
    }
    bool can_save = !username_pending && username_is_valid(username_input) && ecps_backend.server_new.load();
    gui::begin_disabled(!can_save);
    if (gui::button("Save") || (enter && can_save)) {
      username_pending = true;
      username_pending_default = false;
      username_error.clear();
      backend_queue([name = username_input]() -> fan::event::task_t {
        co_await ecps_backend.send_set_username(name);
      });
    }
    gui::end_disabled();
    gui::same_line();
    if (gui::button("Use default name")) {
      username_pending = true;
      username_pending_default = true;
      username_error.clear();
      backend_queue([]() -> fan::event::task_t {
        co_await ecps_backend.send_set_username(std::string());
      });
    }
    gui::same_line();
    if (gui::button("Cancel")) {
      gui::close_current_popup();
    }
    if (username_close_popup) {
      username_close_popup = false;
      gui::close_current_popup();
    }
    gui::end_popup();
  }

  // ---- channel passwords ---------------------------------------------------------------------------------------

  // The password prompt: shown when the server asks for a password the saved ones don't answer. What is typed is
  // remembered once the server lets us in, see ecps_backend_t::remember_password
  // the + button: the new channel's name first
  void render_new_channel_popup() {
    if (new_channel_popup_requested) {
      new_channel_popup_requested = false;
      new_channel_popup_focus = true;
      gui::open_popup("New channel");
    }
    if (!gui::begin_popup_modal("New channel", gui::window_flags_always_auto_resize | gui::window_flags_no_saved_settings)) {
      return;
    }
    gui::text("Channel name");
    gui::push_item_width(320);
    if (new_channel_popup_focus) {
      new_channel_popup_focus = false;
      gui::set_keyboard_focus_here();
    }
    bool enter = gui::input_text("##new_channel_name", &channel_rename_input, gui::input_text_flags_enter_returns_true);
    gui::pop_item_width();
    gui::text_wrapped_disabled("Empty uses your name. You can rename it later in the channel's settings");
    if (gui::button("Create") || enter) {
      std::string name = channel_rename_input;
      write_to_config("channel_title", name);
      backend_queue([this, name]() -> fan::event::task_t {
        try {
          ecps_backend.own_channel_title = ecps_backend_t::trim_password(name).substr(0, 31);
          auto channel_id = co_await ecps_backend.channel_create();
          co_await ecps_backend.channel_join(channel_id, true);
          co_await ecps_backend.request_channel_list();
          co_await ecps_backend.request_channel_session_list(channel_id);
          selected_channel_id = channel_id;
        }
        catch (...) {
          fan::print("Failed to create channel");
        }
      });
      gui::close_current_popup();
    }
    gui::same_line();
    if (gui::button("Cancel")) {
      gui::close_current_popup();
    }
    gui::end_popup();
  }

  void render_password_popup() {
    render_new_channel_popup();
    save_passwords_if_changed();
    bool open_now = false;
    if (!password_popup_open) {
      std::lock_guard<std::mutex> lock(ecps_backend.password_mutex);
      if (ecps_backend.password_prompt.wanted) {
        ecps_backend.password_prompt.wanted = false;
        password_popup_channel = ecps_backend.password_prompt.channel_id;
        password_popup_name = ecps_backend.password_prompt.name;
        password_popup_wrong = ecps_backend.password_prompt.wrong;
        open_now = true;
      }
    }
    if (open_now) {
      password_popup_open = true;
      password_input.clear();
      gui::set_next_window_pos(fan::vec2(engine.window.get_size().x, engine.window.get_size().y) / 2 - fan::vec2(190, 80));
      gui::open_popup("Channel password");
    }
    if (!password_popup_open) {
      return;
    }
    // modal: it stays in front, and open, until Join or Cancel
    if (!gui::begin_popup_modal("Channel password", gui::window_flags_always_auto_resize | gui::window_flags_no_saved_settings)) {
      password_popup_open = false;
      return;
    }
    gui::text("\"" + password_popup_name + "\" needs a password");
    if (password_popup_wrong) {
      gui::text(fan::color(1.f, 0.5f, 0.45f, 1.f), "That password was not right");
    }
    gui::push_item_width(320);
    if (open_now) {
      gui::set_keyboard_focus_here();
    }
    bool enter = gui::input_text("##channel_password_prompt", &password_input,
      gui::input_text_flags_enter_returns_true | (password_prompt_show ? 0 : gui::input_text_flags_password));
    gui::pop_item_width();
    // the dots hide a typo, this shows what is being sent
    gui::checkbox("Show password", &password_prompt_show);
    gui::text_wrapped_disabled("It is remembered on this computer, you are not asked again for this channel.");
    bool can_join = !password_input.empty();
    gui::begin_disabled(!can_join);
    if (gui::button("Join") || (enter && can_join)) {
      backend_queue([id = password_popup_channel, password = password_input]() -> fan::event::task_t {
        try {
          co_await ecps_backend.channel_join(id, false, password);
          co_await ecps_backend.request_channel_list();
          co_await ecps_backend.request_channel_session_list(id);
        }
        catch (...) {
          fan::print("Failed to join channel");
        }
      });
      password_input.clear();
      password_popup_open = false;
      gui::close_current_popup();
    }
    gui::end_disabled();
    gui::same_line();
    if (gui::button("Cancel")) {
      password_input.clear();
      password_popup_open = false;
      gui::close_current_popup();
    }
    gui::end_popup();
  }

  // the host's part, in the channel's controls
  void render_channel_password_controls(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    gui::settings_section("NAME");
    gui::push_item_width(-1.f);
    bool rename_enter = gui::input_text("##channel_rename", &channel_rename_input, gui::input_text_flags_enter_returns_true);
    gui::pop_item_width();
    if (gui::button("Rename") || rename_enter) {
      std::string name = channel_rename_input;
      write_to_config("channel_title", name);
      backend_queue([channel_id, name]() -> fan::event::task_t {
        ecps_backend.channel_rename(channel_id, name);
        co_return;
      });
    }
    gui::settings_section("PASSWORD");
    if (!ecps_backend.channel_passwords_supported(channel_id)) {
      gui::text_wrapped_disabled("This server is too old for channel passwords, update it first");
      return;
    }
    bool has_password = false;
    for (const auto& channel : ecps_backend.available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        has_password = channel.is_password_protected;
      }
    }
    gui::push_item_width(-1.f);
    gui::input_text("##channel_password_set", &channel_password_input, channel_password_show ? 0 : gui::input_text_flags_password);
    gui::pop_item_width();
    gui::checkbox("Show##channel_password_show", &channel_password_show);
    // what it is now, so the host can tell the others exactly that
    if (has_password) {
      std::string current = ecps_backend.own_saved_password();
      if (!current.empty()) {
        gui::same_line();
        gui::text_wrapped_disabled("Current: " + (channel_password_show ? current : std::string(current.size(), '*')));
      }
    }
    gui::begin_disabled(channel_password_input.empty());
    if (gui::button(has_password ? "Change password" : "Set password")) {
      backend_queue([channel_id, password = channel_password_input]() -> fan::event::task_t {
        try {
          co_await ecps_backend.channel_set_password(channel_id, password);
        }
        catch (...) {
          fan::print("Failed to set the channel password");
        }
      });
      channel_password_input.clear();
    }
    gui::end_disabled();
    if (has_password) {
      gui::same_line();
      if (gui::button("Remove password")) {
        backend_queue([channel_id]() -> fan::event::task_t {
          try {
            co_await ecps_backend.channel_set_password(channel_id, std::string());
          }
          catch (...) {
            fan::print("Failed to remove the channel password");
          }
        });
      }
    }
  }

  // the host's last control: deleting the channel, after a question. A server without this ignores the request
  void render_delete_channel_button(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    gui::push_style_color(gui::col_button, fan::color(0.45f, 0.08f, 0.08f, 1.f));
    gui::push_style_color(gui::col_button_hovered, fan::color(0.65f, 0.1f, 0.1f, 1.f));
    bool clicked = gui::button("Delete channel");
    gui::pop_style_color(2);
    if (clicked) {
      gui::open_popup("##delete_channel_confirm");
    }
    if (gui::begin_popup("##delete_channel_confirm")) {
      gui::text("Delete this channel for everyone?");
      gui::text_wrapped_disabled("Everyone in it is sent out, and its chat is lost.");
      if (gui::button("Delete##delete_channel_yes")) {
        backend_queue([channel_id]() -> fan::event::task_t {
          try {
            co_await ecps_backend.delete_channel(channel_id);
          }
          catch (...) {
            fan::print("Failed to delete the channel");
          }
        });
        gui::close_current_popup();
      }
      gui::same_line();
      if (gui::button("Cancel##delete_channel_no")) {
        gui::close_current_popup();
      }
      gui::end_popup();
    }
  }

  // the backend runs on its own thread, so it only says that the saved passwords changed
  void save_passwords_if_changed() {
    if (!passwords_dirty.exchange(false)) {
      return;
    }
    fan::json saved = fan::json::object();
    {
      std::lock_guard<std::mutex> lock(ecps_backend.password_mutex);
      for (const auto& [key, password] : ecps_backend.channel_passwords) {
        saved[key] = password;
      }
    }
    write_to_config("channel_passwords", saved);
  }

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

  void set_auto_join(const std::string& host_name) {
    auto_join_host = host_name;
    write_to_config("auto_join", host_name);
  }

  // Channels are named after their host, and names are unique. A channel that is ticked "Join automatically",
  // or that we were in before an update, is joined as soon as it exists, and looked for again until then
  void update_auto_join() {
    if (auto_join_host.empty() && resume_join.empty() && resume_select.empty()) {
      return;
    }
    // a channel asked for its password: no joins without it meanwhile, their failures would cancel the one with it
    if (password_popup_open) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(ecps_backend.password_mutex);
      if (ecps_backend.password_prompt.wanted) {
        return;
      }
    }
    if (!ecps_backend.connected || ecps_backend.session_id.i == 0) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    // a new connection: its channel list is asked for at once, "nothing to join" is told as soon as it is here
    if ((std::uint32_t)ecps_backend.session_id.i != auto_join_session) {
      auto_join_session = (std::uint32_t)ecps_backend.session_id.i;
      auto_created = false;
      next_auto_join = now;
      next_auto_join_list = now;
    }
    if (now < next_auto_join) {
      return;
    }
    // looked at often until the first list is here, then every 2 s
    next_auto_join = now + (ecps_backend.channel_list_received ? std::chrono::milliseconds(2000) : std::chrono::milliseconds(100));
    if (now >= next_auto_join_list) {
      next_auto_join_list = now + std::chrono::seconds(6);
      backend_queue([]() -> fan::event::task_t {
        try {
          co_await ecps_backend.request_channel_list();
        }
        catch (...) {}
      });
    }

    std::vector<std::string> wanted = resume_join;
    if (!auto_join_host.empty()) {
      wanted.push_back(auto_join_host);
    }
    for (const auto& channel : ecps_backend.available_channels) {
      if (std::find(wanted.begin(), wanted.end(), channel.name) == wanted.end()) {
        continue;
      }
      if (channel.host_session_id.i == ecps_backend.session_id.i) {
        continue; // our own channel
      }
      if (ecps_backend.is_channel_joined(channel.channel_id)) {
        resume_join.erase(std::remove(resume_join.begin(), resume_join.end(), channel.name), resume_join.end());
        continue;
      }
      auto tried = auto_join_tried.find(channel.name);
      if (tried != auto_join_tried.end() && now - tried->second < std::chrono::seconds(6)) {
        continue; // a join for it is on its way
      }
      auto_join_tried[channel.name] = now;
      backend_queue([id = channel.channel_id]() -> fan::event::task_t {
        try {
          co_await ecps_backend.channel_join(id);
          co_await ecps_backend.request_channel_list();
          co_await ecps_backend.request_channel_session_list(id);
        }
        catch (...) {}
      });
    }

    // Nothing to join: our own channel is made instead, so the one we wait for finds us there and joins it
    // (their "Join automatically" for our name does that). Once per connection, and only when we are in no channel
    if (!auto_join_host.empty() && !auto_created && ecps_backend.channel_list_received &&
      ecps_backend.channel_info.empty() && !ecps_backend.is_hosting_any_channel()) {
      bool target_exists = std::any_of(ecps_backend.available_channels.begin(), ecps_backend.available_channels.end(),
        [&](const auto& channel) { return channel.name == auto_join_host; });
      if (!target_exists) {
        auto_created = true;
        backend_queue([this]() -> fan::event::task_t {
          try {
            auto channel_id = co_await ecps_backend.channel_create();
            co_await ecps_backend.channel_join(channel_id, true);
            co_await ecps_backend.request_channel_list();
            co_await ecps_backend.request_channel_session_list(channel_id);
            selected_channel_id = channel_id;
          }
          catch (...) {
            fan::print("could not make a channel to wait in");
          }
        });
      }
    }

    // the channel that was open before an update, once we are in it
    if (!resume_select.empty()) {
      for (const auto& channel : ecps_backend.available_channels) {
        if (channel.name == resume_select && (ecps_backend.is_channel_joined(channel.channel_id) ||
          channel.host_session_id.i == ecps_backend.session_id.i)) {
          selected_channel_id = channel.channel_id;
          resume_select.clear();
          break;
        }
      }
    }
  }

  // ---- picking the session up again after an update ----------------------------------------------------------

  std::string channel_name_of(ecps_backend_t::Protocol_ChannelID_t channel_id) const {
    for (const auto& channel : ecps_backend.available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        return channel.name;
      }
    }
    return {};
  }

  // what the Update button hands to the new process. Not streaming: sharing the screen again is not something to
  // start by itself, and neither is letting others read the logs
  std::string collect_resume_state() {
    fan::json state = fan::json::object();
    state["saved_at"] = (std::int64_t)std::time(nullptr);
    state["tab"] = window_handler.current_tab;
    fan::json joined = fan::json::array();
    for (const auto& channel : ecps_backend.channel_info) {
      if (ecps_backend.is_hosting_channel(channel.channel_id)) {
        continue;
      }
      std::string name = channel_name_of(channel.channel_id);
      if (!name.empty()) {
        joined.push_back(name);
      }
    }
    state["joined"] = joined;
    state["selected"] = channel_name_of(selected_channel_id);
    fan::json midi = fan::json::object();
    midi["send_host"] = send_midi;
    midi["send_viewers"] = send_midi_viewers;
    midi["allow"] = allow_midi;
    midi["play"] = play_host_midi;
    midi["piano"] = show_midi_piano;
    midi["volume"] = midi_volume;
    state["midi"] = midi;
    state["show_own_stream"] = show_own_stream;
    // sharing goes on after the update: into the same channel, from the camera or from the same screen or window
    // (on Linux the portal's token skips its dialog). Only this file carries it, a normal start never shares by itself
    for (const auto& channel : ecps_backend.channel_info) {
      if (!channel.is_streaming) {
        continue;
      }
      fan::json share = fan::json::object();
      share["channel"] = channel_name_of(channel.channel_id);
      share["own"] = ecps_backend.is_hosting_channel(channel.channel_id);
      share["camera"] = stream_settings.share_camera;
      if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
        share["token"] = rt->screen_encoder.restore_token();
      }
      state["share"] = share;
      break;
    }
    return state.dump();
  }

  void load_resume_state() {
    if (!fan::io::file::exists(updater_t::state_file)) {
      return;
    }
    std::string data;
    fan::io::file::read(std::string(updater_t::state_file), &data);
    std::error_code ec;
    std::filesystem::remove(updater_t::state_file, ec);
    try {
      fan::json state = fan::json::parse(data);
      // a leftover from long ago is not what the user just did
      std::int64_t age = (std::int64_t)std::time(nullptr) - state.value("saved_at", (std::int64_t)0);
      if (age < 0 || age > 600) {
        return;
      }
      int tab = state.value("tab", 0);
      if (tab >= 0 && tab <= 2) {
        window_handler.main_tab = tab;
      }
      if (state.contains("joined") && state["joined"].is_array()) {
        for (const auto& name : state["joined"]) {
          if (name.is_string()) {
            resume_join.push_back(name.get<std::string>());
          }
        }
      }
      resume_select = state.value("selected", std::string());
      if (state.contains("midi") && state["midi"].is_object()) {
        fan::json midi = state["midi"];
        send_midi = midi.value("send_host", send_midi);
        send_midi_viewers = midi.value("send_viewers", send_midi_viewers);
        allow_midi = midi.value("allow", allow_midi);
        play_host_midi = midi.value("play", play_host_midi);
        show_midi_piano = midi.value("piano", show_midi_piano);
        midi_volume = midi.value("volume", midi_volume);
      }
      show_own_stream = state.value("show_own_stream", show_own_stream);
      if (state.contains("share") && state["share"].is_object()) {
        fan::json share = state["share"];
        resume_share.channel = share.value("channel", std::string());
        resume_share.own = share.value("own", false);
        resume_share.token = share.value("token", std::string());
        stream_settings.share_camera = share.value("camera", stream_settings.share_camera);
        resume_share.active = !resume_share.channel.empty();
        resume_share.since = std::chrono::steady_clock::now();
        // a channel of somebody else's is joined again like the others
        if (resume_share.active && !resume_share.own &&
          std::find(resume_join.begin(), resume_join.end(), resume_share.channel) == resume_join.end()) {
          resume_join.push_back(resume_share.channel);
        }
      }
    }
    catch (...) {}
  }

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

  void update_resume_share() {
    if (!resume_share.active) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - resume_share.since > std::chrono::seconds(60)) {
      fan::print("could not share again after the update: the channel did not come back");
      resume_share.active = false;
      return;
    }
    if (!ecps_backend.connected || ecps_backend.session_id.i == 0 || !ecps_backend.channel_list_received) {
      return;
    }
    const ecps_backend_t::channel_list_info_t* target = nullptr;
    for (const auto& channel : ecps_backend.available_channels) {
      if (channel.name == resume_share.channel) {
        target = &channel;
      }
    }
    if (target && ecps_backend.is_channel_joined(target->channel_id)) {
      if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
        rt->screen_encoder.set_resume_token(resume_share.token);
      }
      selected_channel_id = target->channel_id;
      stream_button_renderer_t::set_sharing(true);
      fan::print("sharing again after the update, into " + resume_share.channel);
      resume_share.active = false;
      return;
    }
    // our own channel: joined back (the server keeps it), or made again. Others are joined by the auto join
    if (!resume_share.own || now < resume_share.next_try) {
      return;
    }
    resume_share.next_try = now + std::chrono::seconds(5);
    if (target) {
      backend_queue([id = target->channel_id]() -> fan::event::task_t {
        try {
          co_await ecps_backend.channel_join(id);
          co_await ecps_backend.request_channel_list();
          co_await ecps_backend.request_channel_session_list(id);
        }
        catch (...) {}
      });
    }
    else if (!ecps_backend.is_hosting_any_channel()) {
      backend_queue([this]() -> fan::event::task_t {
        try {
          auto channel_id = co_await ecps_backend.channel_create();
          co_await ecps_backend.channel_join(channel_id, true);
          co_await ecps_backend.request_channel_list();
          co_await ecps_backend.request_channel_session_list(channel_id);
        }
        catch (...) {}
      });
    }
  }

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
  void update_stream_settings_persistence() {
    auto* rt = get_render_thread();
    if (!rt || !rt->screen_encoder.is_initialized()) {
      return;
    }
    auto& encoder = rt->screen_encoder;
    auto now = std::chrono::steady_clock::now();

    if (!stream_applied) {
      stream_applied = true;
      if (saved_stream.valid) {
        if (saved_stream.width >= 160 && saved_stream.height >= 120 && saved_stream.width <= 7680 && saved_stream.height <= 4320) {
          encoder.set_user_resolution((std::uint32_t)saved_stream.width, (std::uint32_t)saved_stream.height);
        }
        if (saved_stream.fps >= 3 && saved_stream.fps <= 144) {
          stream_settings.framerate = saved_stream.fps;
          encoder.config_.frame_rate = saved_stream.fps;
          std::lock_guard<std::mutex> lock(encoder.mutex);
          encoder.update_flags |= codec_update_e::frame_rate;
        }
        if (saved_stream.bitrate_mode == 0 || saved_stream.bitrate_mode == 1) {
          stream_settings.bitrate_mode = saved_stream.bitrate_mode;
          stream_settings.use_adaptive_bitrate = saved_stream.bitrate_mode == 0;
          if (saved_stream.bitrate_mode == 1 && saved_stream.bitrate_mbps >= 0.5f && saved_stream.bitrate_mbps <= 50.f) {
            stream_settings.bitrate_mbps = saved_stream.bitrate_mbps;
            encoder.config_.bitrate = (int)(saved_stream.bitrate_mbps * 1000000);
            std::lock_guard<std::mutex> lock(encoder.mutex);
            encoder.update_flags |= codec_update_e::rate_control;
          }
        }
        if (saved_stream.scaling_quality >= 0 && saved_stream.scaling_quality <= 4) {
          stream_settings.selected_scaling_quality = saved_stream.scaling_quality;
          encoder.update_scaling_quality(static_cast<codec_config_t::scaling_quality_e>(saved_stream.scaling_quality));
        }
      }
      seen_stream = current_stream_settings();
      return;
    }

    stream_snapshot_t current = current_stream_settings();
    if (!current.same(seen_stream)) {
      seen_stream = current;
      stream_changed_at = now;
      return;
    }
    if (stream_changed_at != std::chrono::steady_clock::time_point{} && now - stream_changed_at > std::chrono::seconds(1) &&
      (!saved_stream.valid || !current.same(saved_stream))) {
      fan::json stream = fan::json::object();
      stream["width"] = current.width;
      stream["height"] = current.height;
      stream["fps"] = current.fps;
      stream["bitrate_mode"] = current.bitrate_mode;
      stream["bitrate_mbps"] = current.bitrate_mbps;
      stream["scaling_quality"] = current.scaling_quality;
      write_to_config("stream", stream);
      saved_stream = current;
      stream_changed_at = std::chrono::steady_clock::time_point{};
    }
  }

  stream_snapshot_t saved_stream;
  stream_snapshot_t seen_stream;
  bool stream_applied = false;
  std::chrono::steady_clock::time_point stream_changed_at;

  // ---- many streamers in one channel -------------------------------------------------------------------------

  // Any member of a channel can share into it, and everyone in the channel can watch any sharing member, one at a
  // time. Sharing goes where the share button says, watching is picked here (the first sharer by default)

  std::string session_label(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t session) const {
    auto sessions = ecps_backend.channel_sessions.find(channel_id.i);
    if (sessions != ecps_backend.channel_sessions.end()) {
      for (const auto& info : sessions->second) {
        if ((std::uint32_t)info.session_id.i == session) {
          return info.username;
        }
      }
    }
    return "user " + std::to_string(session);
  }

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

  void set_watch(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t source) {
    watch_active = true;
    watch_channel = (std::uint32_t)channel_id.i;
    watch_source = source;
    ecps_backend.view_resync = true;
    // the new picture builds on nothing the decoder has, it starts at the sharer's next keyframe
    ecps_backend.view_need_keyframe = true;
    if (auto* rt = get_render_thread()) {
      rt->host_cursor.visible.store(false);
    }
    for (const auto& channel : ecps_backend.channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        backend_queue([id = channel.channel_id, session = channel.session_id, source]() -> fan::event::task_t {
          co_await ecps_backend.send_watch(id, session, ecps_backend_t::Protocol_SessionID_t{ (ecps_backend_t::Protocol_SessionID_t::Type)source });
        });
        break;
      }
    }
  }

  // keeps what is watched sensible: still sharing, else the next one (the selected channel's first)
  // The picture goes away when it stops coming: the one watched stopped sharing, or no frame came for 2 s (a
  // sharer sends one at least every 100 ms, also when its screen is still). Otherwise the last frame stayed on
  std::uint64_t picture_frames_seen = 0;
  std::chrono::steady_clock::time_point picture_last_frame_at;

  void update_stale_picture() {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    std::uint64_t decoded = ecps_backend.view_frames_decoded.load();
    if (decoded != picture_frames_seen) {
      picture_frames_seen = decoded;
      picture_last_frame_at = now;
      return;
    }
    if (rt->network_frame.get_image() == engine.default_texture) {
      return;
    }
    // your own share shown as a preview has no one to watch, only the time counts for it
    bool source_gone = !watch_active && !show_own_stream;
    if (source_gone || now - picture_last_frame_at > std::chrono::seconds(2)) {
      rt->network_frame.set_image(engine.default_texture);
    }
  }

  void update_watch() {
    if (ecps_backend.session_id.i == 0) {
      watch_active = false;
      stream_state_sent.clear();
      return;
    }
    auto options = streamer_options();
    if (watch_active) {
      for (const auto& option : options) {
        if ((std::uint32_t)option.channel.i == watch_channel && option.session == watch_source) {
          return;
        }
      }
    }
    if (options.empty()) {
      watch_active = false;
      return;
    }
    const streamer_option_t* pick = &options.front();
    for (const auto& option : options) {
      if (option.channel.i == selected_channel_id.i) {
        pick = &option;
        break;
      }
    }
    set_watch(pick->channel, pick->session);
  }

  // tells the server when this client starts or stops sharing into a channel, and again every 10 s
  void update_stream_state() {
    auto now = std::chrono::steady_clock::now();
    bool refresh = now - stream_state_sent_at > std::chrono::seconds(10);
    for (const auto& channel : ecps_backend.channel_info) {
      std::uint32_t key = (std::uint32_t)channel.channel_id.i;
      auto sent = stream_state_sent.find(key);
      bool streaming = channel.is_streaming;
      if (sent == stream_state_sent.end() && !streaming) {
        stream_state_sent[key] = false; // nothing to say yet
        continue;
      }
      if (!refresh && sent != stream_state_sent.end() && sent->second == streaming) {
        continue;
      }
      stream_state_sent[key] = streaming;
      backend_queue([id = channel.channel_id, session = channel.session_id, streaming]() -> fan::event::task_t {
        co_await ecps_backend.send_stream_state(id, session, streaming);
      });
    }
    if (refresh) {
      stream_state_sent_at = now;
    }
  }

  std::string watch_preview(const std::vector<streamer_option_t>& options) const {
    std::string preview = options.empty() ? "Nobody is sharing" : "Pick who to watch";
    for (const auto& option : options) {
      if (watch_active && (std::uint32_t)option.channel.i == watch_channel && option.session == watch_source) {
        preview = "Watching " + option.name;
      }
    }
    return preview;
  }

  // The top right of the stream area: the Stats tick and whose screen is watched. It fades out like the toolbar and
  // comes back when the mouse goes near the top edge, or while there is no picture
  void render_stream_top_overlay() {
    // with nobody sharing there is nobody to pick
    auto options = streamer_options();
    if (options.empty()) {
      top_overlay_alpha = 0.f;
      return;
    }
    auto& io = gui::get_io();
    fan::vec2 area_pos = gui::get_window_pos();
    fan::vec2 area_size = gui::get_window_size();
    f32_t frame_height = gui::get_frame_height();
    bool mouse_near = io.MousePos.x >= area_pos.x && io.MousePos.x <= area_pos.x + area_size.x &&
      io.MousePos.y >= area_pos.y && io.MousePos.y <= area_pos.y + frame_height * 3.f;
    auto* rt = get_render_thread();
    bool has_picture = rt && rt->network_frame.get_image() != engine.default_texture;
    auto now = std::chrono::steady_clock::now();
    if (mouse_near || !has_picture) {
      top_overlay_visible_until = now + std::chrono::milliseconds(1500);
    }
    f32_t target = now < top_overlay_visible_until ? 1.f : 0.f;
    top_overlay_alpha += (target - top_overlay_alpha) * std::min(1.f, io.DeltaTime * 14.f);
    if (target == 0.f && top_overlay_alpha < 0.02f) {
      top_overlay_alpha = 0.f;
      return;
    }

    f32_t picker_width = std::max(180.f, gui::calc_text_size(watch_preview(options)).x + frame_height * 1.8f);
    gui::push_style_var(gui::style_var_alpha, std::max(top_overlay_alpha, 0.02f));
    gui::set_cursor_pos(fan::vec2(area_size.x - picker_width - 14.f, 10.f + stream_top_offset() + gui::get_scroll_y()));
    render_watch_picker(picker_width);
    gui::pop_style_var();
  }

  // who to watch
  void render_watch_picker(f32_t width) {
    auto options = streamer_options();
    std::string preview = watch_preview(options);
    gui::push_item_width(width);
    if (gui::begin_combo("##watch_picker", preview)) {
      for (const auto& option : options) {
        bool selected = watch_active && (std::uint32_t)option.channel.i == watch_channel && option.session == watch_source;
        if (gui::selectable(option.name + "##watch" + std::to_string(option.channel.i) + "_" + std::to_string(option.session), selected)) {
          set_watch(option.channel, option.session);
        }
      }
      gui::end_combo();
    }
    gui::pop_item_width();
  }

  bool watch_active = false;
  std::uint32_t watch_channel = 0;
  std::uint32_t watch_source = 0;
  std::unordered_map<std::uint32_t, bool> stream_state_sent;
  std::chrono::steady_clock::time_point stream_state_sent_at;

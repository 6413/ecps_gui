// Session implementation, out of line.
#include "gui_std.h" // precompiled, first
#include "ecps_app.h"

std::string render_thread_t::ecps_gui_t::username_notice_text() const {
    if (!username_notice.empty() && std::chrono::steady_clock::now() < username_notice_until) {
      return username_notice;
    }
    return {};
  }

void render_thread_t::ecps_gui_t::update_username() {
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

void render_thread_t::ecps_gui_t::render_username_popup() {
    if (username_popup_requested) {
      username_popup_requested = false;
      username_input = ecps_backend.get_current_username();
      username_error.clear();
      gui::set_next_window_pos(fan::vec2(owner->engine.window.get_size().x, owner->engine.window.get_size().y) / 2 - fan::vec2(190, 90));
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

void render_thread_t::ecps_gui_t::render_new_channel_popup() {
    if (new_channel_popup_requested) {
      new_channel_popup_requested = false;
      new_channel_popup_focus = true;
      gui::open_popup("New channel");
    }
    // the modal can be drawn outside the channel panel's push_theme scope; without this its body came out with the
    // default white popup background while the title bar was dark
    fan::color shown_bg = theme_background_display();
    gui::push_style_color(gui::col_popup_bg, fan::color(
      std::pow(shown_bg.r, 2.2f) * 2.f, std::pow(shown_bg.g, 2.2f) * 2.f, std::pow(shown_bg.b, 2.2f) * 2.f, 0.98f));
    gui::push_style_color(gui::col_text, fan::color(0.9f, 0.9f, 0.9f, 1.f));
    gui::push_style_color(gui::col_text_disabled, fan::color(0.6f, 0.6f, 0.65f, 1.f));
    if (!gui::begin_popup_modal("New channel", gui::window_flags_always_auto_resize | gui::window_flags_no_saved_settings)) {
      gui::pop_style_color(3);
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
        ecps_backend.own_channel_title = ecps_backend_t::trim_password(name).substr(0, 31);
        auto created = co_await ecps_expected(ecps_backend.channel_create());
        if (!created) {
          fan::print("Failed to create channel: " + created.error());
          co_return;
        }
        auto joined = co_await ecps_expected(ecps_backend.channel_join(*created, true));
        if (!joined) {
          fan::print("Failed to join the new channel: " + joined.error());
          co_return;
        }
        if (auto listed = co_await ecps_expected(ecps_backend.request_channel_list()); !listed) {
          fan::print("Failed to refresh the channel list: " + listed.error());
          co_return;
        }
        if (auto sessions = co_await ecps_expected(ecps_backend.request_channel_session_list(*created)); !sessions) {
          fan::print("Failed to load the channel's users: " + sessions.error());
          co_return;
        }
        selected_channel_id = *created;
      });
      gui::close_current_popup();
    }
    gui::same_line();
    if (gui::button("Cancel")) {
      gui::close_current_popup();
    }
    gui::end_popup();
    gui::pop_style_color(3);
  }

void render_thread_t::ecps_gui_t::render_password_popup() {
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
      gui::set_next_window_pos(fan::vec2(owner->engine.window.get_size().x, owner->engine.window.get_size().y) / 2 - fan::vec2(190, 80));
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
        auto joined = co_await ecps_expected(ecps_backend.channel_join(id, false, password));
        if (!joined) {
          fan::print("Failed to join channel: " + joined.error());
          co_return;
        }
        if (auto listed = co_await ecps_expected(ecps_backend.request_channel_list()); !listed) {
          fan::print("Failed to refresh the channel list: " + listed.error());
          co_return;
        }
        if (auto sessions = co_await ecps_expected(ecps_backend.request_channel_session_list(id)); !sessions) {
          fan::print("Failed to load the channel's users: " + sessions.error());
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

void render_thread_t::ecps_gui_t::render_channel_password_controls(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    std::string current_name = ecps_backend.channel_name_of(channel_id);
    if (current_name.empty()) {
      current_name = ecps_backend.own_channel_title;
    }
    // the channel's name can change without this box (another session renamed it): show what it is now, except while
    // it is being typed in
    bool channel_changed = channel_manage_rename_channel.i != channel_id.i;
    bool name_changed = !current_name.empty() && current_name != channel_manage_rename_shown;
    if (!channel_manage_rename_active && (channel_changed || name_changed)) {
      channel_manage_rename_input = current_name;
      channel_manage_rename_channel = channel_id;
      channel_manage_rename_shown = current_name;
    }
    settings_section_icon(icon_e::users, "NAME");
    gui::push_item_width(-1.f);
    bool rename_enter = gui::input_text("##channel_rename", &channel_manage_rename_input, gui::input_text_flags_enter_returns_true);
    gui::pop_item_width();
    channel_manage_rename_active = gui::is_item_active();
    if (gui::button("Rename") || rename_enter) {
      std::string name = channel_manage_rename_input;
      write_to_config("channel_title", name);
      // the list shows the trimmed, shortened name the backend sends; do not let the old one replace the box while
      // the rename is on its way
      std::string sent = ecps_backend_t::trim_password(name);
      if (sent.size() > 31) {
        sent.resize(31);
      }
      channel_manage_rename_shown = sent;
      backend_queue([channel_id, name]() -> fan::event::task_t {
        ecps_backend.channel_rename(channel_id, name);
        co_return;
      });
    }
    settings_section_icon(icon_e::lock, "PASSWORD");
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
      std::string current = ecps_backend.saved_password(channel_id);
      if (!current.empty()) {
        gui::same_line();
        gui::text_wrapped_disabled("Current: " + (channel_password_show ? current : std::string(current.size(), '*')));
      }
    }
    gui::begin_disabled(channel_password_input.empty());
    // one under the other, each as wide as the panel: side by side they didn't fit a narrow one
    f32_t button_width = gui::get_content_region_avail().x;
    if (gui::button(has_password ? "Change password" : "Set password", fan::vec2(button_width, 0))) {
      backend_queue([channel_id, password = channel_password_input]() -> fan::event::task_t {
        if (auto set = co_await ecps_expected(ecps_backend.channel_set_password(channel_id, password)); !set) {
          fan::print("Failed to set the channel password: " + set.error());
        }
      });
      channel_password_input.clear();
    }
    gui::end_disabled();
    if (has_password) {
      if (gui::button("Remove password", fan::vec2(button_width, 0))) {
        backend_queue([channel_id]() -> fan::event::task_t {
          if (auto removed = co_await ecps_expected(ecps_backend.channel_set_password(channel_id, std::string())); !removed) {
            fan::print("Failed to remove the channel password: " + removed.error());
          }
        });
      }
    }
  }

void render_thread_t::ecps_gui_t::render_delete_channel_button(ecps_backend_t::Protocol_ChannelID_t channel_id) {
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
          if (auto deleted = co_await ecps_expected(ecps_backend.delete_channel(channel_id)); !deleted) {
            fan::print("Failed to delete the channel: " + deleted.error());
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

void render_thread_t::ecps_gui_t::save_passwords_if_changed() {
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

void render_thread_t::ecps_gui_t::set_auto_join(const std::string& host_name) {
    auto_join_host = host_name;
    write_to_config("auto_join", host_name);
  }

void render_thread_t::ecps_gui_t::update_auto_join() {
    if (auto_join_host.empty() && resume_join.empty() && resume_select.empty() && !auto_host) {
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
        (void)co_await ecps_expected(ecps_backend.request_channel_list());
      });
    }

    // Our own channel, "Open automatically": once the server's list is here, joined back when it still has it (it
    // keeps a channel its host left), made again when it doesn't. Not while an update's own resume does the same
    if (auto_host && ecps_backend.channel_list_received && !(resume_share.active && resume_share.own) && now >= auto_host_next_try) {
      bool have_own = false;
      for (const auto& channel : ecps_backend.channel_info) {
        have_own |= ecps_backend.is_hosting_channel(channel.channel_id);
      }
      if (!have_own) {
        auto_host_next_try = now + std::chrono::seconds(6);
        std::string own_name = ecps_backend.own_channel_title.empty() ? ecps_backend.get_current_username() : ecps_backend.own_channel_title;
        const ecps_backend_t::channel_list_info_t* kept = nullptr;
        for (const auto& channel : ecps_backend.available_channels) {
          bool someone_hosts = channel.host_session_id.i != decltype(channel.host_session_id.i)(-1);
          if (channel.name == own_name && !someone_hosts) {
            kept = &channel;
          }
        }
        if (kept) {
          backend_queue([this, id = kept->channel_id]() -> fan::event::task_t {
            if (auto joined = co_await ecps_expected(ecps_backend.channel_join(id, true)); !joined) {
              co_return;
            }
            (void)co_await ecps_expected(ecps_backend.request_channel_list());
            (void)co_await ecps_expected(ecps_backend.request_channel_session_list(id));
            selected_channel_id = id;
          });
        }
        else {
          backend_queue([this]() -> fan::event::task_t {
            auto created = co_await ecps_expected(ecps_backend.channel_create());
            if (!created) {
              fan::print("Could not open your channel: " + created.error());
              co_return;
            }
            if (auto joined = co_await ecps_expected(ecps_backend.channel_join(*created, true)); !joined) {
              co_return;
            }
            (void)co_await ecps_expected(ecps_backend.request_channel_list());
            (void)co_await ecps_expected(ecps_backend.request_channel_session_list(*created));
            selected_channel_id = *created;
          });
        }
      }
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
        if (auto joined = co_await ecps_expected(ecps_backend.channel_join(id)); !joined) {
          co_return;
        }
        if (auto listed = co_await ecps_expected(ecps_backend.request_channel_list()); !listed) {
          co_return;
        }
        (void)co_await ecps_expected(ecps_backend.request_channel_session_list(id));
      });
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

std::string render_thread_t::ecps_gui_t::channel_name_of(ecps_backend_t::Protocol_ChannelID_t channel_id) const {
    for (const auto& channel : ecps_backend.available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        return channel.name;
      }
    }
    return {};
  }

std::string render_thread_t::ecps_gui_t::collect_resume_state() {
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
#if defined(_WIN32)
    // what was picked to share (a window or a monitor), also when not sharing right now
    if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
      fan::json capture = fan::json::object();
      capture["window"] = (std::uint64_t)rt->screen_encoder.mdscr.target_window.load();
      capture["monitor"] = (std::uint64_t)rt->screen_encoder.mdscr.target_monitor.load();
      capture["title"] = capture_window_title;
      state["capture"] = capture;
    }
#endif
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

void render_thread_t::ecps_gui_t::load_resume_state() {
    if (!fan::io::file::exists(updater_t::state_file)) {
      return;
    }
    std::string data;
    fan::io::file::read(std::string(updater_t::state_file), &data);
    std::error_code ec;
    std::filesystem::remove(updater_t::state_file, ec);
    auto state_result = ecps_parse_json(data);
    if (!state_result) {
      return;
    }
    fan::json state = std::move(*state_result);
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
      if (state.contains("capture") && state["capture"].is_object()) {
        fan::json capture = state["capture"];
        resume_capture.window = (std::uintptr_t)capture.value("window", (std::uint64_t)0);
        resume_capture.monitor = (std::uintptr_t)capture.value("monitor", (std::uint64_t)0);
        resume_capture.title = capture.value("title", std::string());
        resume_capture.pending = resume_capture.window != 0 || resume_capture.monitor != 0;
      }
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

void render_thread_t::ecps_gui_t::update_resume_capture() {
    if (!resume_capture.pending) {
      return;
    }
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (!rt) {
      return;
    }
    resume_capture.pending = false;
#if defined(_WIN32)
    auto& capture = rt->screen_encoder.mdscr;
    // only if it is still there: a closed window or an unplugged monitor leaves the main display
    if (resume_capture.monitor) {
      refresh_capture_monitors();
      bool found = std::any_of(capture_monitors.begin(), capture_monitors.end(),
        [&](const auto& monitor) { return monitor.handle == resume_capture.monitor; });
      if (found) {
        capture.target_monitor = resume_capture.monitor;
      }
    }
    if (capture_window_open(resume_capture.window)) {
      capture_window_title = resume_capture.title;
      capture.target_window = resume_capture.window;
    }
    fan::print("share source after the update: " + std::string(capture.target_window.load() ? "window " + capture_window_title
      : capture.target_monitor.load() ? "a monitor" : "the main display"));
#endif
  }

void render_thread_t::ecps_gui_t::update_resume_share() {
    update_resume_capture();
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
        if (auto joined = co_await ecps_expected(ecps_backend.channel_join(id)); !joined) {
          co_return;
        }
        if (auto listed = co_await ecps_expected(ecps_backend.request_channel_list()); !listed) {
          co_return;
        }
        (void)co_await ecps_expected(ecps_backend.request_channel_session_list(id));
      });
    }
  }

void render_thread_t::ecps_gui_t::save_stream_settings_now() {
    stream_snapshot_t current = current_stream_settings();
    fan::json stream = fan::json::object();
    stream["width"] = current.width;
    stream["height"] = current.height;
    stream["fps"] = current.fps;
    stream["bitrate_mode"] = current.bitrate_mode;
    stream["bitrate_mbps"] = current.bitrate_mbps;
    stream["scaling_quality"] = current.scaling_quality;
    stream["preset"] = current.preset;
    // by name: the list's order can change between versions
    stream["preset_name"] = stream_presets[std::clamp(current.preset, 0, stream_preset_count - 1)].name;
    write_to_config("stream", stream);
    saved_stream = current;
    seen_stream = current;
    stream_changed_at = std::chrono::steady_clock::time_point{};
  }

void render_thread_t::ecps_gui_t::update_stream_settings_persistence() {
    auto* rt = get_render_thread();
    if (!rt || !rt->screen_encoder.is_initialized()) {
      return;
    }
    auto& encoder = rt->screen_encoder;
    auto now = std::chrono::steady_clock::now();

    if (!stream_applied) {
      stream_applied = true;
      stream_settings.preset = std::clamp(saved_stream.preset, 0, stream_preset_count - 1);
      // the codec side comes from ui, stream keeps the rest: a saved preset only stays if the two still agree
      if (stream_settings.preset > 0) {
        const auto& preset = stream_presets[stream_settings.preset];
        if (video_codec != preset.codec || sharp_text != preset.sharp_text || lossless != preset.lossless) {
          stream_settings.preset = 0;
        }
      }
      if (saved_stream.valid) {
        if (saved_stream.width >= 160 && saved_stream.height >= 120 && saved_stream.width <= 7680 && saved_stream.height <= 4320) {
          encoder.set_user_resolution((std::uint32_t)saved_stream.width, (std::uint32_t)saved_stream.height);
        }
        if (saved_stream.fps >= 3 && saved_stream.fps <= 244) {
          stream_settings.framerate = saved_stream.fps;
          encoder.config_.frame_rate = saved_stream.fps;
          std::lock_guard<std::mutex> lock(encoder.mutex);
          encoder.update_flags |= codec_update_e::frame_rate;
        }
        if (saved_stream.bitrate_mode == 0 || saved_stream.bitrate_mode == 1) {
          stream_settings.bitrate_mode = saved_stream.bitrate_mode;
          stream_settings.use_adaptive_bitrate = saved_stream.bitrate_mode == 0;
          if (!lossless && saved_stream.bitrate_mode == 1 && saved_stream.bitrate_mbps >= 0.5f && saved_stream.bitrate_mbps <= 50.f) {
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
      // the codec side is kept in the ui settings, not in stream: load it from there, and compare the hand changes
      // against what is actually on screen
      saved_stream.preset = stream_settings.preset;
      saved_stream.codec = video_codec;
      saved_stream.sharp_text = sharp_text;
      saved_stream.lossless = lossless;
      seen_stream = current_stream_settings();
      return;
    }

    // a preset in use follows the shared source's shape: not a hand change, so it stays the preset
    if (follow_preset_source()) {
      seen_stream = current_stream_settings();
      stream_changed_at = now;
      return;
    }

    stream_snapshot_t current = current_stream_settings();
    if (!current.same(seen_stream)) {
      // a row was changed by hand: no preset describes the result any more. The choice goes back to Custom at once,
      // the values themselves are written after the usual second of quiet
      if (stream_settings.preset != 0) {
        stream_settings.preset = 0;
        current.preset = 0;
      }
      seen_stream = current;
      stream_changed_at = now;
      return;
    }
    if (stream_changed_at != std::chrono::steady_clock::time_point{} && now - stream_changed_at > std::chrono::seconds(1) &&
      (!saved_stream.valid || !current.same(saved_stream))) {
      save_stream_settings_now();
    }
  }

std::string render_thread_t::ecps_gui_t::session_label(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t session) const {
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

void render_thread_t::ecps_gui_t::set_watch(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t source) {
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

void render_thread_t::ecps_gui_t::update_stale_picture() {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    std::uint64_t decoded = ecps_backend.view_frames_decoded.load();
    if (decoded != picture_frames_seen) {
      picture_frames_seen = decoded;
      picture_last_frame_at = now;
      picture_stalled = false;
      return;
    }
    if (rt->network_frame.get_image() == owner->engine.default_texture) {
      picture_stalled = false;
      return;
    }
    // the stream still on: the person watched still shares (or you, for your own preview)
    bool still_live = show_own_stream ? ecps_backend.is_streaming_to_any_channel() : watch_active;
    if (still_live && !show_own_stream) {
      auto sharing = ecps_backend.streamers_of(ecps_backend_t::Protocol_ChannelID_t((std::uint16_t)watch_channel));
      still_live = std::find(sharing.begin(), sharing.end(), watch_source) != sharing.end();
    }
    auto quiet = now - picture_last_frame_at;
    // A lag spike (here or at the sharer) keeps the last picture, frozen, instead of going black: it was cleared after
    // 2 s before, and the stream looked like it had stopped. Only a stream that ended, or nothing for 30 s, clears it
    if (!still_live || quiet > std::chrono::seconds(30)) {
      rt->network_frame.set_image(owner->engine.default_texture);
      picture_stalled = false;
      return;
    }
    picture_stalled = quiet > std::chrono::milliseconds(1500);
    // what comes after a gap refers to frames that never came: a keyframe to start from, asked again every 2 s
    if (picture_stalled && now - picture_stall_idr_at > std::chrono::seconds(2)) {
      picture_stall_idr_at = now;
      request_idr_reset();
    }
  }

void render_thread_t::ecps_gui_t::update_watch() {
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

void render_thread_t::ecps_gui_t::update_stream_state() {
    if (stream_source_lost.exchange(false)) {
      fan::print("share: the shared window closed, stopped sharing");
      stream_button_renderer_t::set_sharing(false);
    }
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

std::string render_thread_t::ecps_gui_t::watch_preview(const std::vector<streamer_option_t>& options) const {
    std::string preview = options.empty() ? "Nobody is sharing" : "Pick who to watch";
    for (const auto& option : options) {
      if (watch_active && (std::uint32_t)option.channel.i == watch_channel && option.session == watch_source) {
        preview = "Watching " + option.name;
      }
    }
    return preview;
  }

void render_thread_t::ecps_gui_t::render_stream_top_overlay() {
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
    bool has_picture = rt && rt->network_frame.get_image() != owner->engine.default_texture;
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

void render_thread_t::ecps_gui_t::render_watch_picker(f32_t width) {
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

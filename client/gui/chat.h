// Part of ecps_gui_t, included inside the struct by gui.h: the channel's chat.
// Text goes to everyone in the channel through the server, which keeps the last messages for those who join later.
// Files (the attach button, pasted files, files dropped while the chat is open, and text too long for a message)
// go to everyone in the channel as file offers, and a line in the chat says so

  std::string chat_input;
  std::uint64_t chat_seen_revision = 0;
  std::uint32_t chat_seen_channel = (std::uint32_t)-1;
  bool chat_input_active = false;
  bool chat_focus_input = false;
  std::chrono::steady_clock::time_point chat_tab_seen_at{};
  // files dropped on the window while the chat was showing, sent by the next frame of the chat
  std::vector<std::string> chat_dropped_files;

  // longer text than this goes as a .txt file, a message holds Protocol::ChatTextMax bytes
  static constexpr std::size_t chat_inline_max = 1000;

  bool chat_showing() const {
    return std::chrono::steady_clock::now() - chat_tab_seen_at < std::chrono::milliseconds(250);
  }

  static std::string chat_time_label(std::int64_t time) {
    std::time_t t = (std::time_t)time;
    std::tm local{};
  #if defined(_WIN32)
    localtime_s(&local, &t);
  #else
    localtime_r(&t, &local);
  #endif
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
    return buffer;
  }

  static std::string chat_size_label(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
      value /= 1024.0;
      ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buffer;
  }

  // files into the chat: offered to everyone else in the channel, one line in the chat for each
  void chat_send_files(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::vector<std::string>& paths) {
    auto peers = drop_peers(channel_id);
    for (const auto& path : paths) {
      std::error_code ec;
      if (!std::filesystem::is_regular_file(path, ec)) {
        continue;
      }
      for (const auto& peer : peers) {
        backend_queue([channel_id, peer = peer.session_id, path]() -> fan::event::task_t {
          return ecps_backend.send_file(channel_id, peer, path);
        });
      }
      std::string line = "[file] " + std::filesystem::path(path).filename().string() + " (" +
        chat_size_label(std::filesystem::file_size(path, ec)) + ")" + (peers.empty() ? "  - nobody else here to send it to" : "");
      backend_queue([channel_id, line]() -> fan::event::task_t {
        return ecps_backend.send_chat(channel_id, line);
      });
    }
  }

  // a message, or a .txt file when it is too long for one
  void chat_send_text(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::string& text) {
    if (text.empty()) {
      return;
    }
    if (text.size() <= chat_inline_max) {
      backend_queue([channel_id, text]() -> fan::event::task_t {
        return ecps_backend.send_chat(channel_id, text);
      });
      return;
    }
    std::error_code ec;
    auto folder = std::filesystem::temp_directory_path(ec) / "ecps";
    std::filesystem::create_directories(folder, ec);
    auto path = folder / ("message-" + std::to_string((long long)std::time(nullptr)) + ".txt");
    {
      std::ofstream out(path, std::ios::binary);
      out << text;
    }
    chat_send_files(channel_id, { path.string() });
  }

  // the files on the clipboard: Explorer's copied files on Windows, file:// lines or paths elsewhere
  std::vector<std::string> chat_clipboard_files() {
    std::vector<std::string> paths;
  #if defined(_WIN32)
    paths = clipboard_file_paths();
    if (!paths.empty()) {
      return paths;
    }
  #endif
    std::string text = engine.window.get_clipboard();
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      if (line.empty()) {
        continue;
      }
      if (line.rfind("file://", 0) == 0) {
        line = line.substr(7);
        // %20 and the like
        std::string decoded;
        for (std::size_t i = 0; i < line.size(); ++i) {
          if (line[i] == '%' && i + 2 < line.size() && std::isxdigit((unsigned char)line[i + 1]) && std::isxdigit((unsigned char)line[i + 2])) {
            decoded += (char)std::stoi(line.substr(i + 1, 2), nullptr, 16);
            i += 2;
          }
          else {
            decoded += line[i];
          }
        }
        line = decoded;
      }
      std::error_code ec;
      if (!std::filesystem::is_regular_file(line, ec)) {
        return {};
      }
      paths.push_back(line);
    }
    return paths;
  }

  void render_chat_tab(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    chat_tab_seen_at = std::chrono::steady_clock::now();
    if (!ecps_backend.chat_known(channel_id)) {
      text_wrapped_disabled("Connecting to the chat... If this stays, the server is too old for chat and needs updating.");
      return;
    }

    if (!chat_dropped_files.empty()) {
      chat_send_files(channel_id, chat_dropped_files);
      chat_dropped_files.clear();
    }

    auto messages = ecps_backend.chat_of(channel_id);
    f32_t input_height = gui::get_frame_height() + gui::get_style().ItemSpacing.y * 2;
    fan::vec2 avail = gui::get_content_region_avail();
    if (gui::begin_child("##chat_log", fan::vec2(0, std::max(avail.y - input_height, 80.f)), true)) {
      if (messages.empty()) {
        text_wrapped_disabled("No messages yet. Text, pasted text and files go to everyone in this channel.");
      }
      std::uint32_t last_from = (std::uint32_t)-1;
      std::int64_t last_time = 0;
      for (const auto& message : messages) {
        // a run of messages from the same person within a few minutes shares one name line
        if (message.from != last_from || message.time - last_time > 300) {
          gui::spacing();
          bool mine = message.from == (std::uint32_t)ecps_backend.session_id.i;
          gui::text(mine ? fan::color(0.55f, 0.8f, 1.f, 1.f) : fan::color(0.95f, 0.8f, 0.45f, 1.f), message.name);
          gui::same_line();
          text_wrapped_disabled(chat_time_label(message.time));
        }
        last_from = message.from;
        last_time = message.time;
        gui::text_wrapped(message.text);
        // right click copies the message
        if (gui::is_item_hovered() && fan::window::is_mouse_clicked(1)) {
          engine.window.set_clipboard(message.text);
        }
      }
      // new messages, or another channel opened: down to the newest
      std::uint64_t revision = ecps_backend.chat_revision.load();
      if (revision != chat_seen_revision || chat_seen_channel != (std::uint32_t)channel_id.i) {
        chat_seen_revision = revision;
        chat_seen_channel = (std::uint32_t)channel_id.i;
        gui::set_scroll_here_y();
      }
    }
    gui::end_child();

    // ---- the input: attach, text, send
    if (icon_button("##chat_attach", icon_e::send_file, "Send a file to everyone here", 0.f)) {
      fan::graphics::open_file("", [this, channel_id](std::string_view path) {
        chat_dropped_files.push_back(std::string(path));
      });
    }
    gui::same_line();
    f32_t send_width = gui::calc_text_size("Send").x + gui::get_style().FramePadding.x * 2;
    gui::push_item_width(gui::get_content_region_avail().x - send_width - gui::get_style().ItemSpacing.x);

    // Ctrl+V with files on the clipboard sends the files, the text field doesn't get their paths
    bool ctrl = engine.window.is_key_down(fan::key_left_control) || engine.window.is_key_down(fan::key_right_control);
    bool pasting = chat_input_active && ctrl && engine.window.is_key_clicked(fan::key_v);
    std::string before = chat_input;
    std::vector<std::string> pasted_files = pasting ? chat_clipboard_files() : std::vector<std::string>{};

    if (chat_focus_input) {
      gui::set_keyboard_focus_here();
      chat_focus_input = false;
    }
    bool enter = gui::input_text("##chat_input", &chat_input, gui::input_text_flags_enter_returns_true);
    chat_input_active = gui::is_item_active();
    gui::pop_item_width();
    if (gui::is_item_hovered() && chat_input.empty()) {
      gui::set_tooltip("Enter sends. Long text and pasted files go as files");
    }

    if (!pasted_files.empty()) {
      chat_input = before;
      chat_send_files(channel_id, pasted_files);
    }
    // pasted text too long for a message goes as a file
    else if (chat_input.size() > chat_inline_max) {
      chat_send_text(channel_id, chat_input);
      chat_input.clear();
    }

    gui::same_line();
    bool send = gui::button("Send##chat_send");
    if ((enter || send) && !chat_input.empty()) {
      chat_send_text(channel_id, chat_input);
      chat_input.clear();
      chat_focus_input = true;
    }
  }

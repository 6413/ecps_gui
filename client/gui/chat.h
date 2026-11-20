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
  // the whole chat as one plain text field, to mark and copy any part of it
  bool chat_select_mode = false;
  std::string chat_select_text;

  // a message's text: plain parts wrapped, ```code``` in a box of its own (monospace, not wrapped) with a Copy button.
  // A first line like ```cpp names the language and is not shown
  // Text that can be marked with the mouse: a read-only text field without a frame. Text fields don't wrap, so the
  // text is wrapped here for showing, and Ctrl+C copies the original (without those line breaks)
  struct chat_wrapped_t {
    f32_t width = -1.f;
    std::string source;
    std::string shown;
    std::vector<std::size_t> soft_breaks; // positions in shown of the line breaks added here
    std::size_t lines = 1;
  };
  std::unordered_map<std::string, chat_wrapped_t> chat_wrap_cache;
  struct chat_selection_t {
    int start = 0;
    int end = 0;
  };
  std::unordered_map<std::string, chat_selection_t> chat_selections;

  static void chat_wrap(chat_wrapped_t& wrapped, const std::string& text, f32_t width) {
    wrapped.width = width;
    wrapped.source = text;
    wrapped.shown.clear();
    wrapped.soft_breaks.clear();
    wrapped.lines = 1;
    std::string line;
    auto fits = [&](const std::string& candidate) { return gui::calc_text_size(candidate).x <= width; };
    auto soft_break = [&]() {
      wrapped.shown += line;
      wrapped.soft_breaks.push_back(wrapped.shown.size());
      wrapped.shown += '\n';
      ++wrapped.lines;
      line.clear();
    };
    std::size_t at = 0;
    while (at < text.size()) {
      if (text[at] == '\n') {
        wrapped.shown += line + '\n';
        ++wrapped.lines;
        line.clear();
        ++at;
        continue;
      }
      // the next word with the spaces after it
      std::size_t end = at;
      while (end < text.size() && text[end] != ' ' && text[end] != '\n') {
        ++end;
      }
      while (end < text.size() && text[end] == ' ') {
        ++end;
      }
      std::string word = text.substr(at, end - at);
      if (!line.empty() && !fits(line + word)) {
        soft_break();
      }
      // a word longer than the line is cut where it no longer fits
      while (!fits(word) && word.size() > 1) {
        std::size_t cut = word.size() - 1;
        while (cut > 1 && !fits(line + word.substr(0, cut))) {
          --cut;
        }
        // not inside a UTF-8 character
        while (cut > 1 && ((unsigned char)word[cut] & 0xC0) == 0x80) {
          --cut;
        }
        line += word.substr(0, cut);
        word = word.substr(cut);
        soft_break();
      }
      line += word;
      at = end;
    }
    wrapped.shown += line;
  }

  // a position in the shown text to the same place in the original
  static std::size_t chat_unwrap_position(const chat_wrapped_t& wrapped, std::size_t shown_position) {
    std::size_t added = 0;
    for (std::size_t at : wrapped.soft_breaks) {
      if (at < shown_position) {
        ++added;
      }
    }
    return shown_position - added;
  }

  void render_selectable_text(const std::string& text, const std::string& id, const std::string& whole_message) {
    f32_t width = std::max(gui::get_content_region_avail().x - 4.f, 50.f);
    auto& wrapped = chat_wrap_cache[id];
    if (wrapped.width != width || wrapped.source != text) {
      chat_wrap(wrapped, text, width);
    }
    auto& selection = chat_selections[id];
    gui::input_text_callback_t remember_selection = [](auto* data) -> int {
      auto* out = (chat_selection_t*)data->UserData;
      out->start = data->SelectionStart;
      out->end = data->SelectionEnd;
      return 0;
    };
    gui::push_style_color(gui::col_frame_bg, fan::color(0.f, 0.f, 0.f, 0.f));
    gui::push_style_var(gui::style_var_frame_padding, fan::vec2(0.f, 0.f));
    f32_t height = wrapped.lines * gui::get_text_line_height() + 2.f;
    gui::input_text_multiline("##chat_text" + id, &wrapped.shown, fan::vec2(width + 4.f, height),
      gui::input_text_flags_read_only | gui::input_text_flags_no_horizontal_scroll | gui::input_text_flags_callback_always,
      remember_selection, &selection);
    bool active = gui::is_item_active();
    bool hovered = gui::is_item_hovered();
    gui::pop_style_var();
    gui::pop_style_color();
    // the field copied what it shows, with the line breaks added for wrapping: the original replaces it
    bool ctrl = engine.window.is_key_down(fan::key_left_control) || engine.window.is_key_down(fan::key_right_control);
    if (active && ctrl && engine.window.is_key_clicked(fan::key_c) && selection.start != selection.end) {
      std::size_t a = chat_unwrap_position(wrapped, (std::size_t)std::min(selection.start, selection.end));
      std::size_t b = chat_unwrap_position(wrapped, (std::size_t)std::max(selection.start, selection.end));
      if (a < text.size()) {
        engine.window.set_clipboard(text.substr(a, std::min(b, text.size()) - a));
      }
    }
    // right click copies the whole message
    if (hovered && fan::window::is_mouse_clicked(1)) {
      engine.window.set_clipboard(whole_message);
    }
  }

  void render_chat_text(const std::string& text, std::uint32_t message_id) {
    int plain_part = 0;
    auto plain = [&](std::string part) {
      while (!part.empty() && (part.back() == '\n' || part.back() == '\r')) {
        part.pop_back();
      }
      std::size_t first = part.find_first_not_of("\r\n");
      if (first == std::string::npos) {
        return;
      }
      part = part.substr(first);
      render_selectable_text(part, std::to_string(message_id) + "_t" + std::to_string(plain_part++), text);
    };
    std::size_t at = 0;
    int block = 0;
    while (at < text.size()) {
      std::size_t open = text.find("```", at);
      if (open == std::string::npos) {
        plain(text.substr(at));
        break;
      }
      plain(text.substr(at, open - at));
      std::size_t close = text.find("```", open + 3);
      std::size_t code_end = close == std::string::npos ? text.size() : close;
      std::string code = text.substr(open + 3, code_end - open - 3);
      // ```cpp: the language, one word on the first line
      std::size_t first_break = code.find('\n');
      if (first_break != std::string::npos && code.substr(0, first_break).find_first_of(" \t") == std::string::npos) {
        code = code.substr(first_break + 1);
      }
      while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) {
        code.pop_back();
      }
      if (code.find_first_not_of(" \t\r\n") == std::string::npos) {
        at = close == std::string::npos ? text.size() : close + 3;
        continue;
      }
      std::size_t lines = 1 + (std::size_t)std::count(code.begin(), code.end(), '\n');
      std::string id = std::to_string(message_id) + "_" + std::to_string(block++);
      {
        gui::font_scope_t mono(gui::get_font_size(), gui::font::mono);
        f32_t height = lines * gui::get_text_line_height_with_spacing() + gui::get_style().WindowPadding.y * 2 +
          gui::get_style().ScrollbarSize;
        gui::push_style_color(gui::col_child_bg, fan::color(0.f, 0.f, 0.f, 0.35f));
        if (gui::begin_child("##chat_code" + id, fan::vec2(0, std::min(height, 400.f)), 1, gui::window_flags_horizontal_scrollbar)) {
          gui::text(code);
        }
        gui::end_child();
        gui::pop_style_color();
      }
      if (gui::button("Copy##chat_code_copy" + id)) {
        engine.window.set_clipboard(code);
      }
      at = close == std::string::npos ? text.size() : close + 3;
    }
  }

  // ---- the side panel of the stream view: the settings or the chat, picked by the two icons under the stream
  bool side_panel_chat = false;

  // something came in that the chat hasn't shown yet
  bool chat_unread() const {
    return !chat_showing() && ecps_backend.chat_revision.load() != chat_seen_revision &&
      ecps_backend.chat_revision.load() != ecps_backend.chat_history_revision.load() &&
      ecps_backend.chat_known(selected_channel_id);
  }

  // the gear and the chat icon: one opens its panel, the same again closes it, the other one switches to its own
  void toggle_side_panel(bool chat) {
    if (stream_settings.show_in_stream_view && side_panel_chat == chat) {
      stream_settings.show_in_stream_view = false;
      return;
    }
    stream_settings.show_in_stream_view = true;
    side_panel_chat = chat;
    if (chat) {
      chat_focus_input = true;
    }
  }

  // the chat icon of the stream toolbars, with a dot while there is something unread
  void render_side_chat_button(const char* id, f32_t size) {
    bool open = stream_settings.show_in_stream_view && side_panel_chat;
    fan::vec2 at = gui::get_cursor_screen_pos();
    if (icon_button(id, icon_e::chat, open ? "Hide chat" : "Chat", size, icon_style_e::overlay)) {
      toggle_side_panel(true);
    }
    if (chat_unread()) {
      gui::get_window_draw_list()->AddCircleFilled(at + fan::vec2(size * 0.86f, size * 0.14f), size * 0.16f, 0xff4040ffu, 16);
    }
  }

  // the gear of the stream toolbars
  void render_side_settings_button(const char* id, f32_t size) {
    bool open = stream_settings.show_in_stream_view && !side_panel_chat;
    if (icon_button(id, icon_e::gear, open ? "Hide settings" : "Settings", size, icon_style_e::overlay)) {
      toggle_side_panel(false);
    }
  }

  // two tabs at the top of the side panel, Settings and Chat, with an X that closes it
  void render_side_panel_header() {
    f32_t height = gui::get_frame_height();
    f32_t close_width = height;
    f32_t width = (gui::get_content_region_avail().x - close_width - 8.f) * 0.5f;
    auto tab = [&](const char* id, icon_e icon, const char* text, bool selected, bool dot) {
      fan::vec2 at = gui::get_cursor_screen_pos();
      bool clicked = gui::invisible_button(id, fan::vec2(width, height));
      bool hovered = gui::is_item_hovered();
      auto* list = gui::get_window_draw_list();
      std::uint32_t fill = selected ? 0xff3a170cu : hovered ? 0xff1c0f09u : 0x00000000u;
      list->AddRectFilled(at, at + fan::vec2(width, height), fill, height * 0.3f);
      if (selected) {
        list->AddRectFilled(at + fan::vec2(6.f, height - 2.f), at + fan::vec2(width - 6.f, height), 0xffff7a33u, 1.f);
      }
      f32_t icon_size = height * 0.55f;
      f32_t content = icon_size + 6.f + gui::calc_text_size(text).x;
      fan::vec2 start = at + fan::vec2((width - content) * 0.5f, 0);
      std::uint32_t ink = selected || hovered ? 0xffffffffu : 0xffa0a0a0u;
      draw_icon(list, icon, start + fan::vec2(icon_size * 0.5f, height * 0.5f), icon_size, ink);
      list->AddText(start + fan::vec2(icon_size + 6.f, (height - gui::get_font_size()) * 0.5f), ink, text);
      if (dot) {
        list->AddCircleFilled(start + fan::vec2(content + 7.f, height * 0.3f), 3.5f, 0xff4040ffu, 12);
      }
      return clicked;
    };
    if (tab("##side_tab_settings", icon_e::gear, "Settings", !side_panel_chat, false)) {
      side_panel_chat = false;
    }
    gui::same_line(0, 4.f);
    if (tab("##side_tab_chat", icon_e::chat, "Chat", side_panel_chat, chat_unread())) {
      side_panel_chat = true;
      chat_focus_input = true;
    }
    gui::same_line(0, 4.f);
    if (gui::button("x##side_panel_close", fan::vec2(close_width, height))) {
      stream_settings.show_in_stream_view = false;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Close the panel");
    }
    gui::spacing();
  }

  // what the side panel shows under its tabs
  void render_side_panel_content() {
    render_side_panel_header();
    if (side_panel_chat) {
      if (ecps_backend.channel_info.empty()) {
        text_wrapped_disabled("Join a channel to chat");
      }
      else {
        render_chat_tab(selected_channel_id);
      }
    }
    else {
      window_handler.render_stream_settings_content_compact();
    }
  }

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
    // an image goes as a picture file
    if (std::string image = clipboard_image_file(); !image.empty()) {
      return { image };
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
    gui::checkbox("Select text##chat_select", &chat_select_mode);
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Shows the chat as plain text: mark any part of it and copy it with Ctrl+C");
    }
    // the input grows with its lines, up to six
    std::size_t input_lines = std::clamp<std::size_t>(1 + (std::size_t)std::count(chat_input.begin(), chat_input.end(), '\n'), 1, 6);
    f32_t input_box = input_lines * gui::get_text_line_height() + gui::get_style().FramePadding.y * 2;
    f32_t input_height = input_box + gui::get_style().ItemSpacing.y * 2;
    fan::vec2 avail = gui::get_content_region_avail();
    if (chat_select_mode) {
      chat_select_text.clear();
      for (const auto& message : messages) {
        chat_select_text += message.name + "  " + chat_time_label(message.time) + "\n" + message.text + "\n\n";
      }
      gui::input_text_multiline("##chat_select_text", &chat_select_text, fan::vec2(-1, std::max(avail.y - input_height, 80.f)),
        gui::input_text_flags_read_only);
    }
    else if (gui::begin_child("##chat_log", fan::vec2(0, std::max(avail.y - input_height, 80.f)), true)) {
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
        render_chat_text(message.text, message.id);
      }
      // new messages, or another channel opened: down to the newest
      std::uint64_t revision = ecps_backend.chat_revision.load();
      if (revision != chat_seen_revision || chat_seen_channel != (std::uint32_t)channel_id.i) {
        chat_seen_revision = revision;
        chat_seen_channel = (std::uint32_t)channel_id.i;
        gui::set_scroll_here_y();
      }
    }
    if (!chat_select_mode) {
      gui::end_child();
    }

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
    // Enter sends, Shift+Enter is a new line: with Shift held the field takes Enter as a line of its own
    bool shift = engine.window.is_key_down(fan::key_left_shift) || engine.window.is_key_down(fan::key_right_shift);
    gui::input_text_flags_t input_flags = shift ? 0 :
      (gui::input_text_flags_enter_returns_true | gui::input_text_flags_ctrl_enter_for_new_line);
    // with Shift held the field returns true for any change (Enter-returns-true is off then): only without it is it a send
    bool enter = gui::input_text_multiline("##chat_input", &chat_input, fan::vec2(0, input_box), input_flags) && !shift;
    chat_input_active = gui::is_item_active();
    gui::pop_item_width();
    if (gui::is_item_hovered() && chat_input.empty()) {
      gui::set_tooltip("Enter sends, Shift+Enter starts a new line. ```code``` shows as code. Long text and pasted files go as files");
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

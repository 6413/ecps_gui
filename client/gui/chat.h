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

  // Text is wrapped here (chat_wrap) and drawn line by line, every line kept with where it is, so a mark can run
  // across lines and messages like on a web page
  struct chat_wrapped_t {
    f32_t width = -1.f;
    std::string source;
    std::string shown;
    std::vector<std::size_t> soft_breaks; // positions in shown of the line breaks added here
    std::size_t lines = 1;
  };
  std::unordered_map<std::string, chat_wrapped_t> chat_wrap_cache;

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

  // ---- marking with the mouse: a press in the log starts a mark, dragging moves its end, Ctrl+C or a right click
  // copies it. Names, code blocks and pictures are marked whole or not at all
  struct chat_line_t {
    std::uint64_t key = 0; // the message's id and the line's number in it: finds the line again next frame
    std::uint32_t message_id = 0;
    fan::vec2 min = 0;
    fan::vec2 max = 0;
    std::string text;
    std::string after; // what follows it when copied: a line break, or nothing after a line that was only wrapped
    bool whole = false;
  };
  std::vector<chat_line_t> chat_lines;
  std::uint32_t chat_line_message = 0;
  std::uint32_t chat_line_number = 0;
  struct chat_mark_t {
    std::uint64_t key = 0;
    std::size_t at = 0;
  };
  chat_mark_t chat_mark_from;
  chat_mark_t chat_mark_to;
  bool chat_marking = false;
  bool chat_has_mark = false;

  void chat_add_line(std::string text, std::string after, bool whole, fan::vec2 min, fan::vec2 max) {
    chat_line_t line;
    line.message_id = chat_line_message;
    line.key = ((std::uint64_t)chat_line_message << 20) | chat_line_number++;
    line.min = min;
    line.max = max;
    line.text = std::move(text);
    line.after = std::move(after);
    line.whole = whole;
    chat_lines.push_back(std::move(line));
  }

  // a plain part of a message, wrapped to the width, one kept line each
  void chat_plain_lines(const std::string& text, const std::string& id) {
    f32_t width = std::max(gui::get_content_region_avail().x - 4.f, 50.f);
    auto& wrapped = chat_wrap_cache[id];
    if (wrapped.width != width || wrapped.source != text) {
      chat_wrap(wrapped, text, width);
    }
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(gui::get_style().ItemSpacing.x, 1.f));
    std::size_t begin = 0;
    while (true) {
      std::size_t stop = wrapped.shown.find('\n', begin);
      bool soft = stop != std::string::npos &&
        std::find(wrapped.soft_breaks.begin(), wrapped.soft_breaks.end(), stop) != wrapped.soft_breaks.end();
      std::string line = wrapped.shown.substr(begin, stop == std::string::npos ? std::string::npos : stop - begin);
      gui::text(line.empty() ? std::string(" ") : line);
      chat_add_line(line, soft ? "" : "\n", false, gui::get_item_rect_min(), gui::get_item_rect_max());
      if (stop == std::string::npos) {
        break;
      }
      begin = stop + 1;
    }
    gui::pop_style_var();
  }

  int chat_line_index(std::uint64_t key) const {
    for (std::size_t i = 0; i < chat_lines.size(); ++i) {
      if (chat_lines[i].key == key) {
        return (int)i;
      }
    }
    return -1;
  }

  static f32_t chat_prefix_width(const chat_line_t& line, std::size_t at) {
    if (at == 0) {
      return 0.f;
    }
    if (line.whole || at >= line.text.size()) {
      return line.max.x - line.min.x;
    }
    return gui::calc_text_size(line.text.substr(0, at)).x;
  }

  // the place in the text under the mouse: above all lines the start, below them the end
  chat_mark_t chat_hit(fan::vec2 m) const {
    if (chat_lines.empty()) {
      return {};
    }
    if (m.y < chat_lines.front().min.y) {
      return {chat_lines.front().key, 0};
    }
    for (std::size_t i = 0; i < chat_lines.size(); ++i) {
      const auto& line = chat_lines[i];
      if (i + 1 < chat_lines.size() && m.y >= chat_lines[i + 1].min.y) {
        continue;
      }
      if (m.y > line.max.y) {
        return {line.key, line.text.size()};
      }
      if (line.whole) {
        return {line.key, m.x < (line.min.x + line.max.x) * 0.5f ? 0 : line.text.size()};
      }
      f32_t x = m.x - line.min.x;
      std::size_t previous = 0;
      f32_t previous_width = 0.f;
      while (previous < line.text.size()) {
        std::size_t next = previous + 1;
        while (next < line.text.size() && ((unsigned char)line.text[next] & 0xC0) == 0x80) {
          ++next;
        }
        f32_t width = gui::calc_text_size(line.text.substr(0, next)).x;
        if (x < (previous_width + width) * 0.5f) {
          return {line.key, previous};
        }
        previous = next;
        previous_width = width;
      }
      return {line.key, line.text.size()};
    }
    return {chat_lines.back().key, chat_lines.back().text.size()};
  }

  // the marked part, first to last; false when nothing is marked
  bool chat_mark_range(int& a, std::size_t& a_at, int& b, std::size_t& b_at) const {
    if (!chat_has_mark) {
      return false;
    }
    a = chat_line_index(chat_mark_from.key);
    b = chat_line_index(chat_mark_to.key);
    if (a < 0 || b < 0) {
      return false;
    }
    a_at = chat_mark_from.at;
    b_at = chat_mark_to.at;
    if (b < a || (a == b && b_at < a_at)) {
      std::swap(a, b);
      std::swap(a_at, b_at);
    }
    a_at = std::min(a_at, chat_lines[a].text.size());
    b_at = std::min(b_at, chat_lines[b].text.size());
    return !(a == b && a_at == b_at);
  }

  std::string chat_marked_text() const {
    int a, b;
    std::size_t a_at, b_at;
    if (!chat_mark_range(a, a_at, b, b_at)) {
      return {};
    }
    std::string out;
    for (int i = a; i <= b; ++i) {
      const auto& line = chat_lines[i];
      std::size_t from = i == a ? a_at : 0;
      std::size_t to = i == b ? b_at : line.text.size();
      if (to > from) {
        out += line.text.substr(from, to - from);
      }
      if (i != b) {
        out += line.after;
      }
    }
    return out;
  }

  // the mouse over the log, the mark drawn, copying. Called at the end of the log, after its lines
  void chat_update_mark(const std::vector<ecps_backend_t::chat_message_t>& messages) {
    fan::vec2 m = gui::get_io().MousePos;
    bool over_text = gui::is_window_hovered(gui::hovered_flags_child_windows) && !gui::is_any_item_hovered();
    if (over_text && fan::window::is_mouse_clicked(0)) {
      chat_mark_from = chat_mark_to = chat_hit(m);
      chat_marking = true;
      chat_has_mark = false;
    }
    if (chat_marking) {
      if (fan::window::is_mouse_down(0)) {
        chat_mark_to = chat_hit(m);
        chat_has_mark = chat_mark_to.key != chat_mark_from.key || chat_mark_to.at != chat_mark_from.at;
        // past the top or the bottom of the log: it scrolls
        f32_t top = gui::get_window_pos().y;
        f32_t bottom = top + gui::get_window_size().y;
        if (m.y < top) {
          gui::set_scroll_y(gui::get_scroll_y() - (top - m.y) * 0.25f);
        }
        else if (m.y > bottom) {
          gui::set_scroll_y(gui::get_scroll_y() + (m.y - bottom) * 0.25f);
        }
      }
      else {
        chat_marking = false;
      }
    }
    int a, b;
    std::size_t a_at, b_at;
    if (chat_mark_range(a, a_at, b, b_at)) {
      auto* list = gui::get_window_draw_list();
      for (int i = a; i <= b; ++i) {
        const auto& line = chat_lines[i];
        f32_t x0 = chat_prefix_width(line, i == a ? a_at : 0);
        f32_t x1 = i == b ? chat_prefix_width(line, b_at) : line.max.x - line.min.x + (line.after.empty() ? 0.f : 4.f);
        if (x1 > x0) {
          list->AddRectFilled(fan::vec2(line.min.x + x0, line.min.y), fan::vec2(line.min.x + x1, line.max.y), 0x70ff8a3du);
        }
      }
    }
    bool ctrl = engine.window.is_key_down(fan::key_left_control) || engine.window.is_key_down(fan::key_right_control);
    if (chat_has_mark && !gui::get_io().WantTextInput && ctrl && engine.window.is_key_clicked(fan::key_c)) {
      engine.window.set_clipboard(chat_marked_text());
    }
    // right click: the mark, or without one the whole message under the mouse
    if (over_text && fan::window::is_mouse_clicked(1)) {
      if (chat_has_mark) {
        engine.window.set_clipboard(chat_marked_text());
      }
      else if (int i = chat_line_index(chat_hit(m).key); i >= 0) {
        for (const auto& message : messages) {
          if (message.id == chat_lines[i].message_id) {
            engine.window.set_clipboard(message.text);
          }
        }
      }
    }
  }

  // ---- pictures in the chat: a "[file] name.png (size)" line shows the picture itself. The sender has it already,
  // the others take the offer without being asked (pictures only, up to chat_picture_max) into a cache folder;
  // Save keeps a copy where you want it
  static constexpr std::uintmax_t chat_picture_max = 32ull * 1024 * 1024;
  struct chat_picture_t {
    std::future<fan::image::owned_t> loading;
    fan::graphics::image_t image;
    fan::vec2 size = 0;
    bool ready = false;
    bool failed = false;
  };
  std::unordered_map<std::string, chat_picture_t> chat_pictures;
  // what we sent ourselves: file name -> path here
  std::unordered_map<std::string, std::string> chat_sent_files;
  std::unordered_set<std::uint64_t> chat_accepted_pictures;

  static bool chat_is_picture_name(const std::string& name) {
    std::string extension = std::filesystem::path(name).extension().string();
    for (auto& c : extension) {
      c = (char)std::tolower((unsigned char)c);
    }
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".gif" ||
      extension == ".bmp" || extension == ".webp" || extension == ".tga";
  }

  static std::string chat_picture_folder() {
    std::error_code ec;
    auto folder = std::filesystem::temp_directory_path(ec) / "ecps" / "chat_pictures";
    std::filesystem::create_directories(folder, ec);
    return ecps_backend_t::file_utf8(folder);
  }

  bool chat_takes_picture(const ecps_backend_t::file_transfer_view_t& v) const {
    return chat_is_picture_name(v.name) && v.size <= chat_picture_max && ecps_backend.is_channel_joined(v.channel_id);
  }

  static void chat_open_path(const std::string& path) {
  #if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", ecps_backend_t::file_fs_path(path).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  #else
    std::string quoted = "'";
    for (char c : path) {
      quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    quoted += "'";
    std::system(("xdg-open " + quoted + " >/dev/null 2>&1 &").c_str());
  #endif
  }

  // the picture of a file line, or false to show the line as text
  bool render_chat_picture(const std::string& text, std::uint32_t message_id, std::uint32_t from) {
    static const std::string prefix = "[file] ";
    if (text.rfind(prefix, 0) != 0) {
      return false;
    }
    std::size_t paren = text.rfind(" (");
    if (paren == std::string::npos || paren <= prefix.size()) {
      return false;
    }
    std::string name = text.substr(prefix.size(), paren - prefix.size());
    if (!chat_is_picture_name(name)) {
      return false;
    }
    using state_e = ecps_backend_t::file_transfer_t::state_e;
    std::string path;
    std::string waiting;
    if (from == (std::uint32_t)ecps_backend.session_id.i) {
      auto it = chat_sent_files.find(name);
      if (it == chat_sent_files.end()) {
        return false;
      }
      path = it->second;
    }
    else {
      auto views = ecps_backend.file_transfer_views();
      const ecps_backend_t::file_transfer_view_t* found = nullptr;
      for (const auto& v : views) {
        if (!v.outgoing && (std::uint32_t)v.peer.i == from && v.name == name) {
          found = &v;
        }
      }
      if (!found || found->state == state_e::failed || (ecps_backend_t::file_state_finished(found->state) && found->state != state_e::done)) {
        return false;
      }
      if (found->state == state_e::done) {
        path = found->path;
      }
      else {
        waiting = "Receiving " + name + (found->size ? "  " + std::to_string((int)((double)found->transferred * 100.0 / (double)found->size)) + "%" : std::string());
      }
    }
    if (!waiting.empty()) {
      text_wrapped_disabled(waiting);
      return true;
    }
    auto& picture = chat_pictures[path];
    if (!picture.ready && !picture.failed && !picture.loading.valid()) {
      // read and decoded off the gui thread, made big pictures smaller to at most 1600 wide or high
      picture.loading = std::async(std::launch::async, [path]() {
        try {
          return fan::image::load_owned(path, fan::vec2ui(1600, 1600));
        }
        catch (...) {
          return fan::image::owned_t{};
        }
      });
    }
    if (picture.loading.valid() && picture.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto owned = picture.loading.get();
      if (owned.valid()) {
        fan::image::info_t info;
        info.data = owned.data.get();
        info.size = fan::vec2i(owned.size.x, owned.size.y);
        info.channels = owned.channels;
        picture.image = engine.image_load(info, smooth_image());
        picture.size = fan::vec2((f32_t)owned.size.x, (f32_t)owned.size.y);
        picture.ready = true;
      }
      else {
        picture.failed = true;
      }
    }
    if (picture.failed) {
      return false;
    }
    if (!picture.ready) {
      text_wrapped_disabled("Loading " + name + "...");
      return true;
    }
    std::string id = std::to_string(message_id);
    f32_t max_width = std::min(gui::get_content_region_avail().x - 4.f, 420.f);
    f32_t scale = std::min({1.f, max_width / std::max(picture.size.x, 1.f), 320.f / std::max(picture.size.y, 1.f)});
    fan::vec2 size = picture.size * scale;
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::image(picture.image, size);
    chat_add_line(text, "\n", true, gui::get_item_rect_min(), gui::get_item_rect_max());
    gui::set_cursor_screen_pos(at);
    if (gui::invisible_button("##chat_picture" + id, size)) {
      chat_open_path(path);
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Click to open " + name);
      gui::get_window_draw_list()->AddRect(at, at + size, 0x80ffffffu, 4.f);
    }
    if (gui::button("Save##chat_picture_save" + id)) {
      std::string source = path;
      fan::graphics::save_file("", [source, name](std::string_view target) {
        std::error_code ec;
        std::filesystem::path to = ecps_backend_t::file_fs_path(std::string(target));
        if (!to.has_extension()) {
          to += std::filesystem::path(name).extension();
        }
        std::filesystem::copy_file(ecps_backend_t::file_fs_path(source), to, std::filesystem::copy_options::overwrite_existing, ec);
      });
    }
    gui::same_line();
    text_wrapped_disabled(name + "  " + text.substr(paren + 2, text.size() - paren - 3));
    return true;
  }

  void render_chat_text(const std::string& text, std::uint32_t message_id, std::uint32_t from) {
    if (render_chat_picture(text, message_id, from)) {
      return;
    }
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
      chat_plain_lines(part, std::to_string(message_id) + "_t" + std::to_string(plain_part++));
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
        chat_add_line(code, "\n", true, gui::get_item_rect_min(), gui::get_item_rect_max());
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
      chat_sent_files[std::filesystem::path(path).filename().string()] = path;
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
    // the input grows with its lines, up to six
    std::size_t input_lines = std::clamp<std::size_t>(1 + (std::size_t)std::count(chat_input.begin(), chat_input.end(), '\n'), 1, 6);
    f32_t input_box = input_lines * gui::get_text_line_height() + gui::get_style().FramePadding.y * 2;
    f32_t input_height = input_box + gui::get_style().ItemSpacing.y * 2;
    fan::vec2 avail = gui::get_content_region_avail();
    chat_lines.clear();
    if (gui::begin_child("##chat_log", fan::vec2(0, std::max(avail.y - input_height, 80.f)), true)) {
      if (messages.empty()) {
        text_wrapped_disabled("No messages yet. Text, pasted text and files go to everyone in this channel.");
      }
      std::uint32_t last_from = (std::uint32_t)-1;
      std::int64_t last_time = 0;
      for (const auto& message : messages) {
        // a run of messages from the same person within a few minutes shares one name line
        chat_line_message = message.id;
        chat_line_number = 0;
        if (message.from != last_from || message.time - last_time > 300) {
          gui::spacing();
          bool mine = message.from == (std::uint32_t)ecps_backend.session_id.i;
          gui::text(mine ? fan::color(0.55f, 0.8f, 1.f, 1.f) : fan::color(0.95f, 0.8f, 0.45f, 1.f), message.name);
          fan::vec2 name_min = gui::get_item_rect_min();
          gui::same_line();
          text_wrapped_disabled(chat_time_label(message.time));
          chat_add_line(message.name + "  " + chat_time_label(message.time), "\n", true, name_min, gui::get_item_rect_max());
        }
        last_from = message.from;
        last_time = message.time;
        render_chat_text(message.text, message.id, message.from);
      }
      // new messages, or another channel opened: down to the newest
      std::uint64_t revision = ecps_backend.chat_revision.load();
      if (revision != chat_seen_revision || chat_seen_channel != (std::uint32_t)channel_id.i) {
        chat_seen_revision = revision;
        chat_seen_channel = (std::uint32_t)channel_id.i;
        gui::set_scroll_here_y();
      }
      chat_update_mark(messages);
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
    // Enter sends, Shift+Enter is a new line: with Shift held the field takes Enter as a line of its own
    bool shift = engine.window.is_key_down(fan::key_left_shift) || engine.window.is_key_down(fan::key_right_shift);
    gui::input_text_flags_t input_flags = shift ? 0 :
      (gui::input_text_flags_enter_returns_true | gui::input_text_flags_ctrl_enter_for_new_line);
    // with Shift held the field returns true for any change (Enter-returns-true is off then): only without it is it a send
    bool enter = gui::input_text_multiline("##chat_input", &chat_input, fan::vec2(0, input_box), input_flags) && !shift;
    chat_input_active = gui::is_item_active();
    gui::pop_item_width();
    if (gui::is_item_hovered() && chat_input.empty()) {
      gui::set_tooltip("Enter sends, Shift+Enter starts a new line. ```code``` shows as code. Long text and pasted files go as files, pictures show here. Drag over messages to mark them, Ctrl+C or a right click copies");
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

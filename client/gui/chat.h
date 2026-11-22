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
  // files chosen to send, shown above the input until Enter or Send
  std::vector<std::string> chat_pending_files;
  static constexpr f32_t chat_pending_height = 72.f;

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

  bool chat_url_clicked = false;

  // http(s):// and www. links in a line of chat, up to the first space or bracket
  static bool chat_link_at(const std::string& s, std::size_t at, std::size_t& end) {
    bool link = s.compare(at, 7, "http://") == 0 || s.compare(at, 8, "https://") == 0 || s.compare(at, 4, "www.") == 0;
    if (!link) {
      return false;
    }
    end = at;
    while (end < s.size() && (unsigned char)s[end] > 0x20 && s[end] != '<' && s[end] != '>' && s[end] != '"') {
      ++end;
    }
    while (end > at && (s[end - 1] == '.' || s[end - 1] == ',' || s[end - 1] == ';' || s[end - 1] == ')')) {
      --end;
    }
    return end > at;
  }

  static std::string chat_link_url(const std::string& text) {
    return text.rfind("www.", 0) == 0 ? "https://" + text : text;
  }

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
  static constexpr f32_t chat_text_indent = 10.f;
  // ---- replies and pins go as ordinary messages with a tag in front, so a server or client without them still
  // shows readable text: "[reply #12 name] first words" on the line before a reply, "[pin #12] first words", "[unpin #12]"
  struct chat_tag_t {
    enum kind_e { none, reply, pin, unpin };
    kind_e kind = none;
    std::uint32_t id = 0;
    std::string name;  // reply: whose message it answers
    std::string quote; // the first words of that message
    std::string body;  // what is shown as the message itself
  };
  static chat_tag_t chat_parse_tag(const std::string& text) {
    chat_tag_t tag;
    tag.body = text;
    auto parse = [&](const char* prefix, chat_tag_t::kind_e kind) {
      std::size_t n = std::strlen(prefix);
      if (text.compare(0, n, prefix) != 0) {
        return false;
      }
      std::size_t at = n;
      std::uint64_t id = 0;
      std::size_t digits = 0;
      while (at < text.size() && std::isdigit((unsigned char)text[at]) && digits < 10) {
        id = id * 10 + (std::uint64_t)(text[at] - '0');
        ++at;
        ++digits;
      }
      std::size_t line_end = text.find('\n');
      std::size_t close = text.find(']', at);
      if (!digits || close == std::string::npos || (line_end != std::string::npos && close > line_end)) {
        return false;
      }
      if (kind == chat_tag_t::reply) {
        if (text[at] != ' ') {
          return false;
        }
        tag.name = text.substr(at + 1, close - at - 1);
      }
      else if (close != at) {
        return false;
      }
      tag.kind = kind;
      tag.id = (std::uint32_t)id;
      std::size_t after = close + 1;
      if (after < text.size() && text[after] == ' ') {
        ++after;
      }
      tag.quote = text.substr(after, line_end == std::string::npos ? std::string::npos : line_end - after);
      if (kind == chat_tag_t::reply) {
        tag.body = line_end == std::string::npos ? std::string() : text.substr(line_end + 1);
      }
      else {
        tag.body.clear();
      }
      return true;
    };
    if (!parse("[reply #", chat_tag_t::reply) && !parse("[pin #", chat_tag_t::pin)) {
      parse("[unpin #", chat_tag_t::unpin);
    }
    return tag;
  }
  // the first words of a message on one line, for a reply or a pin
  static std::string chat_snippet(const std::string& text, std::size_t max = 60) {
    std::string body = chat_parse_tag(text).body;
    std::string out;
    for (std::size_t i = 0; i < body.size(); ++i) {
      if (body.compare(i, 3, "```") == 0) {
        i += 2;
        continue;
      }
      char c = body[i];
      if (c == '\n' || c == '\r' || c == '\t') {
        if (!out.empty() && out.back() != ' ') {
          out += ' ';
        }
        continue;
      }
      out += c;
    }
    std::size_t first = out.find_first_not_of(' ');
    out = first == std::string::npos ? std::string() : out.substr(first);
    while (!out.empty() && out.back() == ' ') {
      out.pop_back();
    }
    if (out.size() > max) {
      std::size_t cut = max;
      while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) {
        --cut;
      }
      out = out.substr(0, cut) + "...";
    }
    return out.empty() ? std::string("a message") : out;
  }
  // the pinned messages of a channel, oldest pin first, with the words the pin carried for one no longer here
  struct chat_pin_t {
    std::uint32_t id = 0;
    std::string quote;
  };
  static std::vector<chat_pin_t> chat_pins(const std::vector<ecps_backend_t::chat_message_t>& messages) {
    std::vector<chat_pin_t> pins;
    for (const auto& message : messages) {
      chat_tag_t tag = chat_parse_tag(message.text);
      if (tag.kind != chat_tag_t::pin && tag.kind != chat_tag_t::unpin) {
        continue;
      }
      std::erase_if(pins, [&](const chat_pin_t& p) { return p.id == tag.id; });
      if (tag.kind == chat_tag_t::pin) {
        pins.push_back({tag.id, tag.quote});
      }
    }
    return pins;
  }
  // replying: the message answered, shown above the input until sent or dropped
  // editing one of your own messages: the input holds its words, Send changes the message instead of sending a new one
  std::uint32_t chat_edit_id = 0;
  std::uint32_t chat_edit_channel = (std::uint32_t)-1;
  std::string chat_edit_prefix;
  std::uint32_t chat_reply_id = 0;
  // Reply picked in a picture's right-click menu: started by the message loop, which has the message
  std::uint32_t chat_reply_request = 0;
  std::uint32_t chat_reply_channel = (std::uint32_t)-1;
  std::string chat_reply_name;
  std::string chat_reply_quote;
  // a message to scroll to (a reply's quote or a pin clicked) and the one shown lit for a moment after
  std::uint32_t chat_jump_id = 0;
  bool chat_jump_retried = false;
  std::uint32_t chat_flash_id = 0;
  std::chrono::steady_clock::time_point chat_flash_at{};
  // the message under the mouse last frame: its Reply and Pin buttons are drawn this frame
  std::uint32_t chat_hover_id = 0;
  // the message the right click menu is for, and the words marked when it was opened
  std::uint32_t chat_menu_id = 0;
  std::string chat_menu_selection;

  void chat_send_pin(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t id, const std::string& text, bool pin);


  void render_chat_pins(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::vector<ecps_backend_t::chat_message_t>& messages);


  // a reply's quote above its text: who it answers and their first words, a click goes to the message
  void render_chat_reply_quote(const chat_tag_t& tag);


  // The buttons of the message under the mouse: a small bar with icons at its upper right, over a plate so the words
  // under it don't show through. Drawn before the message so its lines follow as usual. Reply and pin for all,
  // edit for your own, delete for your own and, in your channel, anyone's
  void render_chat_message_buttons(ecps_backend_t::Protocol_ChannelID_t channel_id, const ecps_backend_t::chat_message_t& message,
    const chat_tag_t& tag, bool pinned, fan::vec2 back);


  void render_chat_message_menu(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::vector<ecps_backend_t::chat_message_t>& messages,
    const std::vector<chat_pin_t>& pins);


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

  // one drawn line: plain text, with links blue, underlined and clickable
  void chat_draw_link_line(const std::string& line, const std::string& line_id, const std::string& after);


  // a plain part of a message, wrapped to the width, one kept line each
  void chat_plain_lines(const std::string& text, const std::string& id);


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
  void chat_update_mark(const std::vector<ecps_backend_t::chat_message_t>& messages);


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

  // the chat's files on this computer by name (sent or received), kept in the config: after a restart the chat
  // history still shows them. The names carry a time, they don't repeat
  void chat_remember_file(const std::string& name, const std::string& path) {
    fan::json files = config.contains("chat_files") ? config["chat_files"] : fan::json::object();
    if (files.contains(name) && files[name].get<std::string>() == path) {
      return;
    }
    // the ones whose file is gone are forgotten when the list grows
    if (files.size() > 300) {
      fan::json kept = fan::json::object();
      for (auto it = files.begin(); it != files.end(); ++it) {
        std::error_code ec;
        if (std::filesystem::exists(ecps_backend_t::file_fs_path(it.value().get<std::string>()), ec)) {
          kept[it.key()] = it.value();
        }
      }
      files = kept;
    }
    files[name] = path;
    write_to_config("chat_files", files);
  }

  std::string chat_remembered_file(const std::string& name) {
    if (!config.contains("chat_files") || !config["chat_files"].contains(name)) {
      return {};
    }
    std::string path = config["chat_files"][name].get<std::string>();
    std::error_code ec;
    return std::filesystem::exists(ecps_backend_t::file_fs_path(path), ec) ? path : std::string();
  }
  std::unordered_set<std::uint64_t> chat_accepted_pictures;
  bool chat_scroll_down = false;
  // the code block whose copy icon was pressed last, it says Copied for a moment
  std::string chat_copied_id;
  std::chrono::steady_clock::time_point chat_copied_at{};

  // a file offer that a "[file] name (size)" line from the same person in the channel's chat announced: it is taken
  // without asking (pictures into the picture cache to show them, the rest into ecps_downloads)
  bool chat_announced(const ecps_backend_t::file_transfer_view_t& v) const {
    if (!ecps_backend.is_channel_joined(v.channel_id)) {
      return false;
    }
    std::string line = "[file] " + v.name + " (";
    for (const auto& message : ecps_backend.chat_of(v.channel_id)) {
      if (message.from == (std::uint32_t)v.peer.i && message.text.rfind(line, 0) == 0) {
        return true;
      }
    }
    return false;
  }

  // a file line of someone else: how far it is, or Open and Show in folder when it is here
  void render_chat_file_actions(const std::string& text, std::uint32_t message_id, std::uint32_t from);


  // the picture at path, loaded the first time it is asked for: read and decoded off the gui thread, big pictures
  // made smaller to at most 1600 wide or high. ready or failed tell when it is done
  chat_picture_t& chat_picture_for(const std::string& path) {
    auto& picture = chat_pictures[path];
    if (!picture.ready && !picture.failed && !picture.loading.valid()) {
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
        // the log got taller: down to it, so a pasted or new picture is seen
        chat_scroll_down = true;
      }
      else {
        picture.failed = true;
      }
    }
    return picture;
  }

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
    return chat_is_picture_name(v.name) && v.size <= chat_picture_max;
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

  // a copy of a file where the user picks
  static void chat_save_copy(const std::string& source) {
    std::string name = ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(source).filename());
    fan::graphics::save_file("", [source, name](std::string_view target) {
      std::error_code ec;
      std::filesystem::path to = ecps_backend_t::file_fs_path(std::string(target));
      if (!to.has_extension()) {
        to += std::filesystem::path(name).extension();
      }
      std::filesystem::copy_file(ecps_backend_t::file_fs_path(source), to, std::filesystem::copy_options::overwrite_existing, ec);
    });
  }

  // ---- the picture viewer: a clicked chat picture over the whole window. The wheel (with or without Ctrl) zooms
  // at the mouse, dragging moves it, a double click goes back to fitting, Esc or a click beside it closes it
  std::string chat_viewer_path;
  f32_t chat_viewer_zoom = 1.f; // times the size that fits the window
  fan::vec2 chat_viewer_offset = 0;
  bool chat_viewer_dragged = false;

  // Escape also opens the engine's settings menu. When it closed the picture it is put back as it was, for this and
  // the next frame (the engine's toggle can come before or after this)
  int chat_viewer_keep_settings_frames = 0;
  bool chat_viewer_keep_settings_state = false;

  void render_chat_viewer();


  // the picture of a file line, or false to show the line as text
  bool render_chat_picture(const std::string& text, std::uint32_t message_id, std::uint32_t from);


  void render_chat_text(const std::string& text, std::uint32_t message_id, std::uint32_t from);


  // ---- the panels of the main view: Channels, Settings and Chat. Each is open or closed and sits on the right, at
  // the bottom or on the left: drag its tab near an edge of the stream. Those on the same side share one panel with
  // a tab each. Channels starts open on the left. The sides, sizes and what is open are kept in the config
  enum side_panel_e { side_settings, side_chat, side_channels, side_panel_count };
  bool side_open[side_panel_count] = {false, false, true};
  int side_of[side_panel_count] = {0, 0, 2}; // 0 right, 1 bottom, 2 left
  // the tab shown on each side when panels share it, and [3] in full screen, where Settings and Chat share one panel
  int side_front[4] = {-1, -1, -1, -1};
  f32_t side_panel_height = 300.f;
  f32_t side_left_width = 0.f; // 0 until the first frame sets it from the window
  bool side_panel_loaded = false;
  // the stream view this frame: where a dragged tab can be dropped
  fan::vec2 side_view_min = 0;
  fan::vec2 side_view_max = 0;
  int side_panel_moving = -1; // the panel whose tab is being dragged
  int side_panel_drop = -1;
  bool side_panel_resizing = false;
  int side_resizing_side = -1;
  f32_t side_drag_raw = 0.f; // the size the mouse asks for while a line is dragged, below the smallest one too
  f32_t side_size_before_drag = 0.f; // what the side was before the drag, for when the drag hides it
  // a side pushed against its edge: its panels stay open but only a thin grip shows, dragged out (or clicked) it
  // comes back
  bool side_hidden[3] = {false, false, false};
  int side_grip_dragging = -1;
  static constexpr f32_t side_hide_below = 140.f; // dragged smaller than this, the side hides on release
  static constexpr f32_t side_grip_size = 14.f;
  // the tab of a hidden side shows only with the mouse near it, fading in and out
  f32_t side_grip_alpha[3] = {};

  static const char* side_panel_name(int panel) {
    return panel == side_chat ? "Chat" : panel == side_channels ? "Channels" : "Settings";
  }
  // the panels open on a side, hidden or not
  std::vector<int> panels_docked(int side) {
    std::vector<int> out;
    for (int panel = 0; panel < side_panel_count; ++panel) {
      if (side_open[panel] && side_of[panel] == side) {
        out.push_back(panel);
      }
    }
    return out;
  }
  // the panels shown on a side, -1 for full screen: there only Settings and Chat, the stream keeps the rest
  std::vector<int> panels_on(int side) {
    if (side >= 0 && side_hidden[side]) {
      return {};
    }
    std::vector<int> out;
    for (int panel = 0; panel < side_panel_count; ++panel) {
      if (side_open[panel] && (side < 0 ? panel != side_channels : side_of[panel] == side)) {
        out.push_back(panel);
      }
    }
    return out;
  }
  // the tab shown on a side
  int side_shown(int side) {
    auto panels = panels_on(side);
    if (panels.empty()) {
      return -1;
    }
    int front = side_front[side < 0 ? 3 : side];
    return std::find(panels.begin(), panels.end(), front) != panels.end() ? front : panels.front();
  }
  // open, and the tab shown where it is
  bool panel_shown(int panel) {
    return side_open[panel] && (side_shown(side_of[panel]) == panel || (panel != side_channels && is_fullscreen_stream && side_shown(-1) == panel));
  }
  bool any_side_panel_open() const {
    return side_open[side_settings] || side_open[side_chat] || side_open[side_channels];
  }
  // Settings or Chat: what full screen shows beside the stream
  void sync_panels_shown() {
    stream_settings.show_in_stream_view = side_open[side_settings] || side_open[side_chat];
  }

  void load_side_panel_layout() {
    if (side_panel_loaded) {
      return;
    }
    side_panel_loaded = true;
    side_open[side_settings] = stream_settings.show_in_stream_view;
    if (config.contains("side_panel")) {
      fan::json layout = config["side_panel"];
      side_of[side_settings] = std::clamp(layout.value("settings_side", layout.value("side", 0)), 0, 2);
      side_of[side_chat] = std::clamp(layout.value("chat_side", side_of[side_settings]), 0, 2);
      side_of[side_channels] = std::clamp(layout.value("channels_side", 2), 0, 2);
      side_panel_height = layout.value("height", side_panel_height);
      stream_settings.settings_panel_width = layout.value("width", stream_settings.settings_panel_width);
      side_left_width = layout.value("left_width", 0.f);
      window_handler.channels_list_height = layout.value("channels_list_height", 0.f);
      if (layout.contains("open") && layout["open"].is_array() && layout["open"].size() == side_panel_count) {
        for (int panel = 0; panel < side_panel_count; ++panel) {
          side_open[panel] = layout["open"][panel].is_boolean() && layout["open"][panel].get<bool>();
        }
      }
      if (layout.contains("hidden") && layout["hidden"].is_array() && layout["hidden"].size() == 3) {
        for (int side = 0; side < 3; ++side) {
          side_hidden[side] = layout["hidden"][side].is_boolean() && layout["hidden"][side].get<bool>();
        }
      }
    }
    sync_panels_shown();
  }

  void save_side_panel_layout() {
    fan::json layout;
    layout["settings_side"] = side_of[side_settings];
    layout["chat_side"] = side_of[side_chat];
    layout["channels_side"] = side_of[side_channels];
    layout["height"] = side_panel_height;
    layout["width"] = stream_settings.settings_panel_width;
    layout["left_width"] = side_left_width;
    layout["channels_list_height"] = window_handler.channels_list_height;
    fan::json open = fan::json::array();
    for (int panel = 0; panel < side_panel_count; ++panel) {
      open.push_back(side_open[panel]);
    }
    layout["open"] = open;
    fan::json hidden = fan::json::array();
    for (int side = 0; side < 3; ++side) {
      hidden.push_back(side_hidden[side]);
    }
    layout["hidden"] = hidden;
    write_to_config("side_panel", layout);
  }

  // something came in that the chat hasn't shown yet
  bool chat_unread() const {
    return !chat_showing() && ecps_backend.chat_revision.load() != chat_seen_revision &&
      ecps_backend.chat_revision.load() != ecps_backend.chat_history_revision.load() &&
      ecps_backend.chat_known(selected_channel_id);
  }

  void open_side_panel(int panel) {
    load_side_panel_layout();
    side_open[panel] = true;
    side_hidden[side_of[panel]] = false;
    side_front[side_of[panel]] = panel;
    if (panel != side_channels) {
      side_front[3] = panel;
    }
    if (panel == side_chat) {
      chat_focus_input = true;
    }
    sync_panels_shown();
    save_side_panel_layout();
  }
  void close_side_panel(int panel) {
    side_open[panel] = false;
    sync_panels_shown();
    save_side_panel_layout();
  }
  // the toolbar buttons: open their panel (or its tab), the same again closes it
  void toggle_side_panel(int panel) {
    load_side_panel_layout();
    if (panel_shown(panel)) {
      close_side_panel(panel);
    }
    else {
      open_side_panel(panel);
    }
  }

  // the chat icon of the stream toolbars, with a dot while there is something unread
  void render_side_chat_button(const char* id, f32_t size) {
    fan::vec2 at = gui::get_cursor_screen_pos();
    if (icon_button(id, icon_e::chat, panel_shown(side_chat) ? "Hide chat" : "Chat", size, icon_style_e::overlay)) {
      toggle_side_panel(side_chat);
    }
    if (chat_unread()) {
      gui::get_window_draw_list()->AddCircleFilled(at + fan::vec2(size * 0.86f, size * 0.14f), size * 0.16f, 0xff4040ffu, 16);
    }
  }

  // the gear of the stream toolbars
  void render_side_settings_button(const char* id, f32_t size) {
    if (icon_button(id, icon_e::gear, panel_shown(side_settings) ? "Hide settings" : "Settings", size, icon_style_e::overlay)) {
      toggle_side_panel(side_settings);
    }
  }

  // the tabs at the top of a panel, and an X that closes the one shown. Dragging a tab moves that panel
  void render_side_panel_header(const std::vector<int>& panels, int shown) {
    f32_t height = gui::get_frame_height();
    f32_t close_width = height;
    f32_t width = (gui::get_content_region_avail().x - close_width - 4.f * panels.size()) / panels.size();
    for (int panel : panels) {
      bool selected = panel == shown;
      const char* text = side_panel_name(panel);
      fan::vec2 at = gui::get_cursor_screen_pos();
      gui::push_id(panel);
      bool clicked = gui::invisible_button("##side_tab", fan::vec2(width, height));
      gui::pop_id();
      bool hovered = gui::is_item_hovered();
      if (!is_fullscreen_stream && gui::is_item_active() && gui::is_mouse_dragging(0, 6.f)) {
        side_panel_moving = panel;
      }
      if (hovered && side_panel_moving < 0 && !is_fullscreen_stream) {
        gui::set_tooltip("Drag to move it to the right, the bottom or the left. Drop it on another one to share a panel");
      }
      if (clicked && side_panel_moving < 0) {
        side_front[is_fullscreen_stream ? 3 : side_of[panel]] = panel;
        if (panel == side_chat) {
          chat_focus_input = true;
        }
      }
      auto* list = gui::get_window_draw_list();
      std::uint32_t fill = selected ? accent_scaled(0.22f) : hovered ? accent_scaled(0.11f) : 0x00000000u;
      list->AddRectFilled(at, at + fan::vec2(width, height), fill, height * 0.3f);
      if (selected) {
        list->AddRectFilled(at + fan::vec2(6.f, height - 2.f), at + fan::vec2(width - 6.f, height), accent_hover_u32, 1.f);
      }
      f32_t icon_size = height * 0.55f;
      std::string shown_text = gui::fit_title(text, std::max(width - icon_size - 18.f, 10.f));
      f32_t content = icon_size + 6.f + gui::calc_text_size(shown_text).x;
      fan::vec2 start = at + fan::vec2(std::max((width - content) * 0.5f, 4.f), 0);
      std::uint32_t ink = selected || hovered ? 0xffffffffu : 0xffa0a0a0u;
      icon_e icon = panel == side_chat ? icon_e::chat : panel == side_channels ? icon_e::server : icon_e::gear;
      draw_icon(list, icon, start + fan::vec2(icon_size * 0.5f, height * 0.5f), icon_size, ink);
      list->AddText(start + fan::vec2(icon_size + 6.f, (height - gui::get_font_size()) * 0.5f), ink, shown_text.c_str());
      if (panel == side_chat && chat_unread()) {
        list->AddCircleFilled(start + fan::vec2(content + 7.f, height * 0.3f), 3.5f, 0xff4040ffu, 12);
      }
      gui::same_line(0, 4.f);
    }
    if (icon_button("##side_panel_close", icon_e::close,
      std::string("Close ") + side_panel_name(shown) + ". The toolbar under the stream opens it again", close_width)) {
      close_side_panel(shown);
    }
    gui::spacing();
  }

  // a panel: its tabs and the one shown
  void render_side_panel_content(int side = -1);


  // the line between the stream and a panel: dragged, it makes the panel bigger or smaller
  void render_side_panel_handle(fan::vec2 size, int side);


  // the thin strip left at an edge by a hidden side. Dragged out it shows the side again as wide as dragged, a click
  // shows it as it was
  void render_side_grip(fan::vec2 size, int side);


  // while a tab is dragged: where it would go, and on release it goes there
  void render_side_panel_drop();


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
      // the line first: the others take an offer without asking when its line is in the chat
      std::string name = ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(path).filename());
      chat_sent_files[name] = path;
      chat_remember_file(name, path);
      std::string line = "[file] " + name + " (" +
        chat_size_label(std::filesystem::file_size(ecps_backend_t::file_fs_path(path), ec)) + ")" +
        (peers.empty() ? "  - nobody else here to send it to" : "");
      backend_queue([channel_id, line]() -> fan::event::task_t {
        return ecps_backend.send_chat(channel_id, line);
      });
      for (const auto& peer : peers) {
        backend_queue([channel_id, peer = peer.session_id, path]() -> fan::event::task_t {
          return ecps_backend.send_file(channel_id, peer, path);
        });
      }
    }
  }

  // a line of only whitespace, like pressing Enter in an empty box, is not a message
  static bool chat_has_text_content(const std::string& text) {
    return text.find_first_not_of(" \t\r\n\v\f") != std::string::npos;
  }

  // a message, or a .txt file when it is too long for one
  void chat_send_text(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::string& text) {
    if (!chat_has_text_content(text)) {
      return;
    }
    if (text.size() <= chat_inline_max) {
      backend_queue([channel_id, text]() -> fan::event::task_t {
        return ecps_backend.send_chat(channel_id, text);
      });
      return;
    }
    chat_send_files(channel_id, { chat_text_to_file(text) });
  }

  // text too long for one message, written to a .txt file to go as a file
  std::string chat_text_to_file(const std::string& text) {
    std::error_code ec;
    auto folder = std::filesystem::temp_directory_path(ec) / "ecps";
    std::filesystem::create_directories(folder, ec);
    auto path = folder / ("message-" + std::to_string((long long)std::time(nullptr)) + ".txt");
    {
      std::ofstream out(path, std::ios::binary);
      out << text;
    }
    return path.string();
  }

  // Files waiting above the input, pictures shown small: they go with Enter or Send, together with the text, and
  // the x takes one out again. Pasted, dropped and attached files all wait here first
  void render_chat_pending_files();


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
  #else
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

  void render_chat_tab(ecps_backend_t::Protocol_ChannelID_t channel_id);


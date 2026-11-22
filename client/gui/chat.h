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
  std::uint32_t chat_reply_channel = (std::uint32_t)-1;
  std::string chat_reply_name;
  std::string chat_reply_quote;
  // a message to scroll to (a reply's quote or a pin clicked) and the one shown lit for a moment after
  std::uint32_t chat_jump_id = 0;
  std::uint32_t chat_flash_id = 0;
  std::chrono::steady_clock::time_point chat_flash_at{};
  // the message under the mouse last frame: its Reply and Pin buttons are drawn this frame
  std::uint32_t chat_hover_id = 0;

  void chat_send_pin(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t id, const std::string& text, bool pin) {
    chat_send_text(channel_id, pin ? "[pin #" + std::to_string(id) + "] " + chat_snippet(text) : "[unpin #" + std::to_string(id) + "]");
  }

  void render_chat_pins(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::vector<ecps_backend_t::chat_message_t>& messages) {
    auto pins = chat_pins(messages);
    if (pins.empty()) {
      return;
    }
    if (gui::button("Pinned (" + std::to_string(pins.size()) + ")##chat_pins")) {
      gui::open_popup("##chat_pins_popup");
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("The messages pinned in this channel");
    }
    if (gui::begin_popup("##chat_pins_popup")) {
      gui::text_disabled("Pinned messages");
      gui::separator();
      for (auto it = pins.rbegin(); it != pins.rend(); ++it) {
        const ecps_backend_t::chat_message_t* original = nullptr;
        for (const auto& message : messages) {
          if (message.id == it->id) {
            original = &message;
          }
        }
        gui::push_id((int)it->id);
        std::string line = original ? original->name + ": " + chat_snippet(original->text) : it->quote;
        if (gui::selectable(line, false, 0, fan::vec2(std::min(gui::calc_text_size(line).x, 420.f), 0))) {
          if (original) {
            chat_jump_id = it->id;
          }
        }
        if (!original && gui::is_item_hovered()) {
          gui::set_tooltip("Older than the messages kept here");
        }
        gui::same_line(0, 12.f);
        if (gui::button("Unpin")) {
          chat_send_pin(channel_id, it->id, std::string(), false);
        }
        gui::pop_id();
      }
      gui::end_popup();
    }
  }

  // a reply's quote above its text: who it answers and their first words, a click goes to the message
  void render_chat_reply_quote(const chat_tag_t& tag) {
    std::string line = tag.name + ": " + tag.quote;
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::set_cursor_screen_pos(at + fan::vec2(14.f, 0.f));
    gui::text(fan::color(0.6f, 0.62f, 0.7f, 1.f), line);
    fan::vec2 lo = gui::get_item_rect_min();
    fan::vec2 hi = gui::get_item_rect_max();
    // the little hook from the quote down to the reply
    auto* list = gui::get_window_draw_list();
    f32_t mid = (lo.y + hi.y) * 0.5f;
    list->AddLine(fan::vec2(at.x + 4.f, hi.y + 2.f), fan::vec2(at.x + 4.f, mid), 0xff707070u, 1.5f);
    list->AddLine(fan::vec2(at.x + 4.f, mid), fan::vec2(lo.x - 3.f, mid), 0xff707070u, 1.5f);
    if (gui::is_item_hovered()) {
      gui::set_mouse_cursor(gui::mouse_cursor_hand);
      gui::set_tooltip("Go to the message");
      if (fan::window::is_mouse_clicked(0)) {
        chat_jump_id = tag.id;
        chat_url_clicked = true;
      }
    }
    chat_add_line(line, "\n", true, lo, hi);
  }

  // The buttons of the message under the mouse: a small bar with icons at its upper right, over a plate so the words
  // under it don't show through. Drawn before the message so its lines follow as usual. Reply and pin for all,
  // edit for your own, delete for your own and, in your channel, anyone's
  void render_chat_message_buttons(ecps_backend_t::Protocol_ChannelID_t channel_id, const ecps_backend_t::chat_message_t& message,
    const chat_tag_t& tag, bool pinned, fan::vec2 back) {
    // called after the message's text, so its buttons are the last items there and get the mouse where they overlap
    // the text's own areas
    fan::vec2 resume = gui::get_cursor_screen_pos();
    bool mine = message.from == (std::uint32_t)ecps_backend.session_id.i;
    bool can_edit = mine && (tag.kind == chat_tag_t::none || tag.kind == chat_tag_t::reply);
    bool can_delete = mine || ecps_backend.is_current_user_host_of_channel(channel_id);
    int count = 2 + (can_edit ? 1 : 0) + (can_delete ? 1 : 0);

    f32_t size = std::round(gui::get_frame_height() * 0.72f);
    f32_t gap = 3.f;
    f32_t pad = 3.f;
    f32_t width = size * count + gap * (count - 1) + pad * 2.f;
    f32_t right = back.x + gui::get_content_region_avail().x;
    // inside the message's own rows, so the mouse stays over the message while it reaches for a button
    fan::vec2 at(right - width - 2.f, back.y - 1.f);
    auto* list = gui::get_window_draw_list();
    list->AddRectFilled(at, at + fan::vec2(width, size + pad * 2.f), accent_scaled(0.1f, 0xf0), 8.f);
    list->AddRect(at, at + fan::vec2(width, size + pad * 2.f), accent_alpha(accent_hover_u32, 0x40), 8.f);

    gui::push_id((int)message.id);
    fan::vec2 cursor = at + fan::vec2(pad, pad);
    auto place = [&] {
      gui::set_cursor_screen_pos(cursor);
      cursor.x += size + gap;
    };
    place();
    if (icon_button("##msg_reply", icon_e::reply, "Reply", size)) {
      chat_reply_id = message.id;
      chat_reply_channel = (std::uint32_t)channel_id.i;
      chat_reply_name = message.name;
      chat_reply_quote = chat_snippet(tag.body);
      chat_edit_id = 0;
      chat_focus_input = true;
    }
    place();
    if (icon_button("##msg_pin", icon_e::pin, pinned ? "Unpin" : "Pin", size, pinned ? icon_style_e::accent : icon_style_e::plain)) {
      chat_send_pin(channel_id, message.id, tag.body, !pinned);
    }
    if (can_edit) {
      place();
      if (icon_button("##msg_edit", icon_e::edit, "Edit", size)) {
        // the reply's quote line stays as it was, the words under it are what changes
        chat_edit_id = message.id;
        chat_edit_channel = (std::uint32_t)channel_id.i;
        std::size_t line_end = message.text.find('\n');
        chat_edit_prefix = tag.kind == chat_tag_t::reply && line_end != std::string::npos ? message.text.substr(0, line_end + 1) : std::string();
        chat_input = message.text.substr(chat_edit_prefix.size());
        chat_reply_id = 0;
        chat_focus_input = true;
      }
    }
    if (can_delete) {
      place();
      if (icon_button("##msg_delete", icon_e::trash, "Delete", size, icon_style_e::danger)) {
        backend_queue([channel_id, id = message.id]() -> fan::event::task_t {
          return ecps_backend.change_chat(channel_id, id, std::string());
        });
      }
    }
    gui::pop_id();
    gui::set_cursor_screen_pos(resume);
  }

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
  void chat_draw_link_line(const std::string& line, const std::string& line_id, const std::string& after) {
    fan::vec2 start = gui::get_cursor_screen_pos();
    std::size_t at = 0;
    std::size_t plain_start = 0;
    int part = 0;
    auto draw_plain = [&](std::size_t from, std::size_t to) {
      if (to <= from) {
        return;
      }
      gui::text(line.substr(from, to - from));
      gui::same_line(0, 0.f);
    };
    while (at < line.size()) {
      std::size_t end = 0;
      if (!chat_link_at(line, at, end)) {
        ++at;
        continue;
      }
      draw_plain(plain_start, at);
      std::string url = line.substr(at, end - at);
      gui::push_id(line_id + std::to_string(part++));
      gui::text(fan::color(0.3f, 0.6f, 1.f, 1.f), url);
      fan::vec2 lo = gui::get_item_rect_min();
      fan::vec2 hi = gui::get_item_rect_max();
      gui::get_window_draw_list()->AddLine(fan::vec2(lo.x, hi.y - 1.f), fan::vec2(hi.x, hi.y - 1.f), accent_hover_u32, 1.f);
      if (gui::is_item_hovered()) {
        gui::set_tooltip("Open link");
        if (fan::window::is_mouse_clicked(0)) {
          chat_open_path(chat_link_url(url));
          chat_url_clicked = true;
        }
      }
      gui::pop_id();
      gui::same_line(0, 0.f);
      at = end;
      plain_start = end;
    }
    draw_plain(plain_start, line.size());
    // every part above leaves the cursor on its row: end the row, or the next line or message would go on it
    gui::new_line();
    fan::vec2 max = start + fan::vec2(gui::calc_text_size(line.empty() ? std::string(" ") : line).x, gui::get_text_line_height());
    chat_add_line(line, after, false, start, max);
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
      chat_draw_link_line(line.empty() ? std::string(" ") : line, id, soft ? "" : "\n");
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
    if (over_text && !chat_url_clicked && fan::window::is_mouse_clicked(0)) {
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
          list->AddRectFilled(fan::vec2(line.min.x + x0, line.min.y), fan::vec2(line.min.x + x1, line.max.y), accent_alpha(accent_hover_u32, 0x70));
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
            engine.window.set_clipboard(chat_parse_tag(message.text).body);
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
  void render_chat_file_actions(const std::string& text, std::uint32_t message_id, std::uint32_t from) {
    static const std::string prefix = "[file] ";
    if (from == (std::uint32_t)ecps_backend.session_id.i || text.rfind(prefix, 0) != 0) {
      return;
    }
    std::size_t paren = text.rfind(" (");
    if (paren == std::string::npos || paren <= prefix.size()) {
      return;
    }
    std::string name = text.substr(prefix.size(), paren - prefix.size());
    using state_e = ecps_backend_t::file_transfer_t::state_e;
    auto views = ecps_backend.file_transfer_views();
    const ecps_backend_t::file_transfer_view_t* found = nullptr;
    for (const auto& v : views) {
      if (!v.outgoing && (std::uint32_t)v.peer.i == from && v.name == name) {
        found = &v;
      }
    }
    std::string id = std::to_string(message_id);
    std::string kept = found ? std::string() : chat_remembered_file(name);
    if (!kept.empty()) {
      if (gui::button("Open##chat_file_open" + id)) {
        chat_open_path(kept);
      }
      gui::same_line();
      if (gui::button("Show in folder##chat_file_folder" + id)) {
        chat_open_path(ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(kept).parent_path()));
      }
      return;
    }
    if (!found) {
      return;
    }
    if (found->state == state_e::transferring) {
      gui::text_wrapped_disabled("Receiving  " + file_progress_text(*found));
    }
    else if (found->state == state_e::done) {
      chat_remember_file(name, found->path);
      if (gui::button("Open##chat_file_open" + id)) {
        chat_open_path(found->path);
      }
      gui::same_line();
      if (gui::button("Show in folder##chat_file_folder" + id)) {
        chat_open_path(ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(found->path).parent_path()));
      }
    }
    else if (ecps_backend_t::file_state_finished(found->state)) {
      gui::text_wrapped_disabled(found->status);
    }
  }

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

  void render_chat_viewer() {
    if (chat_viewer_path.empty()) {
      return;
    }
    auto found = chat_pictures.find(chat_viewer_path);
    if (found == chat_pictures.end() || !found->second.ready || engine.window.is_key_clicked(fan::key_escape)) {
      chat_viewer_path.clear();
      return;
    }
    auto& picture = found->second;
    auto* viewport = gui::get_main_viewport();
    fan::vec2 pos(viewport->WorkPos.x, viewport->WorkPos.y);
    fan::vec2 area(viewport->WorkSize.x, viewport->WorkSize.y);
    gui::set_next_window_pos(pos);
    gui::set_next_window_size(area);
    gui::set_next_window_focus();
    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_window_border_size, 0.f);
    gui::push_style_var(gui::style_var_window_rounding, 0.f);
    gui::push_style_color(gui::col_window_bg, fan::color(0.f, 0.f, 0.f, 0.9f));
    bool open = gui::begin("##chat_picture_viewer", nullptr, gui::window_flags_no_title_bar | gui::window_flags_no_resize |
      gui::window_flags_no_move | gui::window_flags_no_collapse | gui::window_flags_no_docking | gui::window_flags_no_saved_settings |
      gui::window_flags_no_scrollbar | gui::window_flags_no_scroll_with_mouse);
    gui::pop_style_color();
    gui::pop_style_var(3);
    if (!open) {
      gui::end();
      return;
    }
    auto& io = gui::get_io();
    fan::vec2 middle = pos + area * 0.5f;
    f32_t fit = std::min({1.f, area.x * 0.9f / std::max(picture.size.x, 1.f), area.y * 0.85f / std::max(picture.size.y, 1.f)});

    // the wheel zooms at the mouse: the point under it stays there
    if (gui::is_window_hovered() && io.MouseWheel != 0.f) {
      f32_t old_zoom = chat_viewer_zoom;
      chat_viewer_zoom = std::clamp(chat_viewer_zoom * std::pow(1.2f, io.MouseWheel), 0.2f, 40.f / std::max(fit, 0.01f));
      fan::vec2 center = middle + chat_viewer_offset;
      fan::vec2 mouse = io.MousePos;
      center = mouse + (center - mouse) * (chat_viewer_zoom / old_zoom);
      chat_viewer_offset = center - middle;
    }
    fan::vec2 shown = picture.size * fit * chat_viewer_zoom;
    fan::vec2 image_min = middle + chat_viewer_offset - shown * 0.5f;
    gui::set_cursor_screen_pos(image_min);
    gui::image(picture.image, shown);

    // the buttons, before the background so they get the clicks
    std::string name = ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(chat_viewer_path).filename());
    f32_t button = gui::get_frame_height() * 1.3f;
    gui::set_cursor_screen_pos(pos + fan::vec2(area.x - (button + 10.f) * 3.f, 14.f));
    if (icon_button("##viewer_save", icon_e::send_file, "Save a copy", button, icon_style_e::overlay)) {
      chat_save_copy(chat_viewer_path);
    }
    gui::same_line(0, 10.f);
    if (icon_button("##viewer_open", icon_e::fullscreen, "Open in another program", button, icon_style_e::overlay)) {
      chat_open_path(chat_viewer_path);
    }
    gui::same_line(0, 10.f);
    bool close = icon_button("##viewer_close", icon_e::stop, "Close (Esc)", button, icon_style_e::overlay);

    // the background: dragging moves the picture, a click beside it closes the viewer
    gui::set_cursor_screen_pos(pos);
    bool clicked = gui::invisible_button("##viewer_background", area);
    if (gui::is_item_hovered() && fan::window::is_mouse_clicked(0)) {
      chat_viewer_dragged = false;
    }
    if (gui::is_item_active() && gui::is_mouse_dragging(0, 3.f)) {
      chat_viewer_offset += fan::vec2(io.MouseDelta);
      chat_viewer_dragged = true;
    }
    bool on_picture = io.MousePos.x >= image_min.x && io.MousePos.y >= image_min.y &&
      io.MousePos.x <= image_min.x + shown.x && io.MousePos.y <= image_min.y + shown.y;
    if (gui::is_item_hovered() && gui::is_mouse_double_clicked(0)) {
      chat_viewer_zoom = 1.f;
      chat_viewer_offset = 0;
    }
    else if (clicked && !chat_viewer_dragged && !on_picture) {
      close = true;
    }
    if (gui::is_item_hovered()) {
      gui::set_mouse_cursor(on_picture ? gui::mouse_cursor_resize_all : gui::mouse_cursor_arrow);
    }

    std::string info = name + "   " + std::to_string((int)picture.size.x) + " x " + std::to_string((int)picture.size.y) +
      "   " + std::to_string((int)std::round(fit * chat_viewer_zoom * 100.f)) + "%   Wheel zooms, drag moves, double click fits";
    fan::vec2 info_size = gui::calc_text_size(info);
    gui::get_window_draw_list()->AddText(pos + fan::vec2((area.x - info_size.x) * 0.5f, area.y - info_size.y - 16.f), 0xffc8c8c8u, info.c_str());
    gui::end();
    if (close) {
      chat_viewer_path.clear();
    }
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
    if (auto it = chat_sent_files.find(name); it != chat_sent_files.end()) {
      path = it->second;
    }
    else if (std::string kept = chat_remembered_file(name); !kept.empty()) {
      path = kept;
    }
    else if (from == (std::uint32_t)ecps_backend.session_id.i) {
      return false;
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
        chat_remember_file(name, path);
      }
      else {
        waiting = "Receiving " + name + (found->size ? "  " + std::to_string((int)((double)found->transferred * 100.0 / (double)found->size)) + "%" : std::string());
      }
    }
    if (!waiting.empty()) {
      gui::text_wrapped_disabled(waiting);
      return true;
    }
    auto& picture = chat_picture_for(path);
    if (picture.failed) {
      return false;
    }
    if (!picture.ready) {
      gui::text_wrapped_disabled("Loading " + name + "...");
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
      chat_viewer_path = path;
      chat_viewer_zoom = 1.f;
      chat_viewer_offset = 0;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Click to view " + name);
      gui::get_window_draw_list()->AddRect(at, at + size, 0x80ffffffu, 4.f);
    }
    if (gui::button("Save##chat_picture_save" + id)) {
      chat_save_copy(path);
    }
    gui::same_line();
    gui::text_wrapped_disabled(name + "  " + text.substr(paren + 2, text.size() - paren - 3));
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
        // a block that fits in height has nothing to scroll up or down: the wheel goes on to the chat around it
        // instead of moving the block a few pixels. Only a block taller than its 400 px takes the wheel
        gui::window_flags_t code_flags = gui::window_flags_horizontal_scrollbar;
        if (height <= 400.f) {
          code_flags |= gui::window_flags_no_scroll_with_mouse;
        }
        if (gui::begin_child("##chat_code" + id, fan::vec2(0, std::min(height, 400.f)), 1, code_flags)) {
          gui::text(code);
          bool copied = chat_copied_id == id && std::chrono::steady_clock::now() - chat_copied_at < std::chrono::milliseconds(1500);
          if (gui::is_window_hovered() || copied) {
            f32_t button = gui::get_frame_height();
            gui::set_cursor_screen_pos(gui::get_window_pos() + fan::vec2(gui::get_window_size().x - button - 6.f, 6.f));
            if (icon_button(("##chat_code_copy" + id).c_str(), icon_e::copy, copied ? "Copied" : "Copy", button)) {
              engine.window.set_clipboard(code);
              chat_copied_id = id;
              chat_copied_at = std::chrono::steady_clock::now();
            }
          }
        }
        gui::end_child();
        chat_add_line(code, "\n", true, gui::get_item_rect_min(), gui::get_item_rect_max());
        gui::pop_style_color();
      }
      at = close == std::string::npos ? text.size() : close + 3;
    }
    render_chat_file_actions(text, message_id, from);
  }

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
  static constexpr f32_t side_grip_size = 10.f;

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
  void render_side_panel_content(int side = -1) {
    load_side_panel_layout();
    auto panels = panels_on(side);
    if (panels.empty()) {
      return;
    }
    int shown = side_shown(side);
    render_side_panel_header(panels, shown);
    if (shown == side_chat) {
      if (ecps_backend.channel_info.empty()) {
        gui::text_wrapped_disabled("Join a channel to chat");
      }
      else {
        render_chat_tab(selected_channel_id);
      }
    }
    else if (shown == side_channels) {
      window_handler.render_channels_content();
    }
    else if (gui::begin_tab_bar("SettingsTabs")) {
      if (gui::begin_tab_item("Stream", nullptr, 0)) {
        window_handler.render_stream_settings_content_compact();
        gui::end_tab_item();
      }
      if (gui::begin_tab_item("Audio", nullptr, 0)) {
        render_audio_tab();
        gui::end_tab_item();
      }
      if (gui::begin_tab_item("Theme", nullptr, 0)) {
        render_theme_tab();
        gui::end_tab_item();
      }
      gui::end_tab_bar();
    }
  }

  // the line between the stream and a panel: dragged, it makes the panel bigger or smaller
  void render_side_panel_handle(fan::vec2 size, int side) {
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::push_id(side);
    gui::invisible_button("##side_panel_handle", size);
    gui::pop_id();
    bool hot = gui::is_item_hovered() || gui::is_item_active();
    if (hot) {
      gui::set_mouse_cursor(side == 1 ? gui::mouse_cursor_resize_ns : gui::mouse_cursor_resize_ew);
    }
    f32_t hide_below = side == 1 ? side_hide_below * 0.6f : side_hide_below;
    bool will_hide = false;
    f32_t& stored = side == 1 ? side_panel_height : side == 0 ? stream_settings.settings_panel_width : side_left_width;
    if (gui::is_item_active()) {
      fan::vec2 delta = gui::get_io().MouseDelta;
      fan::vec2 area = side_view_max - side_view_min;
      if (!side_panel_resizing) {
        side_panel_resizing = true;
        side_resizing_side = side;
        side_drag_raw = stored;
        side_size_before_drag = stored;
      }
      side_drag_raw += side == 1 ? -delta.y : side == 0 ? -delta.x : delta.x;
      stored = std::clamp(side_drag_raw, side == 1 ? 150.f : 250.f, (side == 1 ? area.y : area.x) * 0.7f);
      will_hide = side_drag_raw < hide_below;
      if (will_hide) {
        gui::set_tooltip("Let go to push it against the edge. Drag it out from there to bring it back");
      }
    }
    else if (side_panel_resizing && side_resizing_side == side && !fan::window::is_mouse_down(0)) {
      side_panel_resizing = false;
      side_resizing_side = -1;
      if (side_drag_raw < hide_below) {
        side_hidden[side] = true;
        // the drag squeezed the size down to the smallest one: what comes back is the size it had before it
        stored = std::max(side_size_before_drag, side == 1 ? 150.f : 250.f);
      }
      save_side_panel_layout();
    }
    std::uint32_t color = will_hide ? 0xff4040ffu : hot ? accent_hover_u32 : 0x30ffffffu;
    fan::vec2 c = at + size * 0.5f;
    if (side == 1) {
      gui::get_window_draw_list()->AddLine(fan::vec2(c.x - 24.f, c.y), fan::vec2(c.x + 24.f, c.y), color, 3.f);
    }
    else {
      gui::get_window_draw_list()->AddLine(fan::vec2(c.x, c.y - 24.f), fan::vec2(c.x, c.y + 24.f), color, 3.f);
    }
  }

  // the thin strip left at an edge by a hidden side. Dragged out it shows the side again as wide as dragged, a click
  // shows it as it was
  void render_side_grip(fan::vec2 size, int side) {
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::push_id(side);
    bool clicked = gui::invisible_button("##side_grip", size);
    gui::pop_id();
    bool active = gui::is_item_active();
    bool hot = gui::is_item_hovered() || active;
    fan::vec2 m = gui::get_io().MousePos;
    fan::vec2 area = side_view_max - side_view_min;
    f32_t distance = side == 2 ? m.x - side_view_min.x : side == 0 ? side_view_max.x - m.x : side_view_max.y - m.y;
    f32_t smallest = side == 1 ? 150.f : 250.f;
    f32_t largest = (side == 1 ? area.y : area.x) * 0.7f;
    std::string names;
    for (int panel : panels_docked(side)) {
      names += std::string(names.empty() ? "" : " and ") + side_panel_name(panel);
    }
    if (hot) {
      gui::set_mouse_cursor(side == 1 ? gui::mouse_cursor_resize_ns : gui::mouse_cursor_resize_ew);
      if (!active) {
        gui::set_tooltip("Drag out or click to show " + names);
      }
    }
    if (active && gui::is_mouse_dragging(0, 4.f)) {
      side_grip_dragging = side;
      // where it will come to
      f32_t shown = std::clamp(distance, smallest, largest);
      fan::vec2 a, b;
      switch (side) {
        case 0: a = fan::vec2(side_view_max.x - shown, side_view_min.y); b = side_view_max; break;
        case 1: a = fan::vec2(side_view_min.x, side_view_max.y - shown); b = side_view_max; break;
        default: a = side_view_min; b = fan::vec2(side_view_min.x + shown, side_view_max.y); break;
      }
      auto* list = gui::get_foreground_draw_list();
      list->AddRectFilled(a, b, accent_alpha(accent_hover_u32, 0x40), 8.f);
      list->AddRect(a, b, accent_hover_u32, 8.f, 0, 2.f);
      fan::vec2 text_size = gui::calc_text_size(names);
      list->AddText((a + b) * 0.5f - text_size * 0.5f, 0xffffffffu, names.c_str());
    }
    else if (side_grip_dragging == side && !active) {
      // let go after a drag: as wide as it was dragged
      side_grip_dragging = -1;
      if (distance > size.x + size.y) {
        (side == 1 ? side_panel_height : side == 0 ? stream_settings.settings_panel_width : side_left_width) =
          std::clamp(distance, smallest, largest);
      }
      side_hidden[side] = false;
      save_side_panel_layout();
    }
    else if (clicked) {
      side_hidden[side] = false;
      save_side_panel_layout();
    }
    // a rounded bar along the edge
    auto* list = gui::get_window_draw_list();
    std::uint32_t color = hot ? accent_hover_u32 : 0x50ffffffu;
    fan::vec2 c = at + size * 0.5f;
    if (side == 1) {
      list->AddRectFilled(fan::vec2(c.x - 40.f, c.y - 2.f), fan::vec2(c.x + 40.f, c.y + 2.f), color, 2.f);
    }
    else {
      list->AddRectFilled(fan::vec2(c.x - 2.f, c.y - 40.f), fan::vec2(c.x + 2.f, c.y + 40.f), color, 2.f);
    }
  }

  // while a tab is dragged: where it would go, and on release it goes there
  void render_side_panel_drop() {
    if (side_panel_moving < 0) {
      return;
    }
    int moving = side_panel_moving;
    if (!fan::window::is_mouse_down(0)) {
      if (side_panel_drop >= 0 && side_panel_drop != side_of[moving]) {
        side_of[moving] = side_panel_drop;
        side_hidden[side_panel_drop] = false;
        side_front[side_panel_drop] = moving;
        save_side_panel_layout();
      }
      side_panel_moving = -1;
      side_panel_drop = -1;
      return;
    }
    fan::vec2 m = gui::get_io().MousePos;
    fan::vec2 min = side_view_min;
    fan::vec2 max = side_view_max;
    fan::vec2 size = max - min;
    f32_t to_right = (max.x - m.x) / size.x;
    f32_t to_bottom = (max.y - m.y) / size.y;
    f32_t to_left = (m.x - min.x) / size.x;
    side_panel_drop = to_right <= to_bottom && to_right <= to_left ? 0 : to_bottom <= to_left ? 1 : 2;
    std::string others;
    for (int panel : panels_on(side_panel_drop)) {
      if (panel != moving) {
        others += std::string(others.empty() ? "" : " and ") + side_panel_name(panel);
      }
    }
    fan::vec2 a, b;
    std::string where;
    switch (side_panel_drop) {
      case 0: a = fan::vec2(max.x - size.x * 0.28f, min.y); b = max; where = "on the right"; break;
      case 1: a = fan::vec2(min.x, max.y - size.y * 0.35f); b = max; where = "at the bottom"; break;
      default: a = min; b = fan::vec2(min.x + size.x * 0.28f, max.y); where = "on the left"; break;
    }
    where = others.empty() ? std::string(side_panel_name(moving)) + " " + where
      : "Together with " + others + " " + where;
    auto* list = gui::get_foreground_draw_list();
    list->AddRectFilled(min, max, 0x40000000u, 8.f);
    list->AddRectFilled(a, b, accent_alpha(accent_hover_u32, 0x50), 8.f);
    list->AddRect(a, b, accent_hover_u32, 8.f, 0, 2.f);
    fan::vec2 text_size = gui::calc_text_size(where);
    list->AddText((a + b) * 0.5f - text_size * 0.5f, 0xffffffffu, where.c_str());
    gui::set_mouse_cursor(gui::mouse_cursor_resize_all);
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
  void render_chat_pending_files() {
    if (chat_pending_files.empty()) {
      return;
    }
    const f32_t height = chat_pending_height;
    std::size_t remove = (std::size_t)-1;
    for (std::size_t i = 0; i < chat_pending_files.size(); ++i) {
      const std::string& path = chat_pending_files[i];
      std::string name = ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(path).filename());
      if (i) {
        gui::same_line(0, 8.f);
      }
      fan::vec2 at = gui::get_cursor_screen_pos();
      fan::vec2 size(height, height);
      bool picture_shown = false;
      if (chat_is_picture_name(name)) {
        auto& picture = chat_picture_for(path);
        if (picture.ready) {
          f32_t scale = std::min(height / std::max(picture.size.y, 1.f), 2.f * height / std::max(picture.size.x, 1.f));
          size = picture.size * scale;
          gui::image(picture.image, size);
          picture_shown = true;
        }
      }
      if (!picture_shown) {
        // a file that isn't a picture (or one still loading): a card with its name
        size = fan::vec2(std::clamp(gui::calc_text_size(name).x + 20.f, height, 2.f * height), height);
        gui::dummy(size);
        auto* list = gui::get_window_draw_list();
        list->AddRectFilled(at, at + size, 0xff1a1310u, 6.f);
        gui::push_clip_rect(at, at + size, true);
        list->AddText(at + fan::vec2(10.f, size.y * 0.5f - gui::get_font_size() * 0.5f), 0xffd0d0d0u, name.c_str());
        gui::pop_clip_rect();
      }
      if (gui::is_item_hovered()) {
        gui::set_tooltip(name + "\nEnter or Send sends it");
      }
      // the x in the corner takes it out
      fan::vec2 x_at = at + fan::vec2(size.x - 20.f, 2.f);
      gui::set_cursor_screen_pos(x_at);
      if (gui::invisible_button("##chat_pending_remove" + std::to_string(i), fan::vec2(18.f, 18.f))) {
        remove = i;
      }
      bool x_hovered = gui::is_item_hovered();
      auto* list = gui::get_window_draw_list();
      fan::vec2 c = x_at + fan::vec2(9.f, 9.f);
      list->AddCircleFilled(c, 9.f, x_hovered ? 0xff4040d0u : 0xc0000000u);
      list->AddLine(c - fan::vec2(3.5f, 3.5f), c + fan::vec2(3.5f, 3.5f), 0xffffffffu, 1.5f);
      list->AddLine(c + fan::vec2(-3.5f, 3.5f), c + fan::vec2(3.5f, -3.5f), 0xffffffffu, 1.5f);
      if (x_hovered) {
        gui::set_tooltip("Don't send " + name);
      }
      gui::set_cursor_screen_pos(at + fan::vec2(size.x, 0));
      gui::dummy(fan::vec2(0, size.y));
    }
    if (remove != (std::size_t)-1) {
      chat_pending_files.erase(chat_pending_files.begin() + remove);
    }
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
    chat_url_clicked = false;
    if (!ecps_backend.chat_known(channel_id)) {
      gui::text_wrapped_disabled("Connecting to the chat... If this stays, the server is too old for chat and needs updating.");
      return;
    }

    // dropped or attached files wait above the input for Enter or Send
    if (!chat_dropped_files.empty()) {
      chat_pending_files.insert(chat_pending_files.end(), chat_dropped_files.begin(), chat_dropped_files.end());
      chat_dropped_files.clear();
      chat_focus_input = true;
    }

    auto messages = ecps_backend.chat_of(channel_id);
    render_chat_pins(channel_id, messages);
    bool replying = chat_reply_id != 0 && chat_reply_channel == (std::uint32_t)channel_id.i;
    // the input grows with its lines, up to six
    std::size_t input_lines = std::clamp<std::size_t>(1 + (std::size_t)std::count(chat_input.begin(), chat_input.end(), '\n'), 1, 6);
    f32_t input_box = input_lines * gui::get_text_line_height() + gui::get_style().FramePadding.y * 2;
    f32_t input_height = input_box + gui::get_style().ItemSpacing.y * 2;
    if (!chat_pending_files.empty()) {
      input_height += chat_pending_height + gui::get_style().ItemSpacing.y;
    }
    if (replying) {
      input_height += gui::get_frame_height() + gui::get_style().ItemSpacing.y;
    }
    fan::vec2 avail = gui::get_content_region_avail();
    chat_lines.clear();
    if (gui::begin_child("##chat_log", fan::vec2(0, std::max(avail.y - input_height, 80.f)), true)) {
      if (messages.empty()) {
        gui::text_wrapped_disabled("No messages yet. Text, pasted text and files go to everyone in this channel.");
      }
      std::uint32_t last_from = (std::uint32_t)-1;
      std::int64_t last_time = 0;
      bool first_group = true;
      std::vector<chat_pin_t> pins = chat_pins(messages);
      std::uint32_t hovered_now = 0;
      // also while a button is held down: otherwise pressing one made the log "not hovered", the toolbar went away
      // before the mouse came up, and the click never finished
      bool log_hovered = gui::is_window_hovered(gui::hovered_flags_child_windows | gui::hovered_flags_allow_when_blocked_by_active_item);
      for (const auto& message : messages) {
        // a run of messages from the same person within a few minutes shares one name line
        chat_line_message = message.id;
        chat_line_number = 0;
        chat_tag_t tag = chat_parse_tag(message.text);
        // a pin or unpin is a short grey line of its own, a click on a pin goes to the message
        if (tag.kind == chat_tag_t::pin || tag.kind == chat_tag_t::unpin) {
          gui::spacing();
          std::string line = message.name + (tag.kind == chat_tag_t::pin ? " pinned: " + tag.quote : std::string(" unpinned a message"));
          gui::text_disabled(line);
          if (tag.kind == chat_tag_t::pin && gui::is_item_hovered()) {
            gui::set_mouse_cursor(gui::mouse_cursor_hand);
            gui::set_tooltip("Go to the message");
            if (fan::window::is_mouse_clicked(0)) {
              chat_jump_id = tag.id;
              chat_url_clicked = true;
            }
          }
          chat_add_line(line, "\n", true, gui::get_item_rect_min(), gui::get_item_rect_max());
          last_from = (std::uint32_t)-1;
          continue;
        }
        if (message.from != last_from || message.time - last_time > 300) {
          // a blank line between messages, so a new message never reads as another line of the last one
          if (!first_group) {
            gui::dummy(fan::vec2(0.f, gui::get_text_line_height() * 0.6f));
          }
          first_group = false;
          gui::spacing();
          bool mine = message.from == (std::uint32_t)ecps_backend.session_id.i;
          // a short bar marks where a new message begins, so a run of lines from one person reads as one message
          fan::vec2 group_at = gui::get_cursor_screen_pos();
          gui::get_window_draw_list()->AddRectFilled(group_at + fan::vec2(-4.f, 2.f),
            group_at + fan::vec2(-2.f, 2.f + gui::get_font_size()), mine ? accent_u32 : 0xffffa070u, 1.f);
          gui::text(mine ? fan::color(0.55f, 0.8f, 1.f, 1.f) : fan::color(0.95f, 0.8f, 0.45f, 1.f), message.name);
          fan::vec2 name_min = gui::get_item_rect_min();
          std::string time = chat_time_label(message.time);
          // time sits at the right edge of the same line as the name
          gui::same_line(0, 0.f);
          gui::set_cursor_pos_x(gui::get_cursor_pos_x() + gui::get_content_region_avail().x - gui::calc_text_size(time).x);
          gui::text_disabled(time);
          chat_add_line(message.name + "  " + time, "\n", true, name_min, gui::get_item_rect_max());
        }
        last_from = message.from;
        last_time = message.time;
        if (chat_jump_id == message.id) {
          gui::set_scroll_here_y();
          chat_jump_id = 0;
          chat_flash_id = message.id;
          chat_flash_at = std::chrono::steady_clock::now();
        }
        bool pinned = std::find_if(pins.begin(), pins.end(), [&](const chat_pin_t& p) { return p.id == message.id; }) != pins.end();
        fan::vec2 top = gui::get_cursor_screen_pos();
        f32_t row_right = top.x + gui::get_content_region_avail().x;
        // the text a little in from the name, so where one person's run starts is easy to see
        gui::indent(chat_text_indent);
        if (tag.kind == chat_tag_t::reply) {
          render_chat_reply_quote(tag);
        }
        render_chat_text(tag.body, message.id, message.from);
        if (message.edited) {
          gui::text_disabled("(edited)");
        }
        gui::unindent(chat_text_indent);
        if (chat_hover_id == message.id) {
          render_chat_message_buttons(channel_id, message, tag, pinned, top);
        }
        fan::vec2 bottom(row_right, gui::get_cursor_screen_pos().y);
        top.x -= 6.f;
        auto* list = gui::get_window_draw_list();
        // the toolbar can reach below a one-line message, so the mouse on it still counts as over the message
        fan::vec2 hover_bottom(bottom.x, std::max(bottom.y, top.y + gui::get_frame_height()));
        if (log_hovered && gui::is_mouse_hovering_rect(top - fan::vec2(0, 2.f), hover_bottom)) {
          hovered_now = message.id;
          list->AddRectFilled(top - fan::vec2(0, 2.f), bottom, 0x0cffffffu, 4.f);
        }
        if (pinned) {
          list->AddRectFilled(fan::vec2(top.x, top.y), fan::vec2(top.x + 2.f, bottom.y - 3.f), 0xff40b0ffu, 1.f);
        }
        if (chat_flash_id == message.id) {
          f32_t t = std::chrono::duration<f32_t>(std::chrono::steady_clock::now() - chat_flash_at).count();
          if (t < 1.6f) {
            std::uint32_t alpha = (std::uint32_t)(0x50 * (1.f - t / 1.6f));
            list->AddRectFilled(top - fan::vec2(0, 2.f), bottom, (alpha << 24) | (accent_hover_u32 & 0x00ffffffu), 4.f);
          }
          else {
            chat_flash_id = 0;
          }
        }
      }
      // new messages, or another channel opened: down to the newest
      std::uint64_t revision = ecps_backend.chat_revision.load();
      // a picture that came in grows the log: followed down only when the log was near the bottom already
      bool picture_down = chat_scroll_down && gui::get_scroll_y() >= gui::get_scroll_max_y() - 700.f;
      chat_scroll_down = false;
      if (picture_down || revision != chat_seen_revision || chat_seen_channel != (std::uint32_t)channel_id.i) {
        chat_seen_revision = revision;
        chat_seen_channel = (std::uint32_t)channel_id.i;
        gui::set_scroll_here_y();
      }
      // a jump to a message no longer kept here goes nowhere
      chat_jump_id = 0;
      chat_hover_id = hovered_now;
      chat_update_mark(messages);
    }
    gui::end_child();

    render_chat_pending_files();

    bool editing = chat_edit_id != 0 && chat_edit_channel == (std::uint32_t)channel_id.i;
    if (editing) {
      gui::align_text_to_frame_padding();
      gui::text_disabled("Editing your message");
      gui::same_line();
      gui::set_cursor_pos_x(gui::get_cursor_pos_x() + std::max(0.f, gui::get_content_region_avail().x - gui::get_frame_height()));
      // Escape in the input leaves it, the words typed so far go
      if (icon_button("##chat_edit_cancel", icon_e::close, "Stop editing (Esc)", gui::get_frame_height()) ||
        (chat_input_active && engine.window.is_key_clicked(fan::key_escape))) {
        chat_edit_id = 0;
        chat_input.clear();
        editing = false;
      }
    }
    if (replying) {
      gui::align_text_to_frame_padding();
      gui::text_disabled("Replying to");
      gui::same_line(0, 4.f);
      gui::text(fan::color(0.95f, 0.8f, 0.45f, 1.f), chat_reply_name);
      gui::same_line(0, 8.f);
      f32_t room = gui::get_content_region_avail().x - gui::get_frame_height() - 12.f;
      gui::text_disabled(gui::fit_title(chat_reply_quote, std::max(room, 40.f)));
      gui::same_line();
      gui::set_cursor_pos_x(gui::get_cursor_pos_x() + std::max(0.f, gui::get_content_region_avail().x - gui::get_frame_height()));
      // Escape in the input drops the reply too
      if (icon_button("##chat_reply_cancel", icon_e::close, "Stop replying (Esc)", gui::get_frame_height()) ||
        (chat_input_active && engine.window.is_key_clicked(fan::key_escape))) {
        chat_reply_id = 0;
        replying = false;
      }
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
    // Ctrl+Alt+Enter sends the text as a code block. Read before the field, which takes Ctrl+Enter as a new line
    bool alt = engine.window.is_key_down(fan::key_left_alt) || engine.window.is_key_down(fan::key_right_alt);
    bool send_as_code = chat_input_active && ctrl && alt && !shift && engine.window.is_key_clicked(fan::key_enter);
    bool enter = gui::input_text_multiline("##chat_input", &chat_input, fan::vec2(0, input_box), input_flags) && !shift;
    chat_input_active = gui::is_item_active();
    if (send_as_code) {
      chat_input = before;
      while (!chat_input.empty() && (chat_input.back() == '\n' || chat_input.back() == '\r')) {
        chat_input.pop_back();
      }
      if (!chat_input.empty() && chat_input.find("```") == std::string::npos) {
        chat_input = "```\n" + chat_input + "\n```";
      }
      enter = !chat_input.empty();
    }
    gui::pop_item_width();
    // pasted files wait above the input, like dropped ones
    if (!pasted_files.empty()) {
      chat_input = before;
      chat_pending_files.insert(chat_pending_files.end(), pasted_files.begin(), pasted_files.end());
    }
    // pasted text too long for a message waits as a file too
    else if (chat_input.size() > chat_inline_max && chat_has_text_content(chat_input)) {
      chat_pending_files.push_back(chat_text_to_file(chat_input));
      chat_input.clear();
      // the field keeps its own copy of the text while it is being typed in: without letting go of it, it hands the
      // same text back next frame and every frame made one more file. Focus comes back to it right after
      gui::clear_active_id();
      chat_focus_input = true;
    }

    gui::same_line();
    bool send = gui::button("Send##chat_send");
    bool has_text = chat_has_text_content(chat_input);
    if (editing && (enter || send) && has_text) {
      // new words for the message that was picked. Nothing left in it is the Delete button's job
      backend_queue([channel_id, id = chat_edit_id, text = chat_edit_prefix + chat_input]() -> fan::event::task_t {
        return ecps_backend.change_chat(channel_id, id, text);
      });
      chat_edit_id = 0;
      chat_input.clear();
      chat_focus_input = true;
    }
    else if ((enter || send) && (has_text || !chat_pending_files.empty())) {
      if (replying && has_text) {
        // the tag line goes in front, a ] in the name would end the tag early
        std::string name = chat_reply_name;
        std::replace(name.begin(), name.end(), ']', ')');
        chat_send_text(channel_id, "[reply #" + std::to_string(chat_reply_id) + " " + name + "] " + chat_reply_quote + "\n" + chat_input);
        chat_reply_id = 0;
      }
      else {
        chat_send_text(channel_id, chat_input);
      }
      chat_input.clear();
      if (!chat_pending_files.empty()) {
        chat_send_files(channel_id, chat_pending_files);
        chat_pending_files.clear();
      }
      chat_focus_input = true;
    }
  }

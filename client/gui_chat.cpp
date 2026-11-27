// Chat implementation, out of line so a chat edit does not recompile the whole client.
#include "gui_std.h" // precompiled, first
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "ecps_app.h"


// the address a phone on the same network can open: the first private IPv4 of this computer
static std::string phone_upload_local_ip() {
#if defined(_WIN32)
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return "127.0.0.1";
  }
#endif
  std::string fallback;
  char host[256] = {};
  if (gethostname(host, sizeof(host)) == 0) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) == 0) {
      for (auto* it = result; it; it = it->ai_next) {
        auto* ipv4 = (sockaddr_in*)it->ai_addr;
        char text[64] = {};
        if (!inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text))) {
          continue;
        }
        std::string ip = text;
        if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0) {
          continue;
        }
        if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0) {
          fallback = ip;
          break;
        }
        if (fallback.empty()) {
          fallback = ip;
        }
      }
      freeaddrinfo(result);
    }
  }
  return fallback.empty() ? "127.0.0.1" : fallback;
}

void render_thread_t::ecps_gui_t::update_phone_upload() {
    auto joined = [this](std::uint16_t id) {
      ecps_backend_t::Protocol_ChannelID_t channel_id;
      channel_id.i = id;
      return ecps_backend.is_channel_joined(channel_id);
    };
    std::uint16_t selected = selected_channel_id.i;
    ecps_backend.web_upload_channel.store(joined(selected) ? selected : (std::uint16_t)-1);
    if (phone_upload_enabled) {
      ecps_backend.web_upload_enabled.store(true);
      if (phone_upload_token.empty()) {
        std::random_device device;
        std::mt19937 generator(device());
        std::uniform_int_distribution<int> digit(0, 15);
        const char* hex = "0123456789abcdef";
        for (int i = 0; i < 24; ++i) {
          phone_upload_token += hex[digit(generator)];
        }
      }
      if (phone_upload_url.empty()) {
        // each step in the log: a phone upload that stopped the program can be followed to where
        fan::print("phone upload: looking up this computer's address");
        phone_upload_url = "http://" + phone_upload_local_ip() + ":52123/" + phone_upload_token;
        fan::print("phone upload: address found");
      }
      if (!phone_upload_started) {
        phone_upload_started = true;
        std::string token = phone_upload_token;
        backend_queue([token]() -> fan::event::task_t {
          fan::print("phone upload: starting the page on port 52123");
          ecps_backend.start_web_upload(52123, token);
          fan::print(ecps_backend.web_upload_enabled.load() ? "phone upload: page started" : "phone upload: the page could not start");
          co_return;
        });
      }
    }
    else {
      ecps_backend.stop_web_upload();
    }
    for (auto& upload : ecps_backend.take_web_uploads()) {
      ecps_backend_t::Protocol_ChannelID_t channel_id;
      channel_id.i = upload.first;
      if (ecps_backend.is_channel_joined(channel_id)) {
        chat_send_files(channel_id, { upload.second });
      }
    }
  }

void render_thread_t::ecps_gui_t::render_emoji_picker() {
    if (!recent_emojis_loaded) {
      recent_emojis_loaded = true;
      if (config.contains("recent_emojis") && config["recent_emojis"].is_array()) {
        for (const auto& value : config["recent_emojis"]) {
          if (value.is_number()) {
            recent_emojis.push_back(value.get<std::uint32_t>());
          }
        }
      }
    }
    gui::set_next_window_size(fan::vec2(gui::get_frame_height() * 13.f, gui::get_frame_height() * 12.f));
    if (!gui::begin_popup("##emoji_picker")) {
      return;
    }
    struct category_t {
      const char* name;
      std::uint32_t icon; // its tab: the names cut down to "S..." in a picker this narrow
      std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
    };
    static const std::vector<category_t> categories = {
      {"Smileys", 0x1F600, {{0x1F600, 0x1F64F}, {0x1F910, 0x1F92F}, {0x1F970, 0x1F97A}, {0x1FAE0, 0x1FAE8}}},
      {"People", 0x1F44B, {{0x1F446, 0x1F450}, {0x1F466, 0x1F487}, {0x1F90C, 0x1F90F}, {0x1F918, 0x1F91F}, {0x1F930, 0x1F93A},
        {0x1F9B0, 0x1F9B9}, {0x1F9CD, 0x1F9DF}, {0x1FAF0, 0x1FAF8}, {0x1F440, 0x1F445}}},
      {"Nature", 0x1F33F, {{0x1F330, 0x1F344}, {0x1F400, 0x1F43F}, {0x1F980, 0x1F9AE}, {0x1FAB0, 0x1FABD}, {0x1F300, 0x1F32C}}},
      {"Food", 0x1F355, {{0x1F345, 0x1F37F}, {0x1F950, 0x1F96F}, {0x1F9C0, 0x1F9CB}, {0x1FAD0, 0x1FADB}, {0x2615, 0x2615}}},
      {"Activities", 0x26BD, {{0x1F380, 0x1F3D3}, {0x1F93C, 0x1F94F}, {0x26BD, 0x26BE}, {0x1FA80, 0x1FA8F}}},
      {"Travel", 0x2708, {{0x1F680, 0x1F6FF}, {0x1F3D4, 0x1F3F0}, {0x1F5FA, 0x1F5FF}}},
      {"Objects", 0x1F4A1, {{0x1F4A0, 0x1F4FF}, {0x1F500, 0x1F579}, {0x1F9E0, 0x1F9FF}, {0x1FA70, 0x1FA7C}, {0x1FA90, 0x1FAAF}}},
      {"Symbols", 0x2764, {{0x2764, 0x2764}, {0x1F493, 0x1F49F}, {0x2600, 0x27BF}, {0x2B00, 0x2BFF}, {0x1F7E0, 0x1F7EB}}},
    };
    f32_t cell = gui::get_frame_height() * 1.25f;
    auto grid = [&](const std::vector<std::uint32_t>& codes) {
      f32_t width = gui::get_content_region_avail().x;
      int columns = std::max(1, (int)(width / cell));
      int index = 0;
      auto* list = gui::get_window_draw_list();
      for (std::uint32_t code : codes) {
        if (index % columns != 0) {
          gui::same_line(0, 0);
        }
        ++index;
        std::string emoji = utf8_of_codepoint(code);
        fan::vec2 at = gui::get_cursor_screen_pos();
        gui::push_id((int)code);
        bool clicked = gui::invisible_button("##emoji", fan::vec2(cell, cell));
        gui::pop_id();
        if (gui::is_item_hovered()) {
          list->AddRectFilled(at, at + fan::vec2(cell, cell), gui::widget_palette.surface_hover, 6.f);
          for (const auto& [name, value] : emoji_shortcodes()) {
            if (value == code) {
              gui::set_tooltip(std::string(":") + name + ":");
              break;
            }
          }
        }
        fan::vec2 text = gui::calc_text_size(emoji);
        list->AddText(at + (fan::vec2(cell, cell) - text) * 0.5f, 0xffffffffu, emoji.c_str());
        if (clicked) {
          chat_input += emoji;
          // the newest first, each once, the last 24
          recent_emojis.erase(std::remove(recent_emojis.begin(), recent_emojis.end(), code), recent_emojis.end());
          recent_emojis.insert(recent_emojis.begin(), code);
          if (recent_emojis.size() > 24) {
            recent_emojis.resize(24);
          }
          fan::json saved = fan::json::array();
          for (std::uint32_t value : recent_emojis) {
            saved.push_back(value);
          }
          write_to_config("recent_emojis", saved);
        }
      }
    };
    if (!recent_emojis.empty()) {
      gui::text_disabled("Recent");
      grid(recent_emojis);
      gui::separator();
    }
    if (gui::begin_tab_bar("##emoji_categories")) {
      for (const auto& category : categories) {
        bool open = gui::begin_tab_item(utf8_of_codepoint(category.icon) + "##" + category.name);
        if (gui::is_item_hovered()) {
          gui::set_tooltip(category.name);
        }
        if (open) {
          gui::text_disabled(category.name);
          // only what the font has: codes in a range without an emoji, or one this font size doesn't carry, are left out
          std::vector<std::uint32_t> codes;
          for (const auto& [first, last] : category.ranges) {
            for (std::uint32_t code = first; code <= last; ++code) {
              if (gui::has_glyph(code)) {
                codes.push_back(code);
              }
            }
          }
          gui::begin_child("##emoji_grid", fan::vec2(0, 0), false);
          grid(codes);
          gui::end_child();
          gui::end_tab_item();
        }
      }
      gui::end_tab_bar();
    }
    gui::end_popup();
  }

void render_thread_t::ecps_gui_t::render_phone_upload_popup() {
    if (phone_upload_popup_requested) {
      phone_upload_popup_requested = false;
      fan::print("phone upload: window opened");
      gui::open_popup("##phone_upload");
    }
    // a set width: an auto sized popup is 0 wide on its first frame, and the wrapped text has nothing to wrap at
    if (gui::is_popup_open("##phone_upload")) {
      gui::set_next_window_size(fan::vec2(gui::get_font_size() * 24.f, 0.f));
    }
    if (!gui::begin_popup("##phone_upload")) {
      return;
    }
    gui::text("Send a picture from your phone");
    if (gui::settings_switch("Phone upload", &phone_upload_enabled,
      "Pictures taken or picked on the phone go to the channel open in ECPS")) {
      if (!phone_upload_enabled) {
        ecps_backend.stop_web_upload();
      }
      save_ui_config();
    }
    if (phone_upload_enabled) {
      if (phone_upload_url.empty()) {
        update_phone_upload();
      }
      gui::text_wrapped_disabled("Open this on the phone. Both must be on the same Wi-Fi:");
      gui::text_wrapped(phone_upload_url);
      if (gui::button("Copy link##phone_upload_copy")) {
        owner->engine.window.set_clipboard(phone_upload_url);
      }
      if (!ecps_backend.is_channel_joined(selected_channel_id)) {
        settings_warning("Join a channel first: the pictures go to the open channel.");
      }
      settings_hint("If the page does not open, allow ECPS through Windows Firewall.");
    }
    gui::end_popup();
  }


void render_thread_t::ecps_gui_t::render_chat_tab(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    chat_tab_seen_at = std::chrono::steady_clock::now();
    chat_url_clicked = false;
    if (!ecps_backend.chat_known(channel_id)) {
      gui::text_wrapped_disabled("Connecting to the chat... If this stays, the server is too old for chat and needs updating.");
      return;
    }

    // a pasted picture was saved off the render thread: it waits above the input now
    for (auto& image : clipboard_image_take()) {
      chat_pending_files.push_back(std::move(image));
      chat_focus_input = true;
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
            group_at + fan::vec2(-2.f, 2.f + gui::get_font_size()), mine ? accent_u32 : 0xff73ccf2u, 1.f);
          gui::text(mine ? gui::palette_color(gui::widget_palette.accent_text) : fan::color(0.95f, 0.8f, 0.45f, 1.f), message.name);
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
        if (chat_reply_request == message.id) {
          chat_reply_request = 0;
          chat_reply_id = message.id;
          chat_reply_channel = (std::uint32_t)channel_id.i;
          chat_reply_name = message.name;
          chat_reply_quote = chat_snippet(tag.body);
          chat_edit_id = 0;
          chat_focus_input = true;
        }
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
          // only a thin line at the left, a filled background over the whole message was too loud
          if (!pinned) {
            list->AddRectFilled(fan::vec2(top.x, top.y), fan::vec2(top.x + 2.f, bottom.y - 3.f), 0x30ffffffu, 1.f);
          }
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
      // scrolled up to read, new messages leave the log where it is (one's own still goes down)
      bool near_bottom = gui::get_scroll_y() >= gui::get_scroll_max_y() - 60.f;
      bool channel_changed = chat_seen_channel != (std::uint32_t)channel_id.i;
      bool new_messages = revision != chat_seen_revision;
      bool mine_last = !messages.empty() && messages.back().from == (std::uint32_t)ecps_backend.session_id.i;
      if (channel_changed) {
        chat_new_below = 0;
        chat_newest_id = 0;
      }
      if (picture_down || channel_changed || chat_jump_latest || (new_messages && (near_bottom || mine_last))) {
        gui::set_scroll_here_y();
        chat_new_below = 0;
        chat_jump_latest = false;
      }
      else if (new_messages) {
        for (const auto& message : messages) {
          chat_new_below += message.id > chat_newest_id;
        }
      }
      else if (near_bottom) {
        chat_new_below = 0;
      }
      chat_seen_revision = revision;
      chat_seen_channel = (std::uint32_t)channel_id.i;
      if (!messages.empty()) {
        chat_newest_id = std::max(chat_newest_id, messages.back().id);
      }
      if (!near_bottom && !channel_changed) {
        std::string label = chat_new_below > 0 ? std::to_string(chat_new_below) + (chat_new_below == 1 ? " new message" : " new messages")
          : std::string("Jump to latest");
        label += "##chat_jump_latest";
        f32_t width = gui::calc_text_size(label.substr(0, label.find("##"))).x + gui::get_style().FramePadding.x * 2.f;
        fan::vec2 window_at = gui::get_window_pos();
        fan::vec2 window_size = gui::get_window_size();
        gui::set_cursor_screen_pos(fan::vec2(window_at.x + (window_size.x - width) * 0.5f,
          window_at.y + window_size.y - gui::get_frame_height() - 10.f));
        if (gui::button(label.c_str())) {
          chat_jump_latest = true;
        }
      }
      // the click came after the message it points to was drawn this frame (a reply points to an earlier one), so it
      // gets one more frame to find it. A message no longer kept here goes nowhere
      if (chat_jump_id != 0 && !chat_jump_retried) {
        chat_jump_retried = true;
      }
      else {
        chat_jump_id = 0;
        chat_jump_retried = false;
      }
      chat_hover_id = hovered_now;
      chat_update_mark(messages);
      render_chat_message_menu(channel_id, messages, pins);
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
        (chat_input_active && owner->engine.window.is_key_clicked(fan::key_escape))) {
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
        (chat_input_active && owner->engine.window.is_key_clicked(fan::key_escape))) {
        chat_reply_id = 0;
        replying = false;
      }
    }

    // ---- the input: attach, text, send (phone upload was taken out of the window)
    if (icon_button("##chat_attach", icon_e::send_file, "Send a file to everyone here", 0.f)) {
      fan::graphics::open_file("", [this, channel_id](std::string_view path) {
        chat_dropped_files.push_back(std::string(path));
      });
    }
    gui::same_line();
    // the emoji picker: a face in the button, a grid of emojis in the popup, a click adds one to the message
    {
      f32_t size = gui::get_frame_height();
      fan::vec2 at = gui::get_cursor_screen_pos();
      if (gui::invisible_button("##chat_emoji", fan::vec2(size, size))) {
        gui::open_popup("##emoji_picker");
      }
      bool hovered = gui::is_item_hovered();
      if (hovered) {
        gui::set_tooltip("Emojis (or type :fire:, :thumbsup:, :joy: ...)");
      }
      auto* list = gui::get_window_draw_list();
      list->AddRectFilled(at, at + fan::vec2(size, size), hovered ? gui::widget_palette.surface_hover : gui::widget_palette.surface, size * 0.28f);
      std::string face = utf8_of_codepoint(0x1F600);
      fan::vec2 text = gui::calc_text_size(face);
      list->AddText(at + (fan::vec2(size, size) - text) * 0.5f, 0xffffffffu, face.c_str());
      render_emoji_picker();
    }
    gui::same_line();
    f32_t send_width = gui::calc_text_size("Send").x + gui::get_style().FramePadding.x * 2;
    gui::push_item_width(gui::get_content_region_avail().x - send_width - gui::get_style().ItemSpacing.x);

    // Ctrl+V with files on the clipboard sends the files, the text field doesn't get their paths
    bool ctrl = owner->engine.window.is_key_down(fan::key_left_control) || owner->engine.window.is_key_down(fan::key_right_control);
    bool pasting = chat_input_active && ctrl && owner->engine.window.is_key_clicked(fan::key_v);
    std::string before = chat_input;
    std::vector<std::string> pasted_files;
    bool pasted_image = false;
    if (pasting) {
      pasted_files = chat_clipboard_files();
      if (pasted_files.empty()) {
        // a screenshot or a copied picture: saving it happens in the background, so the stream keeps drawing
        clipboard_image_begin();
        pasted_image = clipboard_has_image();
      }
    }

    if (chat_focus_input) {
      gui::set_keyboard_focus_here();
      chat_focus_input = false;
    }
    // Enter sends, Shift+Enter is a new line: with Shift held the field takes Enter as a line of its own
    bool shift = owner->engine.window.is_key_down(fan::key_left_shift) || owner->engine.window.is_key_down(fan::key_right_shift);
    gui::input_text_flags_t input_flags = shift ? 0 :
      (gui::input_text_flags_enter_returns_true | gui::input_text_flags_ctrl_enter_for_new_line);
    // with Shift held the field returns true for any change (Enter-returns-true is off then): only without it is it a send
    // Ctrl+Alt+Enter sends the text as a code block. Read before the field, which takes Ctrl+Enter as a new line
    bool alt = owner->engine.window.is_key_down(fan::key_left_alt) || owner->engine.window.is_key_down(fan::key_right_alt);
    bool send_as_code = chat_input_active && ctrl && alt && !shift && owner->engine.window.is_key_clicked(fan::key_enter);
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
    // pasted files wait above the input, like dropped ones. A picture isn't ready yet: it joins them when the
    // background read finishes (render_chat_tab takes it in)
    if (!pasted_files.empty() || pasted_image) {
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
    if (enter || send) {
      chat_input = chat_expand_shortcodes(chat_input);
    }
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

void render_thread_t::ecps_gui_t::chat_update_mark(const std::vector<ecps_backend_t::chat_message_t>& messages) {
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
    bool ctrl = owner->engine.window.is_key_down(fan::key_left_control) || owner->engine.window.is_key_down(fan::key_right_control);
    if (chat_has_mark && !gui::get_io().WantTextInput && ctrl && owner->engine.window.is_key_clicked(fan::key_c)) {
      owner->engine.window.set_clipboard(chat_marked_text());
    }
    // right click: a menu for the message under the mouse (reply, edit, copy, pin, delete). Copy takes the marked
    // words, or without a mark the whole message
    if (over_text && fan::window::is_mouse_clicked(1)) {
      if (int i = chat_line_index(chat_hit(m).key); i >= 0) {
        chat_menu_id = chat_lines[i].message_id;
        chat_menu_selection = chat_has_mark ? chat_marked_text() : std::string();
        gui::open_popup("##chat_menu");
      }
    }
  }
void render_thread_t::ecps_gui_t::render_chat_viewer() {
    if (chat_viewer_keep_settings_frames > 0) {
      --chat_viewer_keep_settings_frames;
      owner->engine.get_render_settings_menu() = chat_viewer_keep_settings_state;
    }
    if (chat_viewer_path.empty()) {
      return;
    }
    auto found = chat_pictures.find(chat_viewer_path);
    bool escape = owner->engine.window.is_key_clicked(fan::key_escape);
    if (found == chat_pictures.end() || !found->second.ready || escape) {
      if (escape) {
        chat_viewer_keep_settings_state = owner->engine.get_render_settings_menu();
        chat_viewer_keep_settings_frames = 2;
      }
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
void render_thread_t::ecps_gui_t::chat_send_pin(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t id, const std::string& text, bool pin) {
    chat_send_text(channel_id, pin ? "[pin #" + std::to_string(id) + "] " + chat_snippet(text) : "[unpin #" + std::to_string(id) + "]");
  }

void render_thread_t::ecps_gui_t::render_chat_pins(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::vector<ecps_backend_t::chat_message_t>& messages) {
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

void render_thread_t::ecps_gui_t::render_chat_reply_quote(const chat_tag_t& tag) {
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

void render_thread_t::ecps_gui_t::render_chat_message_buttons(ecps_backend_t::Protocol_ChannelID_t channel_id, const ecps_backend_t::chat_message_t& message,
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

void render_thread_t::ecps_gui_t::render_chat_message_menu(ecps_backend_t::Protocol_ChannelID_t channel_id, const std::vector<ecps_backend_t::chat_message_t>& messages,
    const std::vector<chat_pin_t>& pins) {
    if (!gui::begin_popup("##chat_menu")) {
      return;
    }
    auto found = std::find_if(messages.begin(), messages.end(), [&](const auto& m) { return m.id == chat_menu_id; });
    if (found == messages.end()) {
      gui::end_popup();
      return;
    }
    ecps_backend_t::chat_message_t message = *found;
    chat_tag_t tag = chat_parse_tag(message.text);
    bool mine = message.from == (std::uint32_t)ecps_backend.session_id.i;
    bool can_edit = mine && (tag.kind == chat_tag_t::none || tag.kind == chat_tag_t::reply);
    bool can_delete = mine || ecps_backend.is_current_user_host_of_channel(channel_id);
    bool pinned = std::find_if(pins.begin(), pins.end(), [&](const chat_pin_t& p) { return p.id == message.id; }) != pins.end();
    if (gui::menu_item("Reply")) {
      chat_reply_id = message.id;
      chat_reply_channel = (std::uint32_t)channel_id.i;
      chat_reply_name = message.name;
      chat_reply_quote = chat_snippet(tag.body);
      chat_edit_id = 0;
      chat_focus_input = true;
    }
    if (can_edit && gui::menu_item("Edit")) {
      chat_edit_id = message.id;
      chat_edit_channel = (std::uint32_t)channel_id.i;
      std::size_t line_end = message.text.find('\n');
      chat_edit_prefix = tag.kind == chat_tag_t::reply && line_end != std::string::npos ? message.text.substr(0, line_end + 1) : std::string();
      chat_input = message.text.substr(chat_edit_prefix.size());
      chat_reply_id = 0;
      chat_focus_input = true;
    }
    if (gui::menu_item(chat_menu_selection.empty() ? "Copy" : "Copy selection")) {
      owner->engine.window.set_clipboard(chat_menu_selection.empty() ? tag.body : chat_menu_selection);
    }
    if (gui::menu_item(pinned ? "Unpin" : "Pin")) {
      chat_send_pin(channel_id, message.id, tag.body, !pinned);
    }
    if (can_delete && gui::menu_item("Delete")) {
      backend_queue([channel_id, id = message.id]() -> fan::event::task_t {
        return ecps_backend.change_chat(channel_id, id, std::string());
      });
    }
    gui::end_popup();
  }

void render_thread_t::ecps_gui_t::chat_draw_link_line(const std::string& line, const std::string& line_id, const std::string& after) {
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
      gui::text(gui::palette_color(gui::widget_palette.accent_text), url);
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

void render_thread_t::ecps_gui_t::chat_plain_lines(const std::string& text, const std::string& id) {
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

void render_thread_t::ecps_gui_t::render_chat_file_actions(const std::string& text, std::uint32_t message_id, std::uint32_t from) {
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

bool render_thread_t::ecps_gui_t::render_chat_picture(const std::string& text, std::uint32_t message_id, std::uint32_t from) {
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
    // a download button in the top right corner while the mouse is over the picture, and a right-click menu
    f32_t icon_box = std::min(gui::get_frame_height(), std::min(size.x, size.y) * 0.5f);
    fan::vec2 icon_min(at.x + size.x - icon_box - 6.f, at.y + 6.f);
    fan::vec2 icon_max = icon_min + fan::vec2(icon_box, icon_box);
    bool on_icon = gui::is_mouse_hovering_rect(icon_min, icon_max);
    auto view = [&] {
      chat_viewer_path = path;
      chat_viewer_zoom = 1.f;
      chat_viewer_offset = 0;
    };
    if (gui::invisible_button("##chat_picture" + id, size)) {
      if (on_icon) {
        chat_save_copy(path);
      }
      else {
        view();
      }
    }
    if (gui::is_item_clicked(1)) {
      gui::open_popup("##chat_picture_menu" + id);
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip(on_icon ? "Save a copy of " + name : "Click to view " + name + ", right-click for more");
      auto* list = gui::get_window_draw_list();
      list->AddRect(at, at + size, 0x80ffffffu, 4.f);
      list->AddRectFilled(icon_min, icon_max, on_icon ? ((gui::widget_palette.surface_hover & 0x00ffffffu) | 0xe0000000u) : 0xb0000000u, 6.f);
      draw_icon(list, icon_e::download, (icon_min + icon_max) * 0.5f, icon_box * 0.6f, on_icon ? 0xffffffffu : 0xffd0d0d0u);
    }
    if (gui::begin_popup("##chat_picture_menu" + id)) {
      if (gui::menu_item("Reply")) {
        chat_reply_request = message_id;
      }
      if (gui::menu_item("View")) {
        view();
      }
      if (gui::menu_item("Save a copy")) {
        chat_save_copy(path);
      }
      gui::end_popup();
    }
    gui::text_wrapped_disabled(name + "  " + text.substr(paren + 2, text.size() - paren - 3));
    return true;
  }

void render_thread_t::ecps_gui_t::render_chat_text(const std::string& text, std::uint32_t message_id, std::uint32_t from) {
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
              owner->engine.window.set_clipboard(code);
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

void render_thread_t::ecps_gui_t::render_chat_pending_files() {
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
        list->AddRectFilled(at, at + size, gui::widget_palette.surface, 6.f);
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

void render_thread_t::ecps_gui_t::render_side_split(int side, fan::vec2 at, fan::vec2 size) {
    f32_t handle = 10.f;
    f32_t usable = size.y - handle;
    f32_t top = std::clamp(usable * side_split[side], 120.f, std::max(120.f, usable - 120.f));
    gui::set_cursor_screen_pos(at);
    gui::begin_child(("##side_panel_" + std::to_string(side)).c_str(), fan::vec2(size.x, top), true);
    render_side_panel_content(side, 0);
    gui::end_child();
    gui::set_cursor_screen_pos(fan::vec2(at.x, at.y + top));
    gui::push_id(side);
    gui::invisible_button("##side_split_handle", fan::vec2(size.x, handle));
    gui::pop_id();
    bool active = gui::is_item_active();
    bool hot = gui::is_item_hovered() || active;
    if (hot) {
      gui::set_mouse_cursor(gui::mouse_cursor_resize_ns);
    }
    if (active && usable > 0.f) {
      side_split_resizing = true;
      side_split[side] = std::clamp((gui::get_io().MousePos.y - at.y - handle * 0.5f) / usable, 0.15f, 0.85f);
    }
    else if (side_split_resizing && !fan::window::is_mouse_down(0)) {
      side_split_resizing = false;
      save_side_panel_layout();
    }
    f32_t mid = at.y + top + handle * 0.5f;
    gui::get_window_draw_list()->AddLine(fan::vec2(at.x + size.x * 0.35f, mid), fan::vec2(at.x + size.x * 0.65f, mid),
      hot ? accent_hover_u32 : 0x60ffffffu, hot ? 3.f : 2.f);
    gui::set_cursor_screen_pos(fan::vec2(at.x, at.y + top + handle));
    gui::begin_child(("##side_panel_" + std::to_string(side) + "_lower").c_str(), fan::vec2(size.x, size.y - top - handle), true);
    render_side_panel_content(side, 1);
    gui::end_child();
  }

void render_thread_t::ecps_gui_t::render_side_panel_content(int side, int group) {
    load_side_panel_layout();
    auto panels = panels_on(side, side < 0 ? -1 : group);
    if (panels.empty()) {
      return;
    }
    int shown = side_shown(side, group);
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
      if (gui::begin_tab_item("Camera", nullptr, 0)) {
        render_camera_tab();
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

void render_thread_t::ecps_gui_t::render_side_panel_handle(fan::vec2 size, int side) {
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
        gui::set_tooltip("Drop at the edge to hide; drag out to show");
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

void render_thread_t::ecps_gui_t::render_side_grip(fan::vec2 size, int side) {
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
        gui::set_tooltip("Show " + names + " (click or drag to size)");
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
    // a tab on the edge with an arrow pointing in, so a hidden side is seen and it's clear it opens
    // seen only with the mouse within reach of it (or while it is used), out of the way of the stream otherwise
    f32_t reach = 80.f;
    bool mouse_close = hot || (m.x > at.x - reach && m.x < at.x + size.x + reach && m.y > at.y - reach && m.y < at.y + size.y + reach);
    f32_t& shown_alpha = side_grip_alpha[side];
    shown_alpha += ((mouse_close ? 1.f : 0.f) - shown_alpha) * std::min(1.f, gui::get_io().DeltaTime * 12.f);
    if (shown_alpha < 0.01f) {
      return;
    }
    auto faded = [&](std::uint32_t color) {
      return (color & 0x00ffffffu) | ((std::uint32_t)((color >> 24) * shown_alpha) << 24);
    };
    auto* list = gui::get_window_draw_list();
    std::uint32_t ink = faded(hot ? 0xffffffffu : 0xb0ffffffu);
    std::uint32_t fill = faded(hot ? accent_alpha(accent_u32, 0x90) : 0x28ffffffu);
    fan::vec2 c = at + size * 0.5f;
    f32_t half_short = std::max((side == 1 ? size.y : size.x) * 0.5f - 1.f, 4.f);
    fan::vec2 half = side == 1 ? fan::vec2(36.f, half_short) : fan::vec2(half_short, 36.f);
    list->AddRectFilled(c - half, c + half, fill, half_short);
    fan::vec2 dir = side == 0 ? fan::vec2(-1.f, 0.f) : side == 1 ? fan::vec2(0.f, -1.f) : fan::vec2(1.f, 0.f);
    fan::vec2 across(-dir.y, dir.x);
    f32_t arm = std::min(half_short * 0.75f, 4.f);
    fan::vec2 tip = c + dir * (arm * 0.5f);
    list->AddLine(tip, tip - dir * arm + across * arm, ink, 1.8f);
    list->AddLine(tip, tip - dir * arm - across * arm, ink, 1.8f);
  }

void render_thread_t::ecps_gui_t::render_side_panel_drop() {
    if (side_panel_moving < 0) {
      return;
    }
    int moving = side_panel_moving;
    // a side whose upper half went empty: what's below moves up
    auto normalize = [&](int side) {
      if (panels_on(side, 0).empty()) {
        for (int panel = 0; panel < side_panel_count; ++panel) {
          if (side_of[panel] == side) {
            side_lower[panel] = false;
          }
        }
        side_front[side] = side_front_lower[side];
      }
    };
    if (!fan::window::is_mouse_down(0)) {
      int to = side_panel_drop;
      bool lower = to >= 0 && to != 1 && side_panel_drop_lower;
      if (to >= 0 && (to != side_of[moving] || lower != lower_of(moving))) {
        int from = side_of[moving];
        side_of[moving] = to;
        side_lower[moving] = lower;
        side_hidden[to] = false;
        front_of(moving) = moving;
        if (from != to) {
          normalize(from);
        }
        normalize(to);
        save_side_panel_layout();
      }
      side_panel_moving = -1;
      side_panel_drop = -1;
      side_panel_drop_lower = 0;
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
    // the names of the other panels on the drop side, in one half or both
    auto names = [&](int group) {
      std::string out;
      for (int panel : panels_on(side_panel_drop, group)) {
        if (panel != moving) {
          out += std::string(out.empty() ? "" : " and ") + side_panel_name(panel);
        }
      }
      return out;
    };
    // the left and right sides: the lower half of the side puts the panel under the ones there
    side_panel_drop_lower = side_panel_drop != 1 && !names(-1).empty() && m.y > min.y + size.y * 0.5f;
    std::string upper_names = side_panel_drop == 1 ? names(-1) : names(0);
    std::string lower_names = side_panel_drop == 1 ? std::string() : names(1);
    std::string others = side_panel_drop_lower ? lower_names : upper_names;
    fan::vec2 a, b;
    std::string where;
    switch (side_panel_drop) {
      case 0: a = fan::vec2(max.x - size.x * 0.28f, min.y); b = max; where = "on the right"; break;
      case 1: a = fan::vec2(min.x, max.y - size.y * 0.35f); b = max; where = "at the bottom"; break;
      default: a = min; b = fan::vec2(min.x + size.x * 0.28f, max.y); where = "on the left"; break;
    }
    if (side_panel_drop != 1) {
      if (side_panel_drop_lower) {
        a.y = min.y + size.y * 0.5f;
      }
      else if (!lower_names.empty()) {
        b.y = min.y + size.y * 0.5f;
      }
    }
    if (side_panel_drop_lower && others.empty()) {
      where = std::string(side_panel_name(moving)) + " below " + upper_names;
    }
    else if (!side_panel_drop_lower && others.empty() && !lower_names.empty()) {
      where = std::string(side_panel_name(moving)) + " above " + lower_names;
    }
    else {
      where = others.empty() ? std::string(side_panel_name(moving)) + " " + where
        : "Together with " + others + " " + where;
    }
    auto* list = gui::get_foreground_draw_list();
    list->AddRectFilled(min, max, 0x40000000u, 8.f);
    list->AddRectFilled(a, b, accent_alpha(accent_hover_u32, 0x50), 8.f);
    list->AddRect(a, b, accent_hover_u32, 8.f, 0, 2.f);
    fan::vec2 text_size = gui::calc_text_size(where);
    list->AddText((a + b) * 0.5f - text_size * 0.5f, 0xffffffffu, where.c_str());
    gui::set_mouse_cursor(gui::mouse_cursor_resize_all);
  }

// Stream overlay implementation, out of line.
#include "gui_std.h" // precompiled, first
#include "ecps_app.h"

void render_thread_t::ecps_gui_t::render_stream_users_overlay() {
    if (!show_stream_users) {
      return;
    }
    auto channel_id = selected_channel_id;
    if (!ecps_backend.is_channel_joined(channel_id) && !ecps_backend.channel_info.empty()) {
      channel_id = ecps_backend.channel_info.front().channel_id;
    }
    std::vector<std::string> lines;
    auto it = ecps_backend.channel_sessions.find((std::uint32_t)channel_id.i);
    if (it != ecps_backend.channel_sessions.end()) {
      for (const auto& session : it->second) {
        std::string name = session.username;
        if (session.session_id.i == ecps_backend.session_id.i) {
          name += " (you)";
        }
        if (session.is_host) {
          name += " (host)";
        }
        lines.push_back(name);
      }
    }
    lines.insert(lines.begin(), "In channel  " + std::to_string(lines.size()));
    // sized to the text with the child's own padding and border, so nothing is cut or scrolled
    fan::vec2 padding = gui::get_style().WindowPadding;
    f32_t width = 0.f;
    for (const auto& line : lines) {
      width = std::max(width, gui::calc_text_size(line).x);
    }
    width += padding.x * 2.f + 4.f;
    f32_t height = lines.size() * gui::get_text_line_height_with_spacing() - gui::get_style().ItemSpacing.y + padding.y * 2.f + 2.f;
    f32_t max_height = gui::get_window_size().y - 20.f;
    if (height > max_height) {
      height = max_height;
      width += gui::get_style().ScrollbarSize;
    }
    fan::vec2 at = gui::get_window_pos() + fan::vec2(std::max(0.f, gui::get_window_size().x - width - 10.f), 10.f + stream_top_offset());
    gui::set_cursor_screen_pos(at);
    gui::push_style_color(gui::col_child_bg, fan::color(0.f, 0.f, 0.f, 0.6f));
    if (gui::begin_child("##stream_users", fan::vec2(width, height), true)) {
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i == 0) {
          gui::text_disabled(lines[i]);
        }
        else {
          gui::text(lines[i]);
        }
      }
    }
    gui::end_child();
    gui::pop_style_color();
  }

void render_thread_t::ecps_gui_t::render_stream_toolbar() {
    top_bar.render_stream_stats_overlay();
    render_stream_users_overlay();
    render_stream_top_overlay();
    auto& io = gui::get_io();
    fan::vec2 area_pos = gui::get_window_pos();
    fan::vec2 area_size = gui::get_window_size();
    f32_t frame_height = gui::get_frame_height();
    f32_t zone = frame_height * 3.f;

    // Moving the mouse anywhere over the stream view shows the bar, and it goes away when the mouse rests a moment.
    // Resting on the bar itself keeps it, so it doesn't vanish under a button. Moving over the side panels does nothing
    bool mouse_inside = io.MousePos.x >= area_pos.x && io.MousePos.x <= area_pos.x + area_size.x &&
      io.MousePos.y >= area_pos.y && io.MousePos.y <= area_pos.y + area_size.y;
    bool mouse_moved = io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;
    bool mouse_on_bar = mouse_inside && io.MousePos.y >= area_pos.y + area_size.y - zone;
    bool mouse_near = mouse_inside && (mouse_moved || mouse_on_bar);
    auto now = std::chrono::steady_clock::now();
    // opening the side panel or a new unread message shows the bar for a moment, so its icons and the dot are seen,
    // but doesn't pin it: it goes away again after a while, and comes back when the mouse moves over the stream
    bool panel_open = stream_settings.show_in_stream_view;
    bool unread = chat_unread();
    static bool was_panel_open = false;
    static bool was_unread = false;
    bool just_changed = (panel_open && !was_panel_open) || (unread && !was_unread);
    was_panel_open = panel_open;
    was_unread = unread;
    // with no picture it hides too, like over a stream: it stayed up the whole time over the empty area
    if (mouse_near || just_changed || keep_stream_toolbar || mixer_requested || gui::is_popup_open("##stream_volume")) {
      toolbar_visible_until = now + std::chrono::milliseconds(1500);
    }
    f32_t target = now < toolbar_visible_until ? 1.f : 0.f;
    toolbar_alpha += (target - toolbar_alpha) * std::min(1.f, io.DeltaTime * 14.f);
    if (target == 0.f && toolbar_alpha < 0.02f) {
      toolbar_alpha = 0.f;
      return; // not drawn, so nothing under it can be clicked by accident
    }

    gui::push_style_var(gui::style_var_alpha, std::max(toolbar_alpha, 0.02f));
    f32_t y = area_size.y - frame_height - 14.f + gui::get_scroll_y();
    // the channel list beside the stream at the left, closed and opened again from here
    gui::set_cursor_pos(fan::vec2(14.f, y));
    if (icon_button("##btn_channels", icon_e::server, panel_shown(side_channels) ? "Hide the channels" : "Show the channels",
      frame_height, icon_style_e::overlay)) {
      toggle_side_panel(side_channels);
    }
    // the speaker sits by the channels, the right side keeps the panel and full screen icons
    gui::same_line(0, 8.f);
    fan::vec2 volume_at = gui::get_cursor_screen_pos();
    bool volume_muted = stream_settings.audio_volume <= 0.f;
    if (icon_button("##btn_stream_volume", volume_muted ? icon_e::speaker_muted : icon_e::speaker,
      volume_muted ? "Unmute" : "Mute", frame_height, icon_style_e::overlay)) {
      toggle_audio_volume();
    }
    if (gui::is_item_hovered()) {
      // while it is already up, opening it again every frame restarted the popup and made the mixer flash
      if (!gui::is_popup_open("##stream_volume")) {
        gui::open_popup("##stream_volume");
      }
      stream_volume_popup_until = std::max(stream_volume_popup_until, std::chrono::steady_clock::now() + std::chrono::milliseconds(300));
    }
    if (mixer_requested) {
      mixer_requested = false;
      if (gui::is_popup_open("##stream_volume")) {
        stream_volume_popup_until = {};
      }
      else {
        gui::open_popup("##stream_volume");
        stream_volume_popup_until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
      }
    }
    // how the stream comes in: green, yellow or red. A click shows the numbers over the picture
    if (perf_log.quality >= 0) {
      gui::same_line(0, 10.f);
      fan::vec2 dot_at = gui::get_cursor_screen_pos();
      gui::invisible_button("##connection_quality", fan::vec2(frame_height * 0.6f, frame_height));
      if (gui::is_item_clicked()) {
        show_stream_stats = !show_stream_stats;
      }
      static constexpr std::uint32_t colors[] = {0xff5ad06au, 0xff3cc8f0u, 0xff4a4ae8u};
      static constexpr const char* words[] = {"good", "some loss", "bad"};
      bool hot = gui::is_item_hovered();
      gui::get_window_draw_list()->AddCircleFilled(dot_at + fan::vec2(frame_height * 0.3f, frame_height * 0.5f),
        hot ? 6.f : 5.f, colors[perf_log.quality]);
      if (hot) {
        gui::set_tooltip(std::string("Connection ") + words[perf_log.quality] + ": " +
          std::to_string((int)ecps_backend.ping_ms) + " ms ping, " + fan::number_text(perf_log.quality_packet_loss, 1) +
          "% packets and " + fan::number_text(perf_log.quality_frame_loss, 1) + "% frames lost in the last second, " +
          fan::number_text(debug_sample.decoded_fps, 0) + " fps. Click for the stats");
      }
    }
    // as wide as the text needs, a fixed width cut the label off
    f32_t button_width = gui::pill_button_width(stream_button_renderer_t::share_button_text(false));
    gui::set_cursor_pos(fan::vec2((area_size.x - button_width) / 2, y));
    stream_button_renderer_t::render_stream_toggle_button(selected_channel_id, fan::vec2(button_width, 0));

    gui::same_line();
    gui::set_cursor_pos_x(area_size.x - (frame_height * 4) - 30.f);
    render_side_chat_button("##btn_chat", frame_height);
    gui::same_line(0, 8.f);
    render_side_settings_button("##btn_settings", frame_height);

    gui::same_line(area_size.x - frame_height * 2 - 10.f);
    if (icon_button("##btn_mini_player", icon_e::pin, "Mini player: the stream alone, on top of other windows", frame_height, icon_style_e::overlay)) {
      set_mini_player(true);
    }
    gui::same_line(area_size.x - frame_height - 4.f);
    if (icon_button("##btn_fullscreen", icon_e::fullscreen, "Full screen", frame_height, icon_style_e::overlay)) {
      is_fullscreen_stream = true;
      fullscreen_toolbar_alpha = 1.f;
      fullscreen_toolbar_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      owner->engine.window.set_borderless();
    }
    gui::pop_style_var();
    render_stream_volume_popup(volume_at, frame_height);
  }

void render_thread_t::ecps_gui_t::render_stream_volume_popup(fan::vec2 icon_at, f32_t icon_size) {
    // the mixer: one tall bar per sound, side by side. Stream, voice chat, alerts, then each person in the voice chat.
    // A click on a name mutes it, another brings the level back; the value shows in the tooltip
    struct column_t {
      std::string label;
      std::string tip;
      f32_t value;
      int kind; // 0 stream, 1 voice, 2 alerts, 3 one person
      std::uint32_t session = 0;
    };
    std::vector<column_t> columns;
    columns.push_back({"Stream", "Stream sound", stream_settings.audio_volume, 0});
    columns.push_back({"Voice", "Voice chat", stream_settings.voice_volume, 1});
    columns.push_back({"Alerts", "Message, join and leave sounds", stream_settings.notify_volume, 2});
    int voice_channel = voice_chat.wanted_channel.load();
    if (voice_chat.is_active() && voice_channel >= 0) {
      ecps_backend_t::Protocol_ChannelID_t channel_id;
      channel_id.i = (decltype(channel_id.i))voice_channel;
      auto members = ecps_backend.voice_members_of(channel_id);
      auto sessions = ecps_backend.channel_sessions.find((std::uint32_t)voice_channel);
      if (sessions != ecps_backend.channel_sessions.end()) {
        for (const auto& session : sessions->second) {
          std::uint32_t id = (std::uint32_t)session.session_id.i;
          if (id == (std::uint32_t)ecps_backend.session_id.i || std::find(members.begin(), members.end(), id) == members.end()) {
            continue;
          }
          columns.push_back({session.username, session.username, user_volume(session.username), 3, id});
        }
      }
    }

    f32_t font = gui::get_font_size();
    // as wide as the longest name needs ("Stream" was cut to "Strea.."), a long username is still cut at 5 letters' room
    f32_t widest = 0.f;
    for (const auto& column : columns) {
      widest = std::max(widest, gui::calc_text_size(column.label).x);
    }
    f32_t column_width = std::clamp(widest + 14.f, std::max(46.f, font * 2.8f), font * 5.f);
    f32_t bar_height = std::clamp(font * 4.6f, 112.f, 150.f);
    f32_t label_height = font + 8.f;
    f32_t width = column_width * columns.size() + 8.f;
    f32_t height = bar_height + label_height;
    fan::vec2 pos(icon_at.x + icon_size * 0.5f - column_width * 0.5f - 4.f, icon_at.y - height - 8.f);
    pos.x = std::max(4.f, std::min(pos.x, gui::get_io().DisplaySize.x - width - 4.f));
    // only while the popup is there: a next-window position set for nothing would move the next window drawn
    if (gui::is_popup_open("##stream_volume")) {
      gui::set_next_window_pos(pos);
      gui::set_next_window_size(fan::vec2(width, height));
    }
    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0.f, 0.f));
    bool open = gui::begin_popup("##stream_volume", gui::window_flags_no_move | gui::window_flags_no_saved_settings |
      gui::window_flags_no_title_bar | gui::window_flags_no_resize | gui::window_flags_no_scrollbar);
    gui::pop_style_var();
    if (!open) {
      return;
    }
    fan::vec2 at = gui::get_window_pos() + fan::vec2(4.f, 0.f);
    auto* list = gui::get_window_draw_list();
    std::uint32_t ink = gui::get_color_u32(gui::col_text);
    std::uint32_t dim_ink = gui::get_color_u32(gui::col_text_disabled);
    std::uint32_t track = gui::get_color_u32(gui::col_frame_bg);
    bool any_hovered = false;
    bool any_active = false;
    bool released = false;
    for (std::size_t i = 0; i < columns.size(); ++i) {
      auto& column = columns[i];
      f32_t x = at.x + column_width * i;
      gui::push_id((int)i);
      gui::set_cursor_screen_pos(fan::vec2(x, at.y));
      gui::invisible_button("##bar", fan::vec2(column_width, bar_height));
      bool hovered = gui::is_item_hovered();
      bool active = gui::is_item_active();
      f32_t top = at.y + 12.f;
      f32_t bottom = at.y + bar_height - 4.f;
      if (active) {
        f32_t t = 1.f - std::clamp((gui::get_io().MousePos.y - top) / (bottom - top), 0.f, 1.f);
        column.value = std::round(t * 200.f) / 100.f;
      }
      gui::set_cursor_screen_pos(fan::vec2(x, at.y + bar_height));
      bool label_clicked = gui::invisible_button("##label", fan::vec2(column_width, label_height));
      bool label_hovered = gui::is_item_hovered();
      gui::pop_id();
      if (label_clicked) {
        // mute, or back to the level it had
        f32_t* previous = column.kind == 0 ? &premute_audio_volume : column.kind == 1 ? &premute_voice_volume
          : column.kind == 2 ? &premute_notify_volume : &premute_user_volumes[column.label];
        if (column.value > 0.f) {
          *previous = column.value;
          column.value = 0.f;
        }
        else {
          column.value = *previous > 0.f ? *previous : 1.f;
        }
        released = true;
      }

      f32_t cx = x + column_width * 0.5f;
      f32_t bar = std::max(7.f, font * 0.32f);
      fan::vec2 a(cx - bar * 0.5f, top);
      fan::vec2 b(cx + bar * 0.5f, bottom);
      list->AddRectFilled(a, b, track, bar * 0.5f);
      f32_t t = std::clamp(column.value / 2.f, 0.f, 1.f);
      f32_t fill = b.y - (b.y - a.y) * t;
      if (t > 0.f) {
        list->AddRectFilled(fan::vec2(a.x, fill), b, accent_u32, bar * 0.5f);
      }
      // 100% marked beside the track
      f32_t unity = b.y - (b.y - a.y) * 0.5f;
      list->AddLine(fan::vec2(a.x - 5.f, unity), fan::vec2(a.x - 2.f, unity), dim_ink, 1.f);
      f32_t knob = bar * (hovered || active ? 1.05f : 0.9f);
      list->AddCircleFilled(fan::vec2(cx, fill), knob, 0xffffffffu, 20);

      // the name, cut to the column
      std::string text = column.label;
      f32_t room = column_width - 6.f;
      if (gui::calc_text_size(text).x > room) {
        while (!text.empty() && gui::calc_text_size(text + "..").x > room) {
          // a whole letter, not one byte of a longer one
          std::size_t cut = text.size() - 1;
          while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) {
            --cut;
          }
          text.resize(cut);
        }
        text += "..";
      }
      fan::vec2 text_size = gui::calc_text_size(text);
      list->AddText(fan::vec2(cx - text_size.x * 0.5f, at.y + bar_height + (label_height - text_size.y) * 0.5f - 2.f),
        column.value <= 0.f ? dim_ink : label_hovered ? accent_u32 : ink, text.c_str());

      if (active) {
        gui::set_mouse_cursor(gui::mouse_cursor_resize_ns);
      }
      if (hovered || active) {
        gui::set_tooltip(column.tip + ": " + std::to_string((int)std::lround(column.value * 100.f)) + "%");
      }
      else if (label_hovered) {
        gui::set_tooltip((column.value <= 0.f ? "Unmute " : "Mute ") + column.tip);
      }
      any_hovered = any_hovered || hovered || label_hovered;
      any_active = any_active || active;

      // back into the settings
      switch (column.kind) {
      case 0: stream_settings.audio_volume = column.value; break;
      case 1: stream_settings.voice_volume = column.value; break;
      case 2: stream_settings.notify_volume = column.value; break;
      default:
        if (column.value != user_volume(column.label)) {
          voice_user_volumes[column.label] = column.value;
          voice_chat.set_user_volume(column.session, column.value);
        }
        break;
      }
    }
    // a drag let go
    released = released || (stream_volume_dragging && !any_active);
    if (released) {
      save_audio_settings();
      save_voice_settings();
    }

    auto now = std::chrono::steady_clock::now();
    // the speaker it opened from, and the gap up to the mixer, count as staying: while the mixer is open ImGui reports
    // the speaker as not hovered (the popup blocks it), so with the mouse resting on it the mixer closed after 300 ms,
    // the speaker was hovered again and opened it, over and over: the flicker
    fan::vec2 mouse = gui::get_io().MousePos;
    fan::vec2 popup_at = gui::get_window_pos();
    bool over_speaker = mouse.x >= std::min(icon_at.x, popup_at.x) - 4.f && mouse.x <= std::max(icon_at.x + icon_size, popup_at.x + width) + 4.f &&
      mouse.y >= popup_at.y + height - 2.f && mouse.y <= icon_at.y + icon_size + 4.f &&
      mouse.x >= icon_at.x - 4.f && mouse.x <= icon_at.x + icon_size + 4.f;
    bool staying = any_hovered || any_active || over_speaker || gui::is_window_hovered();
    if (staying) {
      stream_volume_popup_until = std::max(stream_volume_popup_until, now + std::chrono::milliseconds(300));
    }
    stream_volume_dragging = any_active;
    if (!staying && now > stream_volume_popup_until) {
      gui::close_current_popup();
    }
    gui::end_popup();
  }

void render_thread_t::ecps_gui_t::render_stream_empty_hint(fan::vec2 area) {
    std::string title;
    std::string detail;
    // the next step as a button under the text: 2 show the channels, 3 connect
    int action = 0;
    bool others_sharing = false;
    for (const auto& channel : ecps_backend.channel_info) {
      for (std::uint32_t session : ecps_backend.streamers_of(channel.channel_id)) {
        others_sharing |= session != (std::uint32_t)ecps_backend.session_id.i;
      }
    }
    if (others_sharing) {
      title = "Loading the stream...";
      detail = "The first picture takes a moment";
    }
    else if (ecps_backend.is_streaming_to_any_channel()) {
      title = "You are sharing";
      detail = "Turn on Preview in the settings to see it here";
    }
    else if (!ecps_backend.channel_info.empty()) {
      title = "Nobody is sharing yet";
      // the toolbar at the bottom has the Share button; it shows when the mouse moves over this area
      detail = "Press Share below, or wait for someone else";
    }
    else if (!ecps_backend.connected) {
      title = "Not connected";
      detail = "Connect to a server to see its channels";
      action = 3;
    }
    else {
      title = "Not in a channel";
      detail = "Join or create one in the channels panel";
      action = panel_shown(side_channels) ? 0 : 2;
    }
    fan::vec2 title_size = gui::calc_text_size(title);
    fan::vec2 detail_size = gui::calc_text_size(detail);
    gui::set_cursor_pos(fan::vec2((area.x - title_size.x) / 2, area.y / 2 - title_size.y));
    gui::text(title);
    gui::set_cursor_pos_x((area.x - detail_size.x) / 2);
    gui::text_disabled(detail);
    if (action == 0) {
      return;
    }
    gui::dummy(fan::vec2(0, 6.f));
    const char* label = action == 2 ? "Show the channels" : "Connect to a server";
    f32_t width = gui::calc_text_size(label).x + gui::get_style().FramePadding.x * 2.f;
    gui::set_cursor_pos_x((area.x - width) / 2);
    if (gui::button(label)) {
      if (action == 2) {
        open_side_panel(side_channels);
      }
      else {
        drop_down_server.toggle_render_server_connect = true;
      }
    }
  }

void render_thread_t::ecps_gui_t::update_debug_sample() {
    auto now = std::chrono::steady_clock::now();
    f32_t seconds = std::chrono::duration<f32_t>(now - debug_sample.at).count();
    if (seconds >= 1.f) {
      std::uint64_t view_packets = ecps_backend.view_udp_packets.load();
      std::uint64_t view_bytes = ecps_backend.view_udp_bytes.load();
      std::uint64_t share_packets = ecps_backend.stream_udp_packets.load();
      std::uint64_t share_bytes = ecps_backend.stream_udp_bytes.load();
      std::uint64_t decoded = ecps_backend.view_frames_decoded.load();
      std::uint64_t assembled = ecps_backend.view_frames_assembled.load();
      std::uint64_t shared = ecps_backend.share_frames_encoded.load();
      if (debug_sample.at != std::chrono::steady_clock::time_point{}) {
        debug_sample.view_pps = (view_packets - debug_sample.view_packets) / seconds;
        debug_sample.view_kbps = (view_bytes - debug_sample.view_bytes) * 8.f / 1000.f / seconds;
        debug_sample.share_pps = (share_packets - debug_sample.share_packets) / seconds;
        debug_sample.share_kbps = (share_bytes - debug_sample.share_bytes) * 8.f / 1000.f / seconds;
        debug_sample.decoded_fps = (decoded - debug_sample.decoded) / seconds;
        debug_sample.assembled_fps = (assembled - debug_sample.assembled) / seconds;
        debug_sample.share_fps = (shared - debug_sample.share_frames) / seconds;
        debug_sample.view_kbps_history.push(debug_sample.view_kbps);
        debug_sample.decoded_fps_history.push(debug_sample.decoded_fps);
        debug_sample.share_kbps_history.push(debug_sample.share_kbps);
        debug_sample.share_fps_history.push(debug_sample.share_fps);
        debug_sample.ping_history.push(ecps_backend.ping_ms);
        perf_log_tick();
      }
      debug_sample.view_packets = view_packets;
      debug_sample.view_bytes = view_bytes;
      debug_sample.share_packets = share_packets;
      debug_sample.share_bytes = share_bytes;
      debug_sample.decoded = decoded;
      debug_sample.assembled = assembled;
      debug_sample.share_frames = shared;
      debug_sample.voices = midi_host.synth.active_voices();
      debug_sample.at = now;
    }
  }

void render_thread_t::ecps_gui_t::perf_collect_send(bool sharing) {
    auto take = [](std::atomic<std::uint64_t>& value) { return value.exchange(0, std::memory_order_relaxed); };
    std::uint64_t flushed = take(send_debug.flushed), backlog_idrs = take(send_debug.backlog_idrs),
      lock_misses = take(send_debug.lock_misses), max_queue = take(send_debug.max_queue), max_gap_us = take(send_debug.max_gap_us),
      queued = take(send_debug.queued), sent = take(send_debug.sent), cap_stops = take(send_debug.cap_stops),
      bucket_stops = take(send_debug.bucket_stops);
    perf_log.queued += queued;
    perf_log.sent += sent;
    perf_log.flushed += flushed;
    perf_log.backlog_idrs += backlog_idrs;
    perf_log.lock_misses += lock_misses;
    perf_log.passes += take(send_debug.passes);
    perf_log.chunks += take(send_debug.chunks);
    perf_log.cap_stops += cap_stops;
    perf_log.bucket_stops += bucket_stops;
    perf_log.send_us += take(send_debug.send_us);
    perf_log.max_send_us = std::max(perf_log.max_send_us, take(send_debug.max_send_us));
    perf_log.max_gap_us = std::max(perf_log.max_gap_us, max_gap_us);
    perf_log.max_queue = std::max(perf_log.max_queue, max_queue);
    if (!sharing) {
      return;
    }
    if (flushed || lock_misses) {
      perf_warn("send_drop", "threw away " + std::to_string(flushed) + " waiting frames for a keyframe (" +
        std::to_string(backlog_idrs) + " asked for by a long queue, up to " + std::to_string(max_queue) + " waited), " +
        std::to_string(lock_misses) + " lost to a locked queue, in the last second: " + std::to_string(queued) + " queued, " +
        std::to_string(sent) + " sent, " + std::to_string(cap_stops) + " passes hit the packet limit, " +
        std::to_string(bucket_stops) + " the bitrate budget, longest wait between passes " + fan::number_text(max_gap_us / 1000.0, 1) + " ms");
    }
  }

void render_thread_t::ecps_gui_t::perf_warn(const std::string& kind, const std::string& text) {
    auto now = std::chrono::steady_clock::now();
    auto it = perf_log.warned_at.find(kind);
    if (it != perf_log.warned_at.end() && now - it->second < std::chrono::seconds(60)) {
      return;
    }
    perf_log.warned_at[kind] = now;
    fan::print("perf: " + text);
  }

void render_thread_t::ecps_gui_t::perf_log_tick() {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    bool sharing = ecps_backend.is_streaming_to_any_channel();
    bool watching = debug_sample.view_pps > 0.5f;

    // what was lost in the last second
    auto& stats = ecps_backend.view.m_stats;
    std::uint64_t packet_drops_now = stats.Packet_HeadDrop + stats.Packet_BodyDrop;
    std::uint64_t frames = stats.Frame_Total - perf_log.frame_total;
    std::uint64_t frames_lost = stats.Frame_Drop - perf_log.frame_drop;
    std::uint64_t packets = stats.Packet_Total - perf_log.packet_total;
    std::uint64_t packets_lost = packet_drops_now - perf_log.packet_drop;
    // of the lost frames, the ones this computer dropped (decode queue full, decode failed) rather than the network
    auto delta = [](std::uint64_t now_value, std::uint64_t& last) { std::uint64_t d = now_value - last; last = now_value; return d; };
    std::uint64_t keyframe_waits = delta(ecps_backend.view_keyframe_waits.load(), perf_log.keyframe_waits);
    std::uint64_t queue_full = delta(ecps_backend.view_queue_full.load(), perf_log.queue_full);
    std::uint64_t decode_failed = delta(ecps_backend.view_decode_failed.load(), perf_log.decode_failed);
    perf_log.frame_total = stats.Frame_Total;
    perf_log.frame_drop = stats.Frame_Drop;
    perf_log.packet_total = stats.Packet_Total;
    perf_log.packet_drop = packet_drops_now;
    f32_t frame_loss = frames ? frames_lost * 100.f / frames : 0.f;
    f32_t packet_loss = packets + packets_lost ? packets_lost * 100.f / (packets + packets_lost) : 0.f;
    {
      f32_t ping = (f32_t)ecps_backend.ping_ms;
      perf_log.quality_frame_loss = frame_loss;
      perf_log.quality_packet_loss = packet_loss;
      perf_log.quality = !watching ? -1 : frame_loss > 10.f || packet_loss > 5.f || ping > 250.f ? 2
        : frame_loss > 2.f || packet_loss > 1.f || ping > 120.f ? 1 : 0;
    }

    std::string encoder_name = rt->screen_encoder.encoder_.get_codec_name();
    auto& decoder = rt->screen_decoder.decoder_;
    std::string decoder_name = (decoder.using_hardware_ ? decoder.get_hardware_type_string() : std::string("CPU")) +
      (rt->screen_decoder.zero_copy_active() ? ", zero copy" : "");
    std::size_t queued = ecps_backend.share.m_NetworkFlow.FrameList.Usage();

    perf_collect_send(sharing);

    {
      auto& encoder = rt->screen_encoder;
      std::uint64_t frames_now = encoder.new_frames, wait_now = encoder.screen_wait_ns;
      std::uint64_t got = frames_now - perf_log.seen_new_frames, waited = wait_now - perf_log.seen_wait_ns;
      perf_log.seen_new_frames = frames_now;
      perf_log.seen_wait_ns = wait_now;
      perf_log.screen_fps = (f32_t)got;
      perf_log.screen_ms = got ? waited / 1e6f / got : 0.f;
      perf_log.sum_new_frames += got;
      perf_log.sum_wait_ns += waited;
    }

    if (sharing) {
      int target = std::max(1, (int)rt->screen_encoder.config_.frame_rate);
      f32_t budget = 1000.f / target;
      f32_t capture_ms = stage_times.capture.ms.load();
      f32_t encode_ms = stage_times.encode.ms.load();
      perf_log.share_slow_seconds = debug_sample.share_fps < target * 0.8f ? perf_log.share_slow_seconds + 1 : 0;
      if (perf_log.share_slow_seconds == 5) {
        std::string why;
        // one frame is captured, then encoded, before the next is asked for: the two together are what has to fit
        // (capture 7 + encode 15 ms came out as 42 fps, though neither alone was over the 16.7 ms)
        if (capture_ms + encode_ms > budget * 0.9f && capture_ms > 2.f && encode_ms > 2.f) {
          bool cpu_frames = encoder_name.find("nvenc") != std::string::npos && !rt->screen_encoder.encoder_.uses_gpu_frames();
          why = "capturing (" + fan::number_text(capture_ms, 1) + " ms) and encoding (" + fan::number_text(encode_ms, 1) +
            " ms) take " + fan::number_text(capture_ms + encode_ms, 1) + " ms together, more than the " + fan::number_text(budget, 1) +
            " ms a frame has" + (cpu_frames ? ": the frames go through the CPU, GPU input could not start (see the gpu input line)" :
            ": the GPU is busy (a game)");
        }
        else if (encode_ms > budget * 0.8f) {
          why = "encoding takes " + fan::number_text(encode_ms, 1) + " ms of the " + fan::number_text(budget, 1) + " ms a frame has (" +
            encoder_name + (encoder_name.rfind("lib", 0) == 0 ? ", the CPU encoder: try Encode on GPU" : "") + ")";
        }
        else if (capture_ms > budget * 0.8f) {
          // the capture's time includes waiting for the screen to have a new picture: on Linux the compositor sends
          // one only when something changed, a still screen read as "capturing takes 384 ms"
          why = "getting a frame takes " + fan::number_text(capture_ms, 1) + " ms of the " + fan::number_text(budget, 1) +
            " ms a frame has, waiting for a new picture included (" + fan::number_text(perf_log.screen_fps, 0) +
            " new ones came: a still screen sends few, a busy compositor or GPU sends them late)";
        }
        else if (queued > 2) {
          why = std::to_string(queued) + " frames wait to be sent: the bitrate is too high for the upload";
        }
        else if (perf_log.screen_ms > budget * 0.8f) {
          why = "the screen gave " + fan::number_text(perf_log.screen_fps, 0) + " new frames, getting each took " +
            fan::number_text(perf_log.screen_ms, 1) + " ms (waiting for the screen included): the screen or the compositor gives no more";
        }
        else {
          why = "the screen gave " + fan::number_text(perf_log.screen_fps, 0) + " new frames in the last second, each in " +
            fan::number_text(perf_log.screen_ms, 1) + " ms, encoding " + fan::number_text(encode_ms, 1) +
            " ms: a still screen, or a game capped below " + std::to_string(target) + " fps";
        }
        perf_warn("share_fps", "sending " + fan::number_text(debug_sample.share_fps, 0) + " of " + std::to_string(target) + " fps: " + why);
      }
    }
    else {
      perf_log.share_slow_seconds = 0;
      perf_log.sum_new_frames = perf_log.sum_wait_ns = 0; // the preview captures too, its frames are not the share's
    }

    if (watching) {
      bool behind = debug_sample.assembled_fps > debug_sample.decoded_fps + 3.f;
      perf_log.decode_behind_seconds = behind ? perf_log.decode_behind_seconds + 1 : 0;
      // decoding itself fast but frames still fall behind: the decode thread waits on something else (the window
      // drawing slowly, the GPU busy), "decodes too slowly" with 0.7 ms a frame read as nonsense
      f32_t decode_ms = stage_times.decode.ms.load();
      f32_t frame_ms = debug_sample.assembled_fps > 0.f ? 1000.f / debug_sample.assembled_fps : 0.f;
      if (perf_log.decode_behind_seconds == 5 && decode_ms < frame_ms * 0.5f) {
        perf_warn("decode", "frames fall behind though decoding takes only " + fan::number_text(decode_ms, 1) + " ms: " +
          fan::number_text(debug_sample.assembled_fps, 0) + " frames/s come in, " + fan::number_text(debug_sample.decoded_fps, 0) +
          " decoded. The decoder waits on this computer (the window drew " + fan::number_text(draw_rate.fps.load(std::memory_order_relaxed), 0) +
          " times a second, slowest " + fan::number_text(draw_rate.slowest_ms.load(std::memory_order_relaxed), 0) + " ms; a busy GPU or CPU)");
      }
      else if (perf_log.decode_behind_seconds == 5) {
        perf_warn("decode", "this computer decodes too slowly: " + fan::number_text(debug_sample.assembled_fps, 0) + " frames/s come in, " +
          fan::number_text(debug_sample.decoded_fps, 0) + " decoded, decode takes " + fan::number_text(stage_times.decode.ms.load(), 1) +
          " ms (" + decoder_name + (decoder.using_hardware_ ? "" : ": try Decode on GPU") + ")");
      }
      // decoded in time but drawn less often: the window, not the stream
      f32_t draw_fps = draw_rate.fps.load(std::memory_order_relaxed);
      bool draw_behind = draw_fps < debug_sample.decoded_fps * 0.8f && debug_sample.decoded_fps > 10.f;
      perf_log.draw_behind_seconds = draw_behind ? perf_log.draw_behind_seconds + 1 : 0;
      if (perf_log.draw_behind_seconds == 5) {
        perf_warn("draw", "this window draws only " + fan::number_text(draw_fps, 0) + " times a second (slowest frame " +
          fan::number_text(draw_rate.slowest_ms.load(std::memory_order_relaxed), 0) + " ms) while " +
          fan::number_text(debug_sample.decoded_fps, 0) + " frames/s are decoded: the stream shows at the window's rate. Near the monitor's refresh rate that is the most it can show");
      }
      if (frames >= 5 && frame_loss > 5.f) {
        std::uint64_t here = std::min(frames_lost, queue_full + decode_failed);
        std::string why;
        if (here * 2 >= frames_lost) {
          why = "dropped on this computer: " + std::to_string(queue_full) + " found the decode queue full, " +
            std::to_string(decode_failed) + " failed to decode";
        }
        else if (packets_lost == 0) {
          why = "no packet went missing, the frames came too late (the sharer's upload or the server is slow)";
        }
        else {
          why = "data is lost between the sharer, the server and here";
        }
        perf_warn("frame_loss", "lost " + fan::number_text(frame_loss, 0) + "% of frames in the last second (" + std::to_string(frames_lost) +
          " of " + std::to_string(frames) + "), " + fan::number_text(packet_loss, 1) + "% of packets, ping " +
          fan::number_text(ecps_backend.ping_ms, 0) + " ms: " + why);
      }
      // held back while waiting for a keyframe: normal for a moment after watching starts or a frame is lost
      if (keyframe_waits >= 30) {
        // after frames lost here, the wait is for the keyframe asked for because of them, the sharer isn't slow
        perf_warn("keyframe_wait", "waited for a keyframe for " + std::to_string(keyframe_waits) + " frames in the last second" +
          (queue_full + decode_failed > 0 ? ": frames were dropped on this computer, the picture holds until the sharer's next keyframe"
            : ": the sharer is slow to send one (an older version, or its upload is full)"));
      }
    }
    else {
      perf_log.decode_behind_seconds = 0;
    }

    if ((sharing || watching) && ecps_backend.ping_ms > 150.f) {
      perf_warn("ping", "ping to the server is " + fan::number_text(ecps_backend.ping_ms, 0) + " ms");
    }

    // the sums cover the time since the last summary, which is 30 s only when this ran every second: divided by
    // that time, not by 30 (a window that drew rarely, or a pause in sharing, made 60 fps read as 91)
    auto reset_sums = [&] {
      perf_log.queued = perf_log.sent = perf_log.flushed = perf_log.backlog_idrs = perf_log.lock_misses = perf_log.passes = 0;
      perf_log.chunks = perf_log.cap_stops = perf_log.bucket_stops = perf_log.send_us = 0;
      perf_log.max_send_us = perf_log.max_gap_us = perf_log.max_queue = 0;
      perf_log.sum_new_frames = perf_log.sum_wait_ns = 0;
    };
    if (!sharing && !watching) {
      // nothing to report: the next summary starts from here, not with what piled up meanwhile
      perf_log.summary_at = now;
      reset_sums();
    }
    if ((sharing || watching) && now - perf_log.summary_at >= std::chrono::seconds(30)) {
      f64_t seconds = std::max(1.0, std::chrono::duration<f64_t>(now - perf_log.summary_at).count());
      perf_log.summary_at = now;
      if (sharing) {
        fan::print("perf: sending " + fan::size_text(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) + " " +
          // the 30 s average like the rest of the line, not the last second's sample (a short still moment read as 26/60)
          fan::number_text(perf_log.sent / seconds, 0) + "/" + std::to_string((int)rt->screen_encoder.config_.frame_rate) + " fps " +
          fan::bitrate_text(debug_sample.share_kbps) + ", capture " + fan::number_text(stage_times.capture.ms.load(), 1) +
          " ms, encode " + fan::number_text(stage_times.encode.ms.load(), 1) + " ms (" + encoder_name + "), " +
          std::to_string(queued) + " frames queued, the screen gave " + fan::number_text(perf_log.sum_new_frames / seconds, 1) +
          " new frames/s, " + fan::number_text(perf_log.sum_new_frames ? perf_log.sum_wait_ns / 1e6 / perf_log.sum_new_frames : 0.0, 1) +
          " ms each (waiting for the screen included)");
        // where the frames went: sent whole, thrown away for a keyframe, or lost before the queue. And how the network
        // loop kept up: packets per pass, passes stopped by the packet limit or the bitrate budget, the longest stall
        auto& p = perf_log;
        fan::print("perf: send path " + fan::number_text(p.queued / seconds, 1) + " frames/s queued, " + fan::number_text(p.sent / seconds, 1) +
          " sent, " + std::to_string(p.flushed) + " thrown away (" + std::to_string(p.backlog_idrs) + " keyframes for a long queue), " +
          std::to_string(p.lock_misses) + " lost to a locked queue, most waiting " + std::to_string(p.max_queue) + ". " +
          fan::number_text(p.passes / seconds, 0) + " passes/s, " + fan::number_text(p.passes ? (f64_t)p.chunks / p.passes : 0.0, 1) +
          " packets each, " + fan::number_text(p.passes ? p.send_us / 1000.0 / p.passes : 0.0, 2) + " ms each (longest " +
          fan::number_text(p.max_send_us / 1000.0, 1) + " ms), " + std::to_string(p.cap_stops) + " stopped at the packet limit, " +
          std::to_string(p.bucket_stops) + " at the bitrate budget, longest stall " + fan::number_text(p.max_gap_us / 1000.0, 1) + " ms");
      }
      reset_sums();
      if (watching) {
        // "fps" is what arrives whole. Lower than the sharer sends: lost on the way. Decoded lower than that: this computer
        fan::print("perf: receiving " + fan::size_text(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y) + " " +
          fan::number_text(debug_sample.assembled_fps, 0) + " fps " + fan::bitrate_text(debug_sample.view_kbps) +
          ", decoded " + fan::number_text(debug_sample.decoded_fps, 0) + "/s, decode " + fan::number_text(stage_times.decode.ms.load(), 1) +
          " ms (" + decoder_name + "), upload " + fan::number_text(stage_times.upload.ms.load(), 1) + " ms, lost " +
          fan::number_text(frame_loss, 1) + "% frames " + fan::number_text(packet_loss, 1) + "% packets, ping " +
          fan::number_text(ecps_backend.ping_ms, 0) + " ms, shown " + fan::number_text(rt->displayed_fps, 0) + "/s, window draws " +
          fan::number_text(draw_rate.fps.load(std::memory_order_relaxed), 0) + "/s (slowest " +
          fan::number_text(draw_rate.slowest_ms.load(std::memory_order_relaxed), 0) + " ms)");
      }
    }
  }

void render_thread_t::ecps_gui_t::render_debug_tab() {
    // the stream view draws the picture, any other tab has to hide it
    if (auto* rt = get_render_thread(); rt) {
      rt->network_frame.set_size(0);
    }
    owner->engine.viewport_set(owner->engine.orthographic_render_view.viewport, 0, owner->engine.window.get_size());


    auto number = [](double value, int decimals) { return fan::number_text(value, decimals); };
    f32_t value_column = gui::get_cursor_pos_x() + gui::calc_text_size("Viewers may play here").x + 28.f;
    auto row = [value_column](const char* label, const std::string& value) {
      f32_t start = gui::get_cursor_pos_x();
      gui::text_disabled(label);
      gui::same_line();
      gui::set_cursor_pos_x(std::max(value_column, start + gui::calc_text_size(label).x + 16.f));
      gui::text(value);
    };
    // a size that was never set holds garbage, show that as "none yet" instead of a huge number
    auto size_text = [](auto x, auto y) {
      long long width = (long long)x;
      long long height = (long long)y;
      if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return std::string("none yet");
      }
      return std::to_string(width) + " x " + std::to_string(height);
    };

    // the last 90 seconds as a line scaled to its own highest value, nothing is drawn for a value that stayed 0
    auto graph = [&](const char* label, const debug_sample_t::history_t& history, const char* unit) {
      f32_t peak = *std::max_element(history.values.begin(), history.values.end());
      if (peak <= 0.f) {
        return;
      }
      fan::vec2 size(std::max(gui::get_content_region_avail().x, 120.f), gui::get_frame_height() * 2.2f);
      fan::vec2 min = gui::get_cursor_screen_pos();
      auto* list = gui::get_window_draw_list();
      list->AddRectFilled(min, min + size, fan::color(1, 1, 1, 0.05f).get_gui_color(), 4.f);
      std::size_t count = history.values.size();
      for (std::size_t i = 0; i < count; ++i) {
        f32_t value = history.values[(history.head + i) % count];
        list->PathLineTo(fan::vec2(min.x + size.x * i / (count - 1), min.y + size.y - 2.f - (size.y - 4.f) * value / peak));
      }
      list->PathStroke(fan::color(0.45f, 0.85f, 0.55f, 1.f).get_gui_color(), 0, 1.5f);
      std::string text = std::string(label) + "  now " + number(history.values[(history.head + count - 1) % count], 0) +
        ", peak " + number(peak, 0) + " " + unit;
      list->AddText(min + fan::vec2(6.f, 3.f), fan::color(1, 1, 1, 0.6f).get_gui_color(), text.c_str());
      gui::dummy(size);
    };

    if (!gui::begin_child("##debug_panel", fan::vec2(0, 0), true)) {
      gui::end_child();
      return;
    }
    auto* rt = get_render_thread();
    auto& io = gui::get_io();

    gui::simple_section("Program");
    row("Version", std::string(ecps_version) + (ecps_update_marker[0] ? std::string(" [") + ecps_update_marker + "]" : std::string()));
    row("Build", updater.build_short().empty() ? std::string("unknown commit") : updater.build_short());
    row("Server", ecps_backend.ip + ":" + std::to_string(ecps_backend.port));
    row("Ping to server", number(ecps_backend.ping_ms, 1) + " ms");
    row("Session id", std::to_string((long long)ecps_backend.session_id.i));
    row("Window", size_text(owner->engine.window.get_size().x, owner->engine.window.get_size().y));
    row("UI frame rate", number(io.Framerate, 0) + " fps");
    graph("Ping", debug_sample.ping_history, "ms");

    if (rt) {
      gui::simple_section("Capture and encoder (this computer shares)");
      auto& geometry = rt->screen_encoder.mdscr.Geometry;
      row("Screen captured at", size_text(geometry.Resolution.x, geometry.Resolution.y) +
        (geometry.Resolution.x == 0 || geometry.Resolution.y == 0 ? "" :
          geometry.Resolution.y > geometry.Resolution.x ? "  (portrait)" : "  (landscape)"));
      row("Encoded as", size_text(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) + " @ " +
        std::to_string((long long)rt->screen_encoder.config_.frame_rate) + " fps");
      row("Bitrate", fan::bitrate_text(rt->screen_encoder.config_.bitrate / 1000.0));
      std::string used_encoder = rt->screen_encoder.encoder_.get_codec_name();
      row("Encoder", (used_encoder.empty() ? rt->screen_encoder.name : used_encoder) + " (" +
        (rt->screen_encoder.encoder_.uses_gpu_frames() ? "GPU frames" : "CPU frames") + ")");
      row("Scaling", fan::graphics::get_scaling_quality_name(rt->screen_encoder.config_.scaling_quality));
#if defined(_WIN32)
      row("Capture source", rt->screen_encoder.mdscr.target_window.load() ? "window" : "screen");
      row("HDR tone map", rt->screen_encoder.mdscr.hdr_active.load() ? "on" : "off");
#endif
      row("Capture time", number(stage_times.capture.ms.load(), 2) + " ms");
      row("Encode time", number(stage_times.encode.ms.load(), 2) + " ms");
      row("Sending", number(debug_sample.share_kbps, 0) + " kbit/s, " + number(debug_sample.share_pps, 0) + " packets/s (" +
        std::to_string((unsigned long long)debug_sample.share_packets) + " total)");
      graph("Sending", debug_sample.share_kbps_history, "kbit/s");
      graph("Frames encoded", debug_sample.share_fps_history, "fps");

      gui::simple_section("Decoder (this computer watches)");
      row("Picture", size_text(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y));
      row("Decode time", number(stage_times.decode.ms.load(), 2) + " ms");
      row("Receiving", number(debug_sample.view_kbps, 0) + " kbit/s, " + number(debug_sample.view_pps, 0) + " packets/s (" +
        std::to_string((unsigned long long)debug_sample.view_packets) + " total)");
      row("Frames put together", number(debug_sample.assembled_fps, 1) + " /s (" + std::to_string((unsigned long long)debug_sample.assembled) + " total)");
      row("Frames decoded", number(debug_sample.decoded_fps, 1) + " /s (" + std::to_string((unsigned long long)debug_sample.decoded) + " total)");
      graph("Receiving", debug_sample.view_kbps_history, "kbit/s");
      graph("Frames decoded", debug_sample.decoded_fps_history, "fps");
      row("Decode failures", std::to_string((unsigned long long)ecps_backend.view_decode_failed.load()));
      auto& stats = ecps_backend.view.m_stats;
      row("Frames dropped", std::to_string((unsigned long long)stats.Frame_Drop) + " of " + std::to_string((unsigned long long)stats.Frame_Total));
      row("Packets lost", std::to_string((unsigned long long)(stats.Packet_HeadDrop + stats.Packet_BodyDrop)) + " of " +
        std::to_string((unsigned long long)stats.Packet_Total) + ", repaired frames " + std::to_string((unsigned long long)stats.Frame_Corrected));
    }

    gui::simple_section("Channels");
    if (ecps_backend.channel_info.empty()) {
      gui::text_wrapped_disabled("Not in any channel");
    }
    for (const auto& channel : ecps_backend.channel_info) {
      std::string line = "Channel " + std::to_string((long long)channel.channel_id.i) +
        (ecps_backend.is_hosting_channel(channel.channel_id) ? "  hosting" : "  member") +
        (channel.is_streaming ? ", streaming" : "") + (channel.is_viewing ? ", watching" : "") +
        std::string(", host allows:") +
        ((channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl) ? " mouse+keyboard" : "") +
        ((channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl) ? " MIDI" : "") +
        ((channel.flag & (ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl |
          ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl)) ? "" : " nothing");
      gui::text_wrapped(line);
      auto sharing_now = ecps_backend.streamers_of(channel.channel_id);
      std::string names;
      for (std::uint32_t session : sharing_now) {
        names += (names.empty() ? "" : ", ") + session_label(channel.channel_id, session);
      }
      gui::text_wrapped(std::string("    sharing: ") + (names.empty() ? "nobody" : names));
    }
    row("Watching", watch_active ? session_label(ecps_backend_t::Protocol_ChannelID_t{ (uint16_t)watch_channel }, watch_source) : std::string("nobody"));

    gui::simple_section("MIDI");
    row("Keyboard", midi_io.is_open() ? "connected" : "not connected");
    row("Keyboards found", std::to_string((unsigned long long)midi_io.devices().size()));
    row("Playing notes here", std::to_string((unsigned long long)debug_sample.voices));
    row("Sending to host", midi_to_host.load() ? "yes" : "no");
    row("Sending to viewers", midi_to_viewers.load() ? "yes" : "no");
    row("Viewers may play here", allow_midi ? "yes" : "no");

    gui::end_child();
  }

void render_thread_t::ecps_gui_t::draw_host_cursor() {
    auto* rt = get_render_thread();
    if (!rt || !rt->host_cursor.visible.load() || rt->network_frame.get_image() == owner->engine.default_texture) {
      return;
    }
    // the host repeats itself every second while it streams, silence means it stopped
    std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms - rt->host_cursor.updated_ms.load() > 3000) {
      return;
    }
    fan::vec2 frame_size = rt->network_frame.get_size() * 2;
    fan::vec2 frame_min = gui::get_window_pos() + fan::vec2(rt->network_frame.get_position()) - frame_size / 2;
    if (frame_size.x <= 0 || frame_size.y <= 0) {
      return;
    }
    fan::vec2 at = frame_min + fan::vec2(rt->host_cursor.x.load() * frame_size.x, rt->host_cursor.y.load() * frame_size.y);

    // an arrow, white with a dark edge so it shows on any picture
    f32_t scale = std::max(1.f, gui::get_frame_height() / 22.f);
    auto point = [&](f32_t x, f32_t y) { return fan::vec2(at.x + x * scale, at.y + y * scale); };
    auto* list = gui::get_window_draw_list();
    list->PushClipRect(frame_min, frame_min + frame_size, true);
    std::uint32_t white = 0xffffffffu;
    std::uint32_t dark = 0xff000000u;
    // one arrow shape with one outline around it, so no edge runs across the inside
    const fan::vec2 shape[] = {
      point(0, 0), point(0, 17), point(4, 13), point(7.5f, 20.5f), point(10.5f, 19.f), point(7.f, 12.f), point(12, 12),
    };
    // filled as convex pieces: the concave fill split the notches wrongly and spilled white outside the arrow.
    // Head: the left edge to the notch, the notch to where the tail meets the head, and the right wing
    list->AddTriangleFilled(shape[0], shape[1], shape[2], white);
    list->AddTriangleFilled(shape[0], shape[2], shape[5], white);
    list->AddTriangleFilled(shape[0], shape[5], shape[6], white);
    // the tail
    list->AddTriangleFilled(shape[2], shape[3], shape[4], white);
    list->AddTriangleFilled(shape[2], shape[4], shape[5], white);
    for (const auto& p : shape) {
      list->PathLineTo(p);
    }
    list->PathStroke(dark, 1 /* closed */, 1.5f);
    list->PopClipRect();
  }

// Part of ecps_gui_t, included inside the struct by gui.h: the toolbar and the text over the stream, the debug tab, the host pointer.

  // The bottom toolbar of the stream view (start/stop, settings, fullscreen). It is drawn at the bottom of the
  // stream area, over the picture, and fades out unless the mouse is near the bottom or there is no picture, so
  // the stream gets the whole height. Call it inside the stream child, no window or focus tricks of its own
  void render_stream_toolbar() {
    render_stream_stats_overlay();
    render_stream_top_overlay();
    auto& io = gui::get_io();
    fan::vec2 area_pos = gui::get_window_pos();
    fan::vec2 area_size = gui::get_window_size();
    f32_t frame_height = gui::get_frame_height();
    f32_t zone = frame_height * 3.f;

    bool mouse_near = io.MousePos.x >= area_pos.x && io.MousePos.x <= area_pos.x + area_size.x &&
      io.MousePos.y >= area_pos.y + area_size.y - zone && io.MousePos.y <= area_pos.y + area_size.y;
    auto* rt = get_render_thread();
    bool has_picture = rt && rt->network_frame.get_image() != engine.default_texture;
    auto now = std::chrono::steady_clock::now();
    if (mouse_near || !has_picture) {
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
    // as wide as the text needs, a fixed width cut the label off
    f32_t button_width = pill_button_width(stream_button_renderer_t::share_button_text(false));
    gui::set_cursor_pos(fan::vec2((area_size.x - button_width) / 2, y));
    stream_button_renderer_t::render_stream_toggle_button(selected_channel_id, fan::vec2(button_width, 0));

    gui::same_line();
    gui::set_cursor_pos_x(area_size.x - (frame_height * 2) - 14.f);

    if (icon_button("##btn_settings", icon_e::gear, stream_settings.show_in_stream_view ? "Hide settings" : "Settings",
      frame_height, icon_style_e::overlay)) {
      stream_settings.show_in_stream_view = !stream_settings.show_in_stream_view;
    }

    gui::same_line(area_size.x - frame_height - 4.f);
    if (icon_button("##btn_fullscreen", icon_e::fullscreen, "Full screen", frame_height, icon_style_e::overlay)) {
      is_fullscreen_stream = true;
      fullscreen_toolbar_alpha = 1.f;
      fullscreen_toolbar_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
      engine.window.set_borderless();
    }
    gui::pop_style_var();
  }

  // what the stream view says while there is no picture
  void render_stream_empty_hint(fan::vec2 area) {
    std::string title;
    std::string detail;
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
      detail = "Press Share to share your screen, or wait for someone else";
    }
    else {
      title = "Not in a channel";
      detail = "Pick or create one in the Channels tab";
    }
    fan::vec2 title_size = gui::calc_text_size(title);
    fan::vec2 detail_size = gui::calc_text_size(detail);
    gui::set_cursor_pos(fan::vec2((area.x - title_size.x) / 2, area.y / 2 - title_size.y));
    gui::text(title);
    gui::set_cursor_pos_x((area.x - detail_size.x) / 2);
    gui::text_disabled(detail);
  }

  // Numbers about the stream and the program. Drawn only while this tab is open, read from counters the stream
  // threads already keep, and the ones that need two samples are worked out once a second. No locks the stream uses
  struct debug_sample_t {
    std::chrono::steady_clock::time_point at{};
    std::uint64_t view_packets = 0, view_bytes = 0, share_packets = 0, share_bytes = 0, decoded = 0, assembled = 0;
    f32_t view_kbps = 0, view_pps = 0, share_kbps = 0, share_pps = 0, decoded_fps = 0, assembled_fps = 0;
    std::size_t voices = 0;
    std::uint64_t share_frames = 0;
    f32_t share_fps = 0;
  }debug_sample;

  // The rates need two samples, so they are worked out once a second from counters the stream threads already
  // keep. Cheap enough to run every frame, the top bar and the stats overlay show them too
  void update_debug_sample() {
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

  void render_debug_tab() {
    // the stream view draws the picture, any other tab has to hide it
    if (auto* rt = get_render_thread(); rt) {
      rt->network_frame.set_size(0);
    }
    engine.viewport_set(engine.orthographic_render_view.viewport, 0, engine.window.get_size());


    auto number = [](double value, int decimals) {
      char text[40];
      std::snprintf(text, sizeof(text), "%.*f", decimals, value);
      return std::string(text);
    };
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

    if (!gui::begin_child("##debug_panel", fan::vec2(0, 0), true)) {
      gui::end_child();
      return;
    }
    auto* rt = get_render_thread();
    auto& io = gui::get_io();

    section_title("Program");
    row("Version", std::string(ecps_version) + (ecps_update_marker[0] ? std::string(" [") + ecps_update_marker + "]" : std::string()));
    row("Build", updater.build_short().empty() ? std::string("unknown commit") : updater.build_short());
    row("Server", ecps_backend.ip + ":" + std::to_string(ecps_backend.port));
    row("Ping to server", number(ecps_backend.ping_ms, 1) + " ms");
    row("Session id", std::to_string((long long)ecps_backend.session_id.i));
    row("Window", size_text(engine.window.get_size().x, engine.window.get_size().y));
    row("UI frame rate", number(io.Framerate, 0) + " fps");

    if (rt) {
      section_title("Capture and encoder (this computer shares)");
      auto& geometry = rt->screen_encoder.mdscr.Geometry;
      row("Screen captured at", size_text(geometry.Resolution.x, geometry.Resolution.y) +
        (geometry.Resolution.x == 0 || geometry.Resolution.y == 0 ? "" :
          geometry.Resolution.y > geometry.Resolution.x ? "  (portrait)" : "  (landscape)"));
      row("Encoded as", size_text(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) + " @ " +
        std::to_string((long long)rt->screen_encoder.config_.frame_rate) + " fps");
      row("Bitrate", number(rt->screen_encoder.config_.bitrate / 1e6, 1) + " Mbit/s");
      row("Encoder", rt->screen_encoder.name);
      row("Capture time", number(stage_times.capture.ms.load(), 2) + " ms");
      row("Encode time", number(stage_times.encode.ms.load(), 2) + " ms");
      row("Sending", number(debug_sample.share_kbps, 0) + " kbit/s, " + number(debug_sample.share_pps, 0) + " packets/s (" +
        std::to_string((unsigned long long)debug_sample.share_packets) + " total)");

      section_title("Decoder (this computer watches)");
      row("Picture", size_text(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y));
      row("Decode time", number(stage_times.decode.ms.load(), 2) + " ms");
      row("Receiving", number(debug_sample.view_kbps, 0) + " kbit/s, " + number(debug_sample.view_pps, 0) + " packets/s (" +
        std::to_string((unsigned long long)debug_sample.view_packets) + " total)");
      row("Frames put together", number(debug_sample.assembled_fps, 1) + " /s (" + std::to_string((unsigned long long)debug_sample.assembled) + " total)");
      row("Frames decoded", number(debug_sample.decoded_fps, 1) + " /s (" + std::to_string((unsigned long long)debug_sample.decoded) + " total)");
      row("Decode failures", std::to_string((unsigned long long)ecps_backend.view_decode_failed.load()));
      auto& stats = ecps_backend.view.m_stats;
      row("Frames dropped", std::to_string((unsigned long long)stats.Frame_Drop) + " of " + std::to_string((unsigned long long)stats.Frame_Total));
      row("Packets lost", std::to_string((unsigned long long)(stats.Packet_HeadDrop + stats.Packet_BodyDrop)) + " of " +
        std::to_string((unsigned long long)stats.Packet_Total) + ", repaired frames " + std::to_string((unsigned long long)stats.Frame_Corrected));
    }

    section_title("Channels");
    if (ecps_backend.channel_info.empty()) {
      text_wrapped_disabled("Not in any channel");
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

    section_title("MIDI");
    row("Keyboard", midi_io.is_open() ? "connected" : "not connected");
    row("Keyboards found", std::to_string((unsigned long long)midi_io.devices().size()));
    row("Playing notes here", std::to_string((unsigned long long)debug_sample.voices));
    row("Sending to host", midi_to_host.load() ? "yes" : "no");
    row("Sending to viewers", midi_to_viewers.load() ? "yes" : "no");
    row("Viewers may play here", allow_midi ? "yes" : "no");

    gui::end_child();
  }

  // The pointer of the computer that shares, drawn over the picture. The capture doesn't contain it (on Windows),
  // the host sends where it is and whether it is showing, so a pointer the host hides is hidden here too
  void draw_host_cursor() {
    auto* rt = get_render_thread();
    if (!rt || !rt->host_cursor.visible.load() || rt->network_frame.get_image() == engine.default_texture) {
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
    for (const auto& p : shape) {
      list->PathLineTo(p);
    }
    list->PathFillConcave(white);
    for (const auto& p : shape) {
      list->PathLineTo(p);
    }
    list->PathStroke(dark, 1 /* closed */, 1.5f);
    list->PopClipRect();
  }

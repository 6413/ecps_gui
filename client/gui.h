
inline static fan::vec2 popup_size{ 300, 100 };

struct ecps_gui_t {

  fan::graphics::image_t icon_settings;
  fan::graphics::image_t icon_fullscreen;
#define This this

  std::vector<std::function<fan::event::task_t()>> task_queue;

  std::mutex task_queue_mutex;
  void backend_queue(const std::function<fan::event::task_t()>& func) {
    std::lock_guard<std::mutex> lock(task_queue_mutex);
    task_queue.emplace_back(func);
  }

  struct stream_button_renderer_t {
    // Sharing is one action for the whole program and always goes into your own channel, so it doesn't depend on
    // which channel is selected, or on being in somebody else's
    static ecps_backend_t::Protocol_ChannelID_t own_channel() {
      for (const auto& channel : ecps_backend.channel_info) {
        if (ecps_backend.is_hosting_channel(channel.channel_id)) {
          return channel.channel_id;
        }
      }
      return ecps_backend_t::Protocol_ChannelID_t{ (uint16_t)-1 };
    }

    // Where sharing goes: the channel selected in the list when you are in it (any member can share into a channel),
    // else your own
    static ecps_backend_t::Protocol_ChannelID_t share_target() {
      if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
        auto selected = rt->ecps_gui.selected_channel_id;
        if (selected.i != (uint16_t)-1 && ecps_backend.is_channel_joined(selected)) {
          return selected;
        }
      }
      return own_channel();
    }

    static void set_sharing(bool on) {
      if (!on) {
        for (const auto& channel : ecps_backend.channel_info) {
          if (channel.is_streaming) {
            ecps_backend.set_channel_streaming(channel.channel_id, false);
          }
        }
        return;
      }
      auto own = share_target();
      if (own.i == (uint16_t)-1) {
        return;
      }
      ecps_backend.share.m_NetworkFlow.Bucket = 0;
      ecps_backend.share.m_NetworkFlow.TimerLastCallAt = fan::event::now();
      ecps_backend.share.m_NetworkFlow.TimerCallCount = 0;
      ecps_backend.share.CalculateNetworkFlowBucket();
      ecps_backend.set_channel_streaming(own, true);
    }

    static const char* share_button_text(bool compact) {
      bool sharing = ecps_backend.is_streaming_to_any_channel();
      return sharing ? (compact ? "Stop" : "Stop sharing") : (compact ? "Share" : "Share screen");
    }

    // the channel argument is only kept for the callers, the button acts on your own channel
    static bool render_stream_toggle_button(ecps_backend_t::Protocol_ChannelID_t,
      fan::vec2 button_size = fan::vec2(200.0f, 0), bool compact = false) {
      bool sharing = ecps_backend.is_streaming_to_any_channel();
      auto target = share_target();
      bool can_share = sharing || target.i != (uint16_t)-1;
      const char* text = share_button_text(compact);

      bool clicked = pill_button("##share_toggle", sharing ? icon_e::stop : icon_e::monitor, text,
        sharing ? 0xff0d0d99u : 0xff1a720du, sharing ? 0xff1212c9u : 0xff269c12u, can_share, button_size.x, button_size.y);
      if (gui::is_item_hovered(gui::hovered_flags_allow_when_disabled)) {
        if (!can_share) {
          gui::set_tooltip("Join a channel first");
        }
        else if (!sharing) {
          std::string where = "your channel";
          for (const auto& channel : ecps_backend.available_channels) {
            if (channel.channel_id.i == target.i && !ecps_backend.is_hosting_channel(target)) {
              where = channel.name;
            }
          }
          gui::set_tooltip("Share into " + where);
        }
      }

      if (clicked && can_share) {
        set_sharing(!sharing);
      }
      return clicked;
    }

    static bool render_centered_stream_button(ecps_backend_t::Protocol_ChannelID_t channel_id,
      fan::vec2 available_area,
      fan::vec2 window_content_size,
      f32_t bottom_margin = 50.0f) {
      fan::vec2 button_size(pill_button_width(share_button_text(false)), gui::get_frame_height());

      fan::vec2 button_pos = fan::vec2(
        (available_area.x - button_size.x) / 2,
        window_content_size.y - button_size.y - bottom_margin
      );

      gui::set_cursor_pos(button_pos);
      return render_stream_toggle_button(channel_id, button_size);
    }
  };

  // ---- icons, drawn with lines and shapes so they stay sharp at any size ------------------------------------
  // (colours are linear, the window shows them through sRGB: small numbers look lighter on screen)
  enum class icon_e { gear, fullscreen, exit_fullscreen, plus, server, leave, join, refresh, send_file, eye, monitor, stop };

  // an icon in a box of `size` around `c`. grow makes every line and dot thicker, for a dark outline drawn under it
  static void draw_icon(gui::draw_list_t* list, icon_e icon, fan::vec2 c, f32_t size, std::uint32_t color, f32_t grow = 0.f) {
    f32_t h = size * 0.5f;
    f32_t t = std::max(1.5f, size * 0.09f) + grow * 2.f;
    auto P = [&](f32_t x, f32_t y) { return c + fan::vec2(x * h, y * h); };
    auto line = [&](fan::vec2 a, fan::vec2 b, f32_t thickness = -1.f) { list->AddLine(a, b, color, thickness < 0 ? t : thickness + grow * 2.f); };
    auto dot = [&](fan::vec2 at, f32_t radius) { list->AddCircleFilled(at, radius + grow, color, 16); };
    auto arrow_head = [&](fan::vec2 tip, fan::vec2 dir) {
      fan::vec2 side(-dir.y, dir.x);
      line(tip, tip - dir * (h * 0.42f) + side * (h * 0.38f));
      line(tip, tip - dir * (h * 0.42f) - side * (h * 0.38f));
    };
    switch (icon) {
      case icon_e::gear: {
        list->AddCircle(c, h * 0.5f, color, 24, h * 0.3f + grow * 2.f);
        for (int i = 0; i < 8; ++i) {
          f32_t a = i * 3.14159265f / 4.f;
          fan::vec2 d(std::cos(a), std::sin(a));
          line(c + d * (h * 0.55f), c + d * (h * 0.92f), h * 0.26f);
        }
        break;
      }
      case icon_e::fullscreen:
      case icon_e::exit_fullscreen: {
        bool out = icon == icon_e::fullscreen;
        for (int sx = -1; sx <= 1; sx += 2) {
          for (int sy = -1; sy <= 1; sy += 2) {
            fan::vec2 corner = out ? P(sx * 0.82f, sy * 0.82f) : P(sx * 0.3f, sy * 0.3f);
            f32_t len = h * 0.45f * (out ? -1.f : 1.f);
            line(corner, corner + fan::vec2(sx * len, 0));
            line(corner, corner + fan::vec2(0, sy * len));
          }
        }
        break;
      }
      case icon_e::plus:
        line(P(-0.75f, 0), P(0.75f, 0));
        line(P(0, -0.75f), P(0, 0.75f));
        break;
      case icon_e::server:
        list->AddRect(P(-0.82f, -0.8f), P(0.82f, -0.1f), color, h * 0.15f, 0, t);
        list->AddRect(P(-0.82f, 0.1f), P(0.82f, 0.8f), color, h * 0.15f, 0, t);
        dot(P(0.45f, -0.45f), h * 0.11f);
        dot(P(0.45f, 0.45f), h * 0.11f);
        break;
      case icon_e::leave:
        line(P(-0.05f, -0.82f), P(-0.8f, -0.82f));
        line(P(-0.8f, -0.82f), P(-0.8f, 0.82f));
        line(P(-0.8f, 0.82f), P(-0.05f, 0.82f));
        line(P(-0.3f, 0), P(0.88f, 0));
        arrow_head(P(0.88f, 0), fan::vec2(1, 0));
        break;
      case icon_e::join:
        line(P(0.05f, -0.82f), P(0.8f, -0.82f));
        line(P(0.8f, -0.82f), P(0.8f, 0.82f));
        line(P(0.8f, 0.82f), P(0.05f, 0.82f));
        line(P(-0.88f, 0), P(0.35f, 0));
        arrow_head(P(0.35f, 0), fan::vec2(1, 0));
        break;
      case icon_e::refresh: {
        f32_t r = h * 0.68f;
        f32_t a0 = 3.14159265f * 0.15f, a1 = 3.14159265f * 1.72f;
        list->PathArcTo(c, r, a0, a1, 24);
        list->PathStroke(color, 0, t);
        fan::vec2 e = c + fan::vec2(std::cos(a1), std::sin(a1)) * r;
        fan::vec2 tangent(-std::sin(a1), std::cos(a1));
        fan::vec2 normal(std::cos(a1), std::sin(a1));
        list->AddTriangleFilled(e + tangent * (h * 0.38f), e + normal * (h * 0.34f + grow), e - normal * (h * 0.34f + grow), color);
        break;
      }
      case icon_e::send_file:
        list->PathLineTo(P(-0.62f, -0.88f));
        list->PathLineTo(P(0.22f, -0.88f));
        list->PathLineTo(P(0.62f, -0.48f));
        list->PathLineTo(P(0.62f, 0.88f));
        list->PathLineTo(P(-0.62f, 0.88f));
        list->PathStroke(color, 1, t);
        line(P(0, 0.52f), P(0, -0.28f));
        arrow_head(P(0, -0.28f), fan::vec2(0, -1));
        break;
      case icon_e::eye:
        list->AddBezierQuadratic(P(-0.95f, 0), P(0, -0.95f), P(0.95f, 0), color, t, 16);
        list->AddBezierQuadratic(P(-0.95f, 0), P(0, 0.95f), P(0.95f, 0), color, t, 16);
        dot(c, h * 0.27f);
        break;
      case icon_e::monitor:
        list->AddRect(P(-0.9f, -0.78f), P(0.9f, 0.42f), color, h * 0.12f, 0, t);
        line(P(0, 0.42f), P(0, 0.72f));
        line(P(-0.42f, 0.78f), P(0.42f, 0.78f));
        line(P(0, 0.18f), P(0, -0.48f));
        arrow_head(P(0, -0.48f), fan::vec2(0, -1));
        break;
      case icon_e::stop:
        list->AddRectFilled(P(-0.5f, -0.5f), P(0.5f, 0.5f), color, h * 0.12f);
        break;
    }
  }

  // with a dark edge, for icons over the picture, which can be anything from black to white
  static void draw_icon_outlined(gui::draw_list_t* list, icon_e icon, fan::vec2 c, f32_t size, std::uint32_t color) {
    draw_icon(list, icon, c, size, 0xc8000000u, 1.6f);
    draw_icon(list, icon, c, size, color);
  }

  enum class icon_style_e { plain, danger, accent, overlay };

  // a square button with an icon and a tooltip
  static bool icon_button(const char* id, icon_e icon, std::string_view tooltip, f32_t size = 0.f, icon_style_e style = icon_style_e::plain) {
    if (size <= 0.f) {
      size = gui::get_frame_height();
    }
    fan::vec2 at = gui::get_cursor_screen_pos();
    bool clicked = gui::invisible_button(id, fan::vec2(size, size));
    bool hovered = gui::is_item_hovered();
    bool held = gui::is_item_active();
    auto* list = gui::get_window_draw_list();
    fan::vec2 c = at + fan::vec2(size, size) * 0.5f;
    std::uint32_t icon_color = hovered ? 0xffffffffu : 0xffff9e6au;
    f32_t rounding = size * 0.28f;
    switch (style) {
      case icon_style_e::plain:
        list->AddRectFilled(at, at + fan::vec2(size, size), held ? 0xff61210fu : hovered ? 0xff3a170cu : 0xff0f0907u, rounding);
        break;
      case icon_style_e::danger:
        list->AddRectFilled(at, at + fan::vec2(size, size), held ? 0xff0c0c70u : hovered ? 0xff0a0a52u : 0xff05051au, rounding);
        icon_color = hovered ? 0xffffffffu : 0xff6a6affu;
        break;
      case icon_style_e::accent:
        list->AddRectFilled(at, at + fan::vec2(size, size), held ? 0xff61210fu : hovered ? 0xff4a1a0du : 0xff2a1009u, rounding);
        icon_color = hovered ? 0xffffffffu : 0xffff7a33u;
        break;
      case icon_style_e::overlay:
        list->AddCircleFilled(c, size * 0.5f, hovered ? 0xb4000000u : 0x64000000u, 24);
        icon_color = hovered ? 0xffffffffu : 0xffe0e0e0u;
        break;
    }
    if (style == icon_style_e::overlay) {
      draw_icon_outlined(list, icon, c, size * 0.56f, icon_color);
    }
    else {
      draw_icon(list, icon, c, size * 0.56f, icon_color);
    }
    if (hovered && !tooltip.empty()) {
      gui::set_tooltip(tooltip);
    }
    return clicked;
  }

  // a rounded button with a small icon in front of its word, coloured by what it does
  static f32_t pill_button_width(std::string_view text) {
    f32_t height = gui::get_frame_height();
    return height * 0.62f + 8.f + gui::calc_text_size(text).x + 30.f;
  }

  static bool pill_button(const char* id, icon_e icon, std::string_view text, std::uint32_t color, std::uint32_t hover_color,
    bool enabled = true, f32_t width = 0.f, f32_t height = 0.f) {
    if (height <= 0.f) {
      height = gui::get_frame_height();
    }
    if (width <= 0.f) {
      width = pill_button_width(text);
    }
    fan::vec2 at = gui::get_cursor_screen_pos();
    bool clicked = gui::invisible_button(id, fan::vec2(width, height));
    bool hovered = gui::is_item_hovered(gui::hovered_flags_allow_when_disabled);
    auto* list = gui::get_window_draw_list();
    std::uint32_t fill = !enabled ? 0xff141414u : hovered ? hover_color : color;
    list->AddRectFilled(at, at + fan::vec2(width, height), fill, height * 0.5f);
    f32_t icon_size = height * 0.62f;
    f32_t content = icon_size + 8.f + gui::calc_text_size(text).x;
    fan::vec2 start = at + fan::vec2((width - content) * 0.5f, 0);
    std::uint32_t ink = enabled ? 0xffffffffu : 0xff606060u;
    draw_icon(list, icon, start + fan::vec2(icon_size * 0.5f, height * 0.5f), icon_size * 0.8f, ink);
    std::string label(text);
    list->AddText(start + fan::vec2(icon_size + 8.f, (height - gui::get_font_size()) * 0.5f), ink, label.c_str());
    return clicked && enabled;
  }

  // a small rounded label, like HOST or LIVE, on the current line
  static void badge(std::string_view text, std::uint32_t fill, std::uint32_t ink) {
    std::string label(text);
    fan::vec2 size = gui::calc_text_size(label);
    fan::vec2 at = gui::get_cursor_screen_pos() + fan::vec2(0, -1.f);
    fan::vec2 box = size + fan::vec2(14.f, 2.f);
    gui::get_window_draw_list()->AddRectFilled(at, at + box, fill, box.y * 0.5f);
    gui::get_window_draw_list()->AddText(at + fan::vec2(7.f, 1.f), ink, label.c_str());
    gui::dummy(box - fan::vec2(0, 2.f));
  }

  struct icon_button_helper_t {
    static bool render_transparent_icon_button(const char* id, fan::graphics::image_t icon,
      fan::vec2 icon_size, fan::vec2 padding = fan::vec2(-1, -1)) {
      if (padding.x < 0) padding = fan::vec2(gui::get_style().FramePadding.y, gui::get_style().FramePadding.y);

      gui::push_style_color(gui::col_button, fan::vec4(0.3f, 0.3f, 0.3f, 0.0f));
      gui::push_style_color(gui::col_button_hovered, fan::vec4(0.5f, 0.5f, 0.5f, 0.9f));
      gui::push_style_color(gui::col_button_active, fan::vec4(0.6f, 0.6f, 0.6f, 1.0f));
      gui::push_style_var(gui::style_var_frame_padding, padding);

      bool clicked = gui::image_button(id, icon, icon_size);

      gui::pop_style_var(1);
      gui::pop_style_color(3);
      return clicked;
    }
  };

  struct button_helper_t {
    static f32_t calculate_text_button_width(const char* text, f32_t extra_padding = 20.0f) {
      fan::vec2 text_size = gui::calc_text_size(text);
      return text_size.x + gui::get_style().FramePadding.x * 2 + extra_padding;
    }

    static bool render_colored_button(const char* text, fan::vec4 color, fan::vec2 size = fan::vec2(0, 0)) {
      gui::push_style_color(gui::col_button, color);
      bool clicked = gui::button(text, size);
      gui::pop_style_color();
      return clicked;
    }
  };

  struct splitter_renderer_t {
    static void render_vertical_splitter(const char* id, f32_t content_height,
      f32_t& unclamped_width, f32_t& clamped_width,
      f32_t min_width, f32_t max_width, bool& is_resizing) {
      gui::push_style_color(gui::col_button, fan::vec4(0.5f, 0.5f, 0.5f, 0.3f));
      gui::push_style_color(gui::col_button_hovered, fan::vec4(0.7f, 0.7f, 0.7f, 0.5f));
      gui::push_style_color(gui::col_button_active, fan::vec4(0.8f, 0.8f, 0.8f, 0.7f));

      f32_t hitbox_width = 8.0f;
      f32_t splitter_width = 2.0f;
      f32_t visual_offset = (hitbox_width - splitter_width) * 0.5f;

      if (gui::invisible_button(id, fan::vec2(hitbox_width, content_height))) {}

      if (gui::is_item_active() && gui::is_mouse_dragging(0)) {
        unclamped_width -= gui::get_io().MouseDelta.x;
        clamped_width = std::clamp(unclamped_width, min_width, max_width);
        is_resizing = true;
        gui::set_mouse_cursor(gui::mouse_cursor_resize_ew);
      }
      else if (is_resizing && !gui::is_mouse_dragging(0)) {
        is_resizing = false;
      }

      if (gui::is_item_hovered()) {
        gui::set_mouse_cursor(gui::mouse_cursor_resize_ew);
      }

      fan::vec2 splitter_pos = gui::get_item_rect_min();
      splitter_pos.x += visual_offset;
      fan::vec2 splitter_max = fan::vec2(splitter_pos.x + splitter_width, splitter_pos.y + content_height);

      gui::get_window_draw_list()->AddRectFilled(
        splitter_pos, splitter_max,
        gui::is_item_hovered() ? gui::get_color_u32(gui::col_button_hovered) : gui::get_color_u32(gui::col_button)
      );

      gui::pop_style_color(3);
    }
  };

  struct stream_area_calculator_t {
    static fan::vec2 calculate_fitted_size(fan::vec2 available_area, fan::vec2 decoded_size) {
      f32_t aspect = 16.0f / 9.0f;
      if (decoded_size.x > 0 && decoded_size.y > 0) {
        aspect = decoded_size.x / decoded_size.y;
      }

      fan::vec2 full_size = (available_area.x / available_area.y > aspect)
        ? fan::vec2(available_area.y * aspect, available_area.y)
        : fan::vec2(available_area.x, available_area.x / aspect);

      return full_size / 2;
    }
  };

  struct network_frame_helper_t {
    #undef This
    #define This OFFSETLESS(this, ecps_gui_t, network_frame_helper) 
    void setup_network_frame(fan::vec2 available_area) {
      auto rt = get_render_thread();
      if (!rt) return;

      fan::vec2 center = available_area / 2;
      rt->network_frame.set_position(center);

      fan::vec2 fitted_size = stream_area_calculator_t::calculate_fitted_size(
        available_area, rt->screen_decoder.decoded_size);

      if (rt->network_frame.get_image() != engine.default_texture) {
        rt->network_frame.set_size(fitted_size);
      }
      else {
        rt->network_frame.set_size(fan::vec2(0, 0));
      }
    }
  }network_frame_helper;
  #undef This
  #define This this

  ecps_gui_t() {

    engine.set_clear_color(gui::get_color(gui::col_window_bg));
    // lines are smoothed by their own geometry: the textured way needs the font atlas sampled with filtering,
    // and without it every drawn line and icon came out jagged
    gui::get_style().AntiAliasedLinesUseTex = false;
    gui::get_style().AntiAliasedLines = true;
    gui::get_style().AntiAliasedFill = true;
    // more segments in circles and rounded corners, the default leaves their edges a little stepped
    gui::get_style().CircleTessellationMaxError = 0.12f;
    gui::get_style().CurveTessellationTol = 0.5f;

    // dropping files on the window sends them, the next frame asks to whom if it isn't obvious
    file_drop_handle = engine.window.on_drop([this](const fan::window_t::drop_data_t& d) {
      dropped_files = d.paths;
      drop_popup_requested = !dropped_files.empty();
    });

    icon_settings = engine.image_load("icons/settings.png", {
     .min_filter = fan::graphics::image_filter_e::linear,
     .mag_filter = fan::graphics::image_filter_e::linear,
      });

    icon_fullscreen = engine.image_load("icons/fullscreen.png", {
     .min_filter = fan::graphics::image_filter_e::nearest,
     .mag_filter = fan::graphics::image_filter_e::nearest,
      });

    if (fan::io::file::exists(config_path)) {
      std::string data;
      fan::io::file::read(std::string(config_path), &data);
      config = fan::json::parse(data);
    }

    if (config.is_object()) {
      if (config.contains("username") && config["username"].is_string()) {
        ecps_backend.desired_username = config["username"].get<std::string>();
      }
      if (config.contains("auto_join") && config["auto_join"].is_string()) {
        auto_join_host = config["auto_join"].get<std::string>();
      }
      if (config.contains("ui") && config["ui"].is_object()) {
        fan::json ui = config["ui"];
        top_bar_compact = ui.value("compact_bar", false);
        ui_autohide = ui.value("autohide", false);
      }
      if (config.contains("stream") && config["stream"].is_object()) {
        fan::json stream = config["stream"];
        saved_stream.width = stream.value("width", 0);
        saved_stream.height = stream.value("height", 0);
        saved_stream.fps = stream.value("fps", 0);
        saved_stream.bitrate_mode = stream.value("bitrate_mode", 0);
        saved_stream.bitrate_mbps = stream.value("bitrate_mbps", 10.f);
        saved_stream.scaling_quality = stream.value("scaling_quality", -1);
        saved_stream.valid = true;
      }
    }
    updater.collect_state = [this]() { return collect_resume_state(); };
    load_resume_state();

    if (config.contains("audio")) {
      fan::json audio = config["audio"];
      stream_settings.share_audio = audio.value("share", stream_settings.share_audio);
      stream_settings.play_audio = audio.value("play", stream_settings.play_audio);
      stream_settings.audio_volume = audio.value("volume", stream_settings.audio_volume);
      stream_settings.audio_device = audio.value("device", std::string());
      if (audio.contains("devices") && audio["devices"].is_object()) {
        fan::json devices = audio["devices"];
        for (auto it = devices.begin(); it != devices.end(); ++it) {
          auto& settings = stream_settings.audio_devices[it.key()];
          settings.gain = it.value().value("gain", settings.gain);
          settings.exclude_discord = it.value().value("exclude_discord", settings.exclude_discord);
        }
      }
    }

    std::string ip, port;
    if (config.contains("server")) {
      if (config["server"].contains("ip")) {
        ip = config["server"]["ip"];
      }
      if (config["server"].contains("port")) {
        port = config["server"]["port"];
      }
    }
    if (ip.size() && port.size()) {
      backend_queue([=]() -> fan::event::task_t {
        try {
          if (!co_await ecps_backend.connect(ip, string_to_number(port))) {
            co_return;
          }
          co_await ecps_backend.login();

          // no channel is made for you, pick one from the list or make your own with "Add new"
          co_await ecps_backend.request_channel_list();
        }
        catch (...) {

        }
        });
    }
  }
  static uint32_t string_to_number(const std::string& str) {
    uint32_t v = 0;
    try {
      v = std::stoul(str);
    }
    catch (const std::invalid_argument& e) {
      fan::print("The input couldn't be converted to a number");
      return -1;
    }
    catch (const std::out_of_range& e) {
      fan::print("The input was too large for unsigned long");
      return -1;
    }
    return v;
  }

  struct drop_down_server_t {
#undef This
#define This OFFSETLESS(this, ecps_gui_t, drop_down_server)

    void render() {

      bool contains_server = This->config.contains("server");
      static std::string ip = contains_server && This->config["server"].contains("ip") ? This->config["server"]["ip"] : "";
      static std::string port = contains_server && This->config["server"].contains("port") ? This->config["server"]["port"] : "";

      fan::json server_json = This->config.contains("server") ? This->config["server"] : fan::json();

      gui::push_style_var(gui::style_var_window_padding, fan::vec2(13.0000, 20.0000));
      gui::push_style_var(gui::style_var_item_spacing, fan::vec2(14, 16));
      if (toggle_render_server_create) {
        gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
        gui::open_popup("server_create");
      }

      if (gui::begin_popup("server_create")) {
        gui::push_item_width(300);
        popup_size = gui::get_window_size();
        gui::text("Channel Create");

        static std::string name;
        gui::input_text("name", &name);

        if (gui::button("Create")) {
          This->backend_queue([=]() -> fan::event::task_t {
            try {
              auto channel_id = co_await ecps_backend.channel_create();
              co_await ecps_backend.channel_join(channel_id, true);
            }
            catch (...) {}
            });

          gui::close_current_popup();
          toggle_render_server_create = false;
        }
        gui::same_line();
        if (gui::button("Cancel")) {
          gui::close_current_popup();
          toggle_render_server_create = false;
        }

        gui::pop_item_width();
        gui::end_popup();
      }

      if (toggle_render_server_connect) {
        gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
        gui::open_popup("server_connect");
      }

      if (gui::begin_popup("server_connect")) {

        gui::push_item_width(300);

        popup_size = gui::get_window_size();

        bool enter_pressed = gui::input_text("ip", &ip, gui::input_text_flags_enter_returns_true);
        enter_pressed |= gui::input_text("port", &port, gui::input_text_flags_enter_returns_true);

        if (gui::button("Connect") || enter_pressed) {
          {
            if (ip.size()) {
              server_json["ip"] = ip;
            }
            if (port.size()) {
              server_json["port"] = port;
            }
            if (ip.size() && port.size()) {
              ecps_backend.ip = ip;
              ecps_backend.port = std::stoul(port);
              This->backend_queue([=]() -> fan::event::task_t {
                try {
                  bool did_connect = co_await ecps_backend.connect(ip, string_to_number(port));
                  if (did_connect) {
                    co_await ecps_backend.login();
                    co_await ecps_backend.request_channel_list();
                  }
                }
                catch (...) {
                }
                });
            }
          }

          This->write_to_config("server", server_json);
          gui::close_current_popup();
          toggle_render_server_connect = false;
        }
        gui::same_line();
        if (gui::button("Cancel")) {
          gui::close_current_popup();
          toggle_render_server_connect = false;
        }
        gui::pop_item_width();
        gui::end_popup();
      }

      if (toggle_render_server_join) {
        gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
        gui::open_popup("join channel");
      }

      if (gui::begin_popup("join channel")) {
        gui::push_item_width(300);

        popup_size = gui::get_window_size();

        if (gui::button("Connect")) {
          This->write_to_config("server", server_json);
          gui::close_current_popup();
          toggle_render_server_join = false;
        }
        gui::same_line();
        if (gui::button("Cancel")) {
          gui::close_current_popup();
          toggle_render_server_join = false;
        }
        gui::pop_item_width();
        gui::end_popup();
      }

      gui::pop_style_var(2);
    }

    bool toggle_render_server_create = false;
    bool toggle_render_server_connect = false;
    bool toggle_render_server_join = false;
  }drop_down_server;

  void render_stream() {
    fan::vec2 viewport_size = gui::get_content_region_avail();
    fan::vec2 popup_size = fan::vec2(viewport_size.x * 0.8f, 80);
    fan::vec2 stream_pos = gui::get_cursor_screen_pos() + fan::vec2(viewport_size.x / 2 - popup_size.x / 2, 0);
    fan::vec2 start_pos = fan::vec2(stream_pos.x, stream_pos.y + viewport_size.y + 50);
    fan::vec2 target_pos = fan::vec2(stream_pos.x, stream_pos.y + viewport_size.y - 90);

    gui::push_style_var(gui::style_var_frame_rounding, 12.f);
    gui::push_style_var(gui::style_var_window_rounding, 12.f);

    {
      static fan::vec2 last_mouse = gui::get_io().MousePos;
      fan::vec2 current_mouse_pos = gui::get_io().MousePos;

      fan::vec2 gui_window_size = gui::get_window_size();
      fan::vec2 parent_min = gui::get_window_pos();
      fan::vec2 parent_max = fan::vec2(parent_min.x + gui_window_size.x,
        parent_min.y + gui_window_size.y);

      fan::vec2 hitbox_size = parent_max - parent_min;

      f32_t cursor_y = gui::get_cursor_pos_y();
      gui::set_cursor_pos_y(cursor_y + hitbox_size.y / 2 + (hitbox_size.y * 0.75) / 2);
      hitbox_size.y *= 0.15;
      gui::dummy(hitbox_size);

      last_mouse = current_mouse_pos;
    }

    gui::pop_style_var(2);
  }

  struct stream_settings_t {
    int selected_scaling_quality = 1;
    int selected_resolution = 0;
    int framerate = 30;
    int selected_encoder = 0;
    int selected_decoder = 0;
    int input_control = 0;
    bool show_in_stream_view = false;
    bool share_audio = false;
    bool play_audio = true;
    f32_t audio_volume = 1.f;

    struct audio_device_settings_t {
      f32_t gain = 1.f;
      bool exclude_discord = true;
    };
    // loopback device to record, empty follows the default output
    std::string audio_device;
    // remembered per device id
    std::map<std::string, audio_device_settings_t> audio_devices;

    f32_t bitrate_mbps = 10;
    bool use_adaptive_bitrate = true;
    int bitrate_mode = 0;

    f32_t settings_panel_width = 350.0f;
    bool is_resizing = false;

    bool p_open = false;
    bool show_network_debug = false;
  }stream_settings;

  struct channel_list_window_t {
#undef This
#define This OFFSETLESS(this, ecps_gui_t, window_handler)

    void render() {
      if (!p_open) return;

      auto* viewport = gui::get_main_viewport();
      fan::vec2 window_pos = fan::vec2(viewport->WorkPos.x, viewport->WorkPos.y);
      fan::vec2 window_size = fan::vec2(viewport->WorkSize.x, viewport->WorkSize.y);
      gui::set_next_window_pos(window_pos);
      gui::set_next_window_size(window_size);
      gui::set_next_window_bg_alpha(0.99);
      This->push_theme();
      if (gui::begin("Channel Browser", &p_open,
        gui::window_flags_no_docking |
        gui::window_flags_no_saved_settings |
        gui::window_flags_no_move |
        gui::window_flags_no_collapse |
        gui::window_flags_no_background |
        gui::window_flags_no_resize |
        gui::window_flags_no_title_bar)
        ) {

        // Stream View with the interface hidden: the stream gets the whole window, the bar and the tabs come back
        // near the top edge (drawn over the stream by render_autohide_bar)
        // a tab change that was asked for but not done yet (main_tab is 0 or 2) keeps the tabs on screen until it is
        // done: the tab bar needs a frame or two to switch, and without it the Stream View would hide the tabs again
        // a tab asked for elsewhere (main_tab) takes effect here
        if (main_tab != -1) {
          current_tab = main_tab;
          main_tab = -1;
        }
        if (This->ui_autohide && current_tab == 1) {
          This->report_display_sizes();
          updater.tick();
          updater.tick_restart();
          render_stream_view_content();
          This->render_about_popup();
        }
        else {
        This->render_top_bar();

        static bool first_open = true;
        if (first_open && p_open) {
          first_open = false;
          This->backend_queue([=]() -> fan::event::task_t {
            try {
              co_await ecps_backend.request_channel_list();
              if (This->selected_channel_id.i != (uint16_t)-1) {
                co_await ecps_backend.request_channel_session_list(This->selected_channel_id);
              }
            }
            catch (...) {
              fan::print("Failed to auto-refresh channel list");
            }
            });
        }

        if (!p_open) {
          first_open = true;
        }

        // the bar above holds the tabs, the one chosen fills the rest
        switch (current_tab) {
          case 0: render_channels_content(); break;
          case 1: render_stream_view_content(); break;
          case 2: This->render_debug_tab(); break;
          default: current_tab = 0; break;
        }
        }
      }
      gui::end();
      if (This->ui_autohide && current_tab == 1) {
        This->render_autohide_bar(window_pos, window_size);
      }
      This->pop_theme();
    }

    void render_channels_content() {
      if (auto rt = get_render_thread(); rt) {
        rt->network_frame.set_size(0);
      }
      engine.viewport_set(engine.orthographic_render_view.viewport, 0, engine.window.get_size());

      fan::vec2 avail_size = gui::get_content_region_avail();
      if (avail_size.x > 0 && avail_size.y > 0) {

        f32_t left_window_padding = 8.0f;
        f32_t right_window_padding = 12.0f;
        f32_t splitter_spacing = 12.0f;
        f32_t splitter_width = 2.0f;

        f32_t total_spacing = left_window_padding + splitter_spacing + splitter_width + splitter_spacing + right_window_padding;

        static f32_t channel_details_width = 0.0f;
        static f32_t unclamped_details_width = 0.0f;
        static bool initialized = false;
        if (!initialized) {
          f32_t available_content_width = avail_size.x - total_spacing;
          channel_details_width = available_content_width * 0.5f;
          unclamped_details_width = channel_details_width;
          initialized = true;
        }

        f32_t min_details_width = 300.0f;
        f32_t max_details_width = avail_size.x * 0.7f;
        channel_details_width = std::clamp(
          unclamped_details_width,
          min_details_width,
          max_details_width
        );

        f32_t channel_list_width = avail_size.x - channel_details_width - total_spacing;

        gui::dummy(fan::vec2(left_window_padding, 0));
        gui::same_line(0, 0);

        gui::set_next_window_bg_alpha(0);
        gui::begin_child("##channel_list_panel", fan::vec2(channel_list_width, avail_size.y), true);

        gui::spacing();
        gui::spacing();

        gui::spacing();
        gui::spacing();

        gui::push_item_width(150);
        gui::input_text("Search", &search_filter);
        gui::pop_item_width();

        gui::spacing();

        gui::spacing();
        gui::spacing();

        gui::set_next_window_bg_alpha(0.99);
        gui::table_flags_t table_flags = gui::table_flags_row_bg
          | gui::table_flags_borders_inner
          | gui::table_flags_borders_outer
          | gui::table_flags_resizable
          | gui::table_flags_sizing_fixed_same;
        if (gui::begin_child("ChannelList", fan::vec2(0, -60), true)) {
          if (gui::begin_table("ChannelTable", 4, table_flags)) {

            gui::table_setup_column("ID", gui::table_column_flags_width_fixed, 50.f, 0);
            gui::table_setup_column("Stream Name", gui::table_column_flags_width_stretch, 0.f, 0);
            gui::table_setup_column("Viewers");
            gui::table_setup_column("Live Time");

            gui::table_headers_row();

            int row_index = 0;
            for (const auto& channel : ecps_backend.available_channels) {
              gui::push_id(row_index);
              if (!search_filter.empty()) {
                std::string name_lower = channel.name;
                std::string filter_lower = search_filter;
                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);
                if (name_lower.find(filter_lower) == std::string::npos) {
                  continue;
                }
              }

              bool is_selected = (This->selected_channel_id.i == channel.channel_id.i);

              bool is_host_of_channel = ecps_backend.is_current_user_host_of_channel(channel.channel_id);

              std::string display_name = channel.name;
              if (is_host_of_channel) {
                display_name = "⭐ " + channel.name;
              }
              if (channel.is_password_protected) {
              }

              bool already_joined = false;
              for (const auto& joined_channel : ecps_backend.channel_info) {
                if (joined_channel.channel_id.i == channel.channel_id.i) {
                  already_joined = true;
                  break;
                }
              }
              gui::table_next_row();
              {
                gui::table_set_column_index(0);
                gui::text(std::to_string(channel.channel_id.i));
              }

              {
                gui::table_set_column_index(1);
                gui::push_style_color(gui::col_header_active, gui::get_style().Colors[gui::col_header_hovered]);

                if (gui::selectable(std::string_view(display_name), is_selected, gui::selectable_flags_span_all_columns)) {
                  This->selected_channel_id = channel.channel_id;

                  This->backend_queue([channel_id = channel.channel_id]() -> fan::event::task_t {
                    try {
                      co_await ecps_backend.request_channel_session_list(channel_id);
                    }
                    catch (...) {
                      fan::print("Failed to request session list");
                    }
                    });
                }
                gui::pop_style_color();
              }
              {
                gui::table_set_column_index(2);
                gui::text(std::to_string(channel.user_count - 1));
              }
              {
                gui::table_set_column_index(3);
                uint64_t stream_time_ns = ecps_backend.get_channel_stream_time(channel.channel_id);
                double stream_time_s = stream_time_ns / 1e+9;
                uint64_t hours = stream_time_s / 3600;
                uint64_t minutes = fmod(stream_time_s, 3600.0) / 60.0;
                uint64_t seconds = fmod(stream_time_s, 60.0);
                if (!stream_time_ns) {
                  hours = minutes = seconds = 0;
                }
                gui::text(fan::format("{:02}:{:02}:{:02}", hours, minutes, seconds));
              }
              row_index++;
              gui::pop_id();
            }
            gui::end_table();
          }

          if (ecps_backend.available_channels.empty()) {
            if (ecps_backend.channel_list_received) {
              gui::text("No channels available, create a new channel.");
            }
            else {
              gui::text("Click 'Refresh Channel List' to load channels");
            }
          }
        }
        gui::end_child();

        gui::spacing();

        bool has_selection = (This->selected_channel_id.i != (uint16_t)-1);
        bool already_in_selected = false;
        bool is_host_of_selected = false;

        if (has_selection) {
          for (const auto& joined_channel : ecps_backend.channel_info) {
            if (joined_channel.channel_id.i == This->selected_channel_id.i) {
              already_in_selected = true;

              is_host_of_selected = ecps_backend.is_current_user_host_of_channel(This->selected_channel_id);
              break;
            }
          }
        }

        if (has_selection && !is_host_of_selected) {
          if (already_in_selected) {
            if (icon_button("##leave_channel", icon_e::leave, "Leave channel", gui::get_frame_height() * 1.15f, icon_style_e::danger)) {
              // a channel joined by itself would be joined again at once, leaving it stops that
              for (const auto& channel : ecps_backend.available_channels) {
                if (channel.channel_id.i == This->selected_channel_id.i && This->auto_join_host == channel.name) {
                  This->set_auto_join(std::string());
                }
              }
              This->backend_queue([channel_id = This->selected_channel_id]() -> fan::event::task_t {
                try {
                  co_await ecps_backend.channel_quit(channel_id);
                  co_await ecps_backend.request_channel_list();
                }
                catch (...) {
                  fan::print("Failed to leave channel");
                }
                });
            }
            gui::same_line();
          }
          else {
            if (icon_button("##join_channel", icon_e::join, "Join channel", gui::get_frame_height() * 1.15f, icon_style_e::accent)) {
              This->backend_queue([channel_id = This->selected_channel_id]() -> fan::event::task_t {
                try {
                  co_await ecps_backend.channel_join(channel_id);
                  co_await ecps_backend.request_channel_list();
                  co_await ecps_backend.request_channel_session_list(channel_id);
                }
                catch (...) {
                  fan::print("Failed to join channel");
                }
                });
            }
            gui::same_line();
          }
        }

        if (icon_button("##add_channel", icon_e::plus, "New channel", gui::get_frame_height() * 1.15f)) {
          This->backend_queue([this]() -> fan::event::task_t {
            try {
              auto channel_id = co_await ecps_backend.channel_create();
              co_await ecps_backend.channel_join(channel_id, true);
              co_await ecps_backend.request_channel_list();
              co_await ecps_backend.request_channel_session_list(channel_id);
              This->selected_channel_id = channel_id;
            }
            catch (...) {
              fan::print("Failed to create channel");
            }
            });
        }
        gui::same_line();
        
        if (icon_button("##connect_server", icon_e::server, "Connect to a server", gui::get_frame_height() * 1.15f)) {
          This->drop_down_server.toggle_render_server_connect = true;
        }

        gui::end_child();

        gui::same_line(0, splitter_spacing);

        static bool is_resizing_channels = false;
        splitter_renderer_t::render_vertical_splitter("##channel_splitter_hitbox", avail_size.y,
          unclamped_details_width, channel_details_width, min_details_width, max_details_width, is_resizing_channels);

        gui::same_line(0, splitter_spacing);

        gui::begin_child("##channel_details_panel", fan::vec2(channel_details_width, avail_size.y), true);

        gui::dummy(fan::vec2(12, 0));
        gui::same_line(0, 0);
        gui::begin_group();

        if (This->selected_channel_id.i != (uint16_t)-1) {
          std::string channel_name = "Channel " + std::to_string(This->selected_channel_id.i);
          for (const auto& channel : ecps_backend.available_channels) {
            if (channel.channel_id.i == This->selected_channel_id.i) {
              channel_name = channel.name;
              break;
            }
          }
          gui::text("Channel: " + channel_name + (ecps_backend.is_hosting_channel(This->selected_channel_id) ? "  (yours)" : ""));
          if (!ecps_backend.is_hosting_channel(This->selected_channel_id) && !channel_name.empty()) {
            gui::same_line(0, 24.f);
            bool auto_join_on = This->auto_join_host == channel_name;
            if (gui::checkbox("Join automatically", &auto_join_on)) {
              This->set_auto_join(auto_join_on ? channel_name : std::string());
            }
            if (gui::is_item_hovered()) {
              gui::set_tooltip("Joins this channel by itself whenever its host is online, also after a restart");
            }
          }

          gui::separator();

          gui::spacing();

          if (gui::begin_tab_bar("ChannelDetailTabs")) {
            gui::spacing();
            gui::spacing();
            if (gui::begin_tab_item("Users", nullptr, detail_tab == 0 ? gui::tab_item_flags_set_selected : 0)) {
              if (detail_tab == 0) detail_tab = -1;

              gui::spacing();
              gui::spacing();
              if (icon_button("##refresh_users", icon_e::refresh, "Refresh")) {
                This->backend_queue([channel_id = This->selected_channel_id]() -> fan::event::task_t {
                  try {
                    co_await ecps_backend.request_channel_session_list(channel_id);
                  }
                  catch (...) {
                    fan::print("Failed to refresh session list");
                  }
                  });
              }

              gui::spacing();
              gui::spacing();
              gui::spacing();

              gui::set_next_window_bg_alpha(0.99);
              // the host's buttons live in their own scrolling child below the list, so they can't run off the panel
              f32_t host_controls_height = is_host_of_selected ? gui::get_frame_height() * (This->stream_settings.share_audio ? 11.f : 6.f) : 0.f;
              if (gui::begin_child("SessionList", fan::vec2(0, -host_controls_height), true)) {
                auto it = ecps_backend.channel_sessions.find(This->selected_channel_id.i);
                if (it != ecps_backend.channel_sessions.end()) {
                  auto sharing_now = ecps_backend.streamers_of(This->selected_channel_id);
                  for (const auto& session : it->second) {
                    bool is_sharing = std::find(sharing_now.begin(), sharing_now.end(), (std::uint32_t)session.session_id.i) != sharing_now.end();
                    bool is_me = session.session_id.i == ecps_backend.session_id.i;
                    bool has_buttons = already_in_selected && !is_me;
                    gui::push_id((int)session.session_id.i);

                    f32_t button_size = gui::get_frame_height();
                    f32_t row_height = button_size + 8.f;
                    f32_t row_width = gui::get_content_region_avail().x;
                    fan::vec2 row_at = gui::get_cursor_screen_pos();
                    if (gui::is_mouse_hovering_rect(row_at, row_at + fan::vec2(row_width, row_height))) {
                      gui::get_window_draw_list()->AddRectFilled(row_at, row_at + fan::vec2(row_width, row_height), 0xff0b0706u, 6.f);
                    }

                    gui::set_cursor_screen_pos(row_at + fan::vec2(10.f, (row_height - gui::get_font_size()) * 0.5f));
                    gui::text(fan::color(0.9f, 0.9f, 0.9f, 1.f), session.username);
                    if (is_me) {
                      gui::same_line(0, 6.f);
                      gui::text(fan::color(0.4f, 0.4f, 0.45f, 1.f), "(you)");
                    }
                    if (session.is_host) {
                      gui::same_line(0, 10.f);
                      badge("host", 0x50ff5919u, 0xffffa070u);
                    }
                    if (is_sharing) {
                      gui::same_line(0, 8.f);
                      badge("live", 0x600a0affu, 0xff6a6affu);
                    }
                    gui::same_line(0, 10.f);
                    gui::text(fan::color(0.3f, 0.3f, 0.34f, 1.f), "#" + std::to_string(session.session_id.i));

                    if (has_buttons) {
                      f32_t x = row_at.x + row_width - 8.f - button_size;
                      gui::set_cursor_screen_pos(fan::vec2(x, row_at.y + 4.f));
                      if (icon_button("##send_file", icon_e::send_file, "Send a file")) {
                        This->send_file_dialog(This->selected_channel_id, session.session_id);
                      }
                      if (is_sharing) {
                        gui::set_cursor_screen_pos(fan::vec2(x - button_size - 4.f, row_at.y + 4.f));
                        if (icon_button("##watch", icon_e::eye, "Watch")) {
                          This->set_watch(This->selected_channel_id, (std::uint32_t)session.session_id.i);
                        }
                      }
                    }
                    gui::set_cursor_screen_pos(row_at + fan::vec2(0, row_height + 2.f));
                    gui::dummy(fan::vec2(0, 0));
                    gui::pop_id();
                  }
                }
                else {
                  gui::text("No user data loaded.");
                  gui::text("Select a channel to view users.");
                }
              }
              gui::end_child();

              gui::spacing();
              if (is_host_of_selected && gui::begin_child("HostControls", fan::vec2(0, 0), 0)) {
                gui::text(ecps_backend.is_streaming_to_any_channel() ? "You are sharing your screen" : "Your screen is not shared");
                text_wrapped_disabled("Use \"Share screen\" at the top. Others join this channel to watch you.");
                if (gui::checkbox("Preview own stream", &This->show_own_stream)) {
                  if (auto rt = get_render_thread(); rt) {
                    if (This->show_own_stream) {
                      rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
                    }
                    else {
                      rt->network_frame.set_image(engine.default_texture);
                    }
                  }
                }
                if (!This->show_own_stream && ecps_backend.is_streaming_to_any_channel()) {
                  // the server doesn't send a share back to its host, without this sharing looks broken
                  text_wrapped_disabled("Sharing - viewers in the channel see your screen. Tick the preview to see it here.");
                }
                This->render_share_audio_checkbox();
              }
              if (is_host_of_selected) {
                gui::end_child();
              }

              gui::end_tab_item();
            }

            if (gui::begin_tab_item("Files", nullptr, 0)) {
              This->render_file_transfers();
              gui::end_tab_item();
            }

            if (gui::begin_tab_item("MIDI", nullptr, 0)) {
              This->render_midi_tab();
              gui::end_tab_item();
            }

            if (gui::begin_tab_item("Logs", nullptr, 0)) {
              This->render_logs_tab();
              gui::end_tab_item();
            }

            if (is_host_of_selected) {
              if (gui::begin_tab_item("Stream Settings", nullptr, detail_tab == 1 ? gui::tab_item_flags_set_selected : 0)) {
                if (detail_tab == 1) detail_tab = -1;

                render_stream_settings_content_compact();
                gui::end_tab_item();
              }
            }

            gui::end_tab_bar();
          }
        }
        else {
          gui::spacing();
          gui::text("Select a channel.");
        }

        gui::end_group();
        gui::end_child();
      }
    }

    // The panel beside the stream. Sections for what you share, what you watch and how it shows, then the details
    // folded away. Each setting is one row, its name at the left and its control at the right, and what is on or off
    // is a switch
    void render_stream_settings_content_compact() {
      gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, 8.f));
      gui::push_style_var(gui::style_var_frame_rounding, 5.f);
      push_settings_colors();
      bool streaming = ecps_backend.is_streaming_to_any_channel();
      bool viewing = ecps_backend.is_viewing_any_channel();
      auto* rt = get_render_thread();

      // ---- what you share
      settings_section("SHARING");
    #if defined(_WIN32)
      This->render_capture_source();
    #endif
      if (streaming && rt) {
        This->resolution_controls.render_resolution_controls();

        settings_row("Framerate", [&] {
          gui::slider("##framerate_compact", &This->stream_settings.framerate, 3.f, 144.f);
          if (gui::is_item_deactivated_after_edit()) {
            rt->screen_encoder.config_.frame_rate = This->stream_settings.framerate;
            std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
            rt->screen_encoder.update_flags |= codec_update_e::frame_rate;
          }
        });

        settings_row("Bitrate", [&] {
          const char* bitrate_modes[] = { "Automatic", "Manual" };
          if (gui::combo("##bitrate_mode", &This->stream_settings.bitrate_mode, bitrate_modes, 2)) {
            This->stream_settings.use_adaptive_bitrate = This->stream_settings.bitrate_mode == 0;
          }
        });
        if (This->stream_settings.bitrate_mode == 1) {
          settings_row("Mbit/s", [&] {
            if (gui::slider("##bitrate_compact", &This->stream_settings.bitrate_mbps, 0.5f, 50.0f, gui::slider_flags_always_clamp)) {
              rt->screen_encoder.config_.bitrate = This->stream_settings.bitrate_mbps * 1000000;
              std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
              rt->screen_encoder.update_flags |= codec_update_e::rate_control;
            }
          });
        }

        bool remote_control = This->stream_settings.input_control != 0;
        if (settings_switch("Remote control", &remote_control,
          "Viewers can use your mouse and keyboard")) {
          This->stream_settings.input_control = remote_control ? 1 : 0;
          This->send_input_control_flag();
        }

        This->render_share_audio_checkbox();
      }
      else if (!streaming) {
        text_wrapped_disabled("Quality, audio and remote control show here while you share.");
      }

      // ---- what you watch
      if (viewing) {
        settings_section("WATCHING");
        This->render_play_audio_controls();
      }

      // ---- how it shows
      settings_section("DISPLAY");
      settings_switch("Stats over the picture", &This->show_stream_stats,
        "Drag the box to move it");

      // ---- the details, folded away
      gui::dummy(fan::vec2(0, 6.f));
      if (settings_fold("Advanced", &advanced_open)) {
        if (streaming && rt) {
          render_encoder_row(rt);
          render_scaling_row(rt);
        }
        if (viewing) {
          render_decoder_row();
        }
        if (streaming) {
          if (gui::button("Reset quality settings")) {
            This->stream_settings.selected_resolution = 0;
            This->stream_settings.framerate = 30;
            This->stream_settings.bitrate_mbps = 5;
            This->stream_settings.input_control = 0;
            This->send_input_control_flag();
          }
          if (gui::is_item_hovered()) {
            gui::set_tooltip("30 fps, 5 Mbit/s, remote control off");
          }
        }
        else if (!viewing) {
          text_wrapped_disabled("Encoder and decoder settings show while you share or watch.");
        }
      }
      if (settings_fold("Network", &network_open)) {
        render_network_details();
      }
      gui::pop_style_color(settings_color_count);
      gui::pop_style_var(2);
    }
    bool advanced_open = false;
    bool network_open = false;

    void render_encoder_row(render_thread_t* rt) {
      static auto encoder_names = [] {
        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) {
          return std::vector<std::string>{"libx264"};
        }
        return rt->screen_encoder.get_encoders();
      }();
      static auto encoder_options = [] {
        std::vector<const char*> names;
        for (const auto& name : encoder_names) {
          names.push_back(name.c_str());
        }
        return names;
      }();

      settings_row("Encoder", [&] {
        if (!gui::combo("##encoder_compact", &This->stream_settings.selected_encoder, encoder_options.data(), encoder_options.size())) {
          return;
        }
        std::string encoder_name = encoder_names[This->stream_settings.selected_encoder];
        codec_config_t::codec_type_e codec_type = codec_config_t::H264;
        if (encoder_name.find("265") != std::string::npos || encoder_name.find("hevc") != std::string::npos) {
          codec_type = codec_config_t::H265;
        }
        else if (encoder_name.find("av1") != std::string::npos) {
          codec_type = codec_config_t::AV1;
        }
        rt->screen_encoder.config_.codec = codec_type;
        rt->screen_encoder.new_codec = This->stream_settings.selected_encoder;
        rt->screen_encoder.update_flags |= codec_update_e::codec;
        rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
      });
    }

    void render_scaling_row(render_thread_t* rt) {
      static auto quality_options = []() {
        std::vector<const char*> options;
        for (int i = 0; i < 5; ++i) {
          options.push_back(fan::graphics::get_scaling_quality_name(static_cast<codec_config_t::scaling_quality_e>(i)));
        }
        return options;
      }();
      settings_row("Scaling", [&] {
        if (gui::combo("##scaling_quality", &This->stream_settings.selected_scaling_quality, quality_options.data(), quality_options.size())) {
          rt->screen_encoder.update_scaling_quality(static_cast<codec_config_t::scaling_quality_e>(This->stream_settings.selected_scaling_quality));
        }
      });
    }

    void render_decoder_row() {
      static auto decoder_names = [] {
        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) {
          return std::vector<std::string>{"h264_cuvid", "auto-detect", "libx264", "libx265", "libaom-av1"};
        }
        return rt->screen_decoder.get_decoders();
      }();
      static bool decoder_inited = false;
      if (!decoder_inited) {
        decoder_inited = true;
        if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
          for (int i = 0; i < decoder_names.size(); i++) {
            if (decoder_names[i] == rt->screen_decoder.name) {
              This->stream_settings.selected_decoder = i;
              break;
            }
          }
          if (This->stream_settings.selected_decoder >= decoder_names.size()) {
            for (int i = 0; i < decoder_names.size(); i++) {
              if (decoder_names[i] == "auto-detect") {
                This->stream_settings.selected_decoder = i;
                break;
              }
            }
          }
        }
      }
      static auto decoder_options = [] {
        std::vector<const char*> names;
        for (const auto& name : decoder_names) {
          names.push_back(name.c_str());
        }
        return names;
      }();

      settings_row("Decoder", [&] {
        if (!gui::combo("##decoder_compact", &This->stream_settings.selected_decoder, decoder_options.data(), decoder_options.size())) {
          return;
        }
        auto* rt = render_thread_ptr.load(std::memory_order_acquire);
        if (!rt) {
          return;
        }
        std::string decoder_name = decoder_names[This->stream_settings.selected_decoder];
        std::string codec_type = "h264";
        if (decoder_name.find("hevc") != std::string::npos) {
          codec_type = "hevc";
        }
        else if (decoder_name.find("av1") != std::string::npos) {
          codec_type = "av1";
        }
        rt->screen_decoder.new_codec = This->stream_settings.selected_decoder;
        rt->screen_decoder.update_flags |= codec_update_e::codec;
        rt->screen_decoder.name = codec_type;
        This->request_idr_reset();
      });
    }

    // a name at the left and a value at the right, for the network details
    static void stat_row(const std::string& label, const std::string& value, fan::color color = fan::color(0.9f, 0.9f, 0.9f, 1.f)) {
      f32_t start = gui::get_cursor_pos_x();
      f32_t right = start + gui::get_content_region_avail().x;
      gui::text(fan::color(0.55f, 0.55f, 0.6f, 1.f), label);
      f32_t label_end = start + gui::calc_text_size(label).x + 10.f;
      gui::same_line();
      gui::set_cursor_pos_x(std::max(label_end, right - gui::calc_text_size(value).x));
      gui::text(color, value);
    }

    void render_network_details() {
      auto& stats = ecps_backend.view.m_stats;
      fan::color good(0.5f, 1.f, 0.6f, 1.f);
      fan::color bad(1.f, 0.5f, 0.5f, 1.f);

      stat_row("Ping", fan::to_string(ecps_backend.ping_ms, 1) + " ms");
      if (auto* rt = get_render_thread(); rt && ecps_backend.is_streaming_to_any_channel()) {
        stat_row("Encode path", rt->screen_encoder.encoder_.uses_gpu_frames() ? "GPU" : "CPU");
      }
      stat_row("Capture", fan::to_string(stage_times.capture.ms.load(), 2) + " ms");
      stat_row("Encode", fan::to_string(stage_times.encode.ms.load(), 2) + " ms");
      stat_row("Decode", fan::to_string(stage_times.decode.ms.load(), 2) + " ms");
      stat_row("Upload", fan::to_string(stage_times.upload.ms.load(), 2) + " ms");

      gui::separator();
      stat_row("Frames", std::to_string(stats.Frame_Total));
      stat_row("Frames dropped", std::to_string(stats.Frame_Drop));
      if (stats.Frame_Total > 0) {
        double rate = (double)stats.Frame_Drop * 100.0 / stats.Frame_Total;
        stat_row("Frame drop rate", fan::to_string(rate, 2) + " %", rate > 5.0 ? bad : good);
      }
      std::uint64_t packet_drops = stats.Packet_HeadDrop + stats.Packet_BodyDrop;
      stat_row("Packets", std::to_string(stats.Packet_Total));
      stat_row("Packets lost", std::to_string(packet_drops));
      if (gui::is_item_hovered()) {
        gui::set_tooltip(std::to_string(stats.Packet_HeadDrop) + " first parts, " + std::to_string(stats.Packet_BodyDrop) + " other parts");
      }
      if (stats.Packet_Total > 0) {
        double rate = (double)packet_drops * 100.0 / stats.Packet_Total;
        stat_row("Packet loss", fan::to_string(rate, 2) + " %", rate > 2.0 ? bad : good);
      }

      if (ecps_backend.is_streaming_to_any_channel()) {
        gui::separator();
        auto& flow = ecps_backend.share.m_NetworkFlow;
        double fill = flow.BucketSize ? (double)flow.Bucket * 100.0 / flow.BucketSize : 0.0;
        stat_row("Send budget", fan::to_string(fill, 0) + " %", fill < 20.0 ? bad : good);
        if (gui::is_item_hovered()) {
          gui::set_tooltip("Low: the stream is held back");
        }
      }

      if (gui::button("Reset counters")) {
        stats.Frame_Drop = 0;
        stats.Frame_Total = 0;
        stats.Packet_Total = 0;
        stats.Packet_HeadDrop = 0;
        stats.Packet_BodyDrop = 0;
      }
    }

    void process_stream_settings() {

      fan::vec2 avail_size = gui::get_content_region_avail();
      // the toolbar floats over the picture now, the stream gets the whole height
      f32_t content_height = avail_size.y - gui::get_style().ItemSpacing.y;
      f32_t left_window_padding = 8.0f;
      f32_t right_window_padding = 12.0f;
      f32_t splitter_spacing = 12.0f;
      f32_t splitter_width = 2.0f;

      f32_t min_settings_width = 250.0f;
      f32_t max_settings_width = avail_size.x * 0.6f;

      static f32_t unclamped_settings_width = This->stream_settings.settings_panel_width;

      This->stream_settings.settings_panel_width = std::clamp(
        unclamped_settings_width,
        min_settings_width,
        max_settings_width
      );

      f32_t total_spacing = left_window_padding + splitter_spacing + splitter_width + splitter_spacing + right_window_padding;
      f32_t stream_width = avail_size.x - This->stream_settings.settings_panel_width - total_spacing;

      gui::dummy(fan::vec2(left_window_padding, 0));
      gui::same_line(0, 0);

      gui::set_next_window_bg_alpha(0);
      gui::begin_child("##stream_display_split", fan::vec2(stream_width, content_height), false);

      gui::set_viewport(engine.orthographic_render_view);

      if (auto rt = get_render_thread(); rt) {
        fan::vec2 stream_area = gui::get_content_region_avail();
        This->network_frame_helper.setup_network_frame(stream_area);
        This->render_fps_counter(stream_width);
        This->draw_host_cursor();

        if (rt->network_frame.get_image() == engine.default_texture) {
          This->render_stream_empty_hint(stream_area);
        }

#if ecps_debug_prints >= 3
        static int debug_counter = 0;
        if (++debug_counter % 300 == 1) {
          fan::vec2 fitted_size = stream_area_calculator_t::calculate_fitted_size(stream_area, rt->screen_decoder.decoded_size);
          f32_t frame_aspect = 16.0f / 9.0f;
          if (rt->screen_decoder.decoded_size.x > 0 && rt->screen_decoder.decoded_size.y > 0) {
            frame_aspect = static_cast<f32_t>(rt->screen_decoder.decoded_size.x) / static_cast<f32_t>(rt->screen_decoder.decoded_size.y);
          }
          fan::printf("VIEWER: stream_area={}x{}, decoded={}x{}, aspect={:.3f}, display_size={}x{}",
            stream_area.x, stream_area.y,
            rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y,
            frame_aspect, fitted_size.x * 2, fitted_size.y * 2);
        }
#endif
      }

      This->render_stream_toolbar();
      gui::end_child();

      gui::same_line(0, splitter_spacing);

      splitter_renderer_t::render_vertical_splitter("##stream_splitter_hitbox", content_height,
        unclamped_settings_width, This->stream_settings.settings_panel_width, min_settings_width, max_settings_width, This->stream_settings.is_resizing);

      gui::same_line(0, splitter_spacing);

      gui::begin_child("##stream_settings_panel", fan::vec2(This->stream_settings.settings_panel_width, content_height), true);

      gui::dummy(fan::vec2(12, 0));
      gui::same_line(0, 0);
      gui::begin_group();

      render_stream_settings_content_compact();

      gui::end_group();
      gui::end_child();
    }

    void process_no_stream_settings() {
      fan::vec2 avail_size = gui::get_content_region_avail();
      // the toolbar floats over the picture now, the stream gets the whole height
      f32_t content_height = avail_size.y - gui::get_style().ItemSpacing.y;
      f32_t side_padding = 12.0f;

      gui::dummy(fan::vec2(side_padding, 0));
      gui::same_line(0, 0);

      gui::set_next_window_bg_alpha(0);
      gui::begin_child("##stream_display", fan::vec2(avail_size.x - (side_padding * 2), content_height), false);

      gui::set_viewport(engine.orthographic_render_view);

      if (ecps_backend.channel_info.empty()) {
        if (auto rt = get_render_thread(); rt) {
          rt->network_frame.set_image(engine.default_texture);
        }
      }

      if (auto rt = get_render_thread(); rt) {
        fan::vec2 stream_area = gui::get_content_region_avail();
        This->network_frame_helper.setup_network_frame(stream_area);
        This->render_fps_counter(stream_area.x);
        This->draw_host_cursor();

        if (rt->network_frame.get_image() == engine.default_texture) {
          This->render_stream_empty_hint(stream_area);
        }
      }

      This->render_stream_toolbar();
      gui::end_child();
    }

    void render_stream_view_content() {
      gui::spacing();
      gui::spacing();

      fan::vec2 avail_size = gui::get_content_region_avail();

      // the toolbar floats over the picture now, the stream gets the whole height
      f32_t content_height = avail_size.y - gui::get_style().ItemSpacing.y;

      if (avail_size.x > 0 && content_height > 0) {
        if (This->stream_settings.show_in_stream_view) {
          process_stream_settings();
        }
        else {
          process_no_stream_settings();
        }
      }

      gui::spacing();

      f32_t main_content_width = This->stream_settings.show_in_stream_view ?
        (avail_size.x - This->stream_settings.settings_panel_width - 40.0f) :
        avail_size.x;

    }

    int main_tab = 0;
    int detail_tab = 0;
    int current_tab = 0;
    bool p_open = true;
    bool auto_refresh = true;
    int refresh_interval = 1;
    std::string search_filter;
  }window_handler;

  struct resolution_gui_controls_t {
    int selected_aspect_ratio = 4;
    int selected_resolution = 0;
    std::string custom_resolution = "";

    uint32_t last_applied_width = 0;
    uint32_t last_applied_height = 0;
    bool dropdown_needs_sync = false;

    void render_resolution_controls() {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      uint32_t current_width = 0, current_height = 0;
      if (rt) {
        current_width = rt->screen_encoder.config_.width;
        current_height = rt->screen_encoder.config_.height;
      }

      if (current_width != last_applied_width || current_height != last_applied_height) {
        sync_dropdown_to_current_resolution(current_width, current_height);
        last_applied_width = current_width;
        last_applied_height = current_height;
      }

      auto aspect_options = rt->screen_encoder.resolution_manager.get_aspect_ratio_options();

      std::vector<std::string> aspect_strings;
      std::vector<const char*> aspect_cstrings;
      for (const auto& [name, ratio] : aspect_options) {
        aspect_strings.push_back(name);
      }
      for (const auto& str : aspect_strings) {
        aspect_cstrings.push_back(str.c_str());
      }

      settings_row("Aspect", [&] {
        if (gui::combo("##aspect_ratio", &selected_aspect_ratio, aspect_cstrings.data(), aspect_cstrings.size())) {
          apply_closest_resolution(current_height);
        }
      });

      std::vector<resolution_system_t::resolution_t> available_resolutions = get_filtered_resolutions();

      std::vector<std::string> res_strings;
      std::vector<const char*> res_cstrings;

      bool found_current = false;
      for (int i = 0; i < available_resolutions.size(); i++) {
        const auto& res = available_resolutions[i];
        res_strings.push_back(res.name + " (" + res.category + ")");

        if (res.width == current_width && res.height == current_height) {
          selected_resolution = i;
          found_current = true;
        }
      }

      if (!found_current && current_width > 0 && current_height > 0) {
        resolution_system_t::resolution_t current_res;
        current_res.width = current_width;
        current_res.height = current_height;
        current_res.name = std::to_string(current_width) + "x" + std::to_string(current_height);
        current_res.category = "Current";

        available_resolutions.insert(available_resolutions.begin(), current_res);
        res_strings.insert(res_strings.begin(), current_res.name + " (" + current_res.category + ")");
        selected_resolution = 0;
      }

      for (const auto& str : res_strings) {
        res_cstrings.push_back(str.c_str());
      }

      settings_row("Resolution", [&] {
        if (gui::combo("##resolution", &selected_resolution, res_cstrings.data(), res_cstrings.size())) {
          if (selected_resolution >= 0 && selected_resolution < available_resolutions.size()) {
            apply_resolution(available_resolutions[selected_resolution]);
          }
        }
      });
    }

  private:
    std::vector<resolution_system_t::resolution_t> get_filtered_resolutions() {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt) return {};
      if (selected_aspect_ratio == 4) {
        return rt->screen_encoder.resolution_manager.detected_info.matching_aspect_resolutions;
      }
      else {
        auto aspect_options = rt->screen_encoder.resolution_manager.get_aspect_ratio_options();
        f32_t selected_aspect = aspect_options[selected_aspect_ratio].second;
        return resolution_system_t::get_resolutions_by_aspect(selected_aspect, 0.01f);
      }
    }

    void sync_dropdown_to_current_resolution(uint32_t current_width, uint32_t current_height) {
      if (current_width == 0 || current_height == 0) return;

      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt) return;

      f32_t current_aspect = static_cast<f32_t>(current_width) / current_height;
      auto aspect_options = rt->screen_encoder.resolution_manager.get_aspect_ratio_options();

      // auto can match the same ratio as a named option, keep what the user picked
      if (std::abs(aspect_options[selected_aspect_ratio].second - current_aspect) >= 0.02f) {
        for (int i = 0; i < aspect_options.size(); i++) {
          if (std::abs(aspect_options[i].second - current_aspect) < 0.02f) {
            selected_aspect_ratio = i;
            break;
          }
        }
      }

      auto available_resolutions = get_filtered_resolutions();
      selected_resolution = 0;

      for (int i = 0; i < available_resolutions.size(); i++) {
        if (available_resolutions[i].width == current_width &&
          available_resolutions[i].height == current_height) {
          selected_resolution = i;
          break;
        }
      }
    }

    void apply_closest_resolution(uint32_t current_height) {
      auto resolutions = get_filtered_resolutions();
      auto height_distance = [&](const auto& resolution) {
        return std::abs((int)resolution.height - (int)current_height);
      };
      auto closest = std::min_element(resolutions.begin(), resolutions.end(), [&](const auto& a, const auto& b) {
        return height_distance(a) < height_distance(b);
      });
      if (closest != resolutions.end()) {
        apply_resolution(*closest);
      }
    }

    void apply_resolution(const resolution_system_t::resolution_t& resolution) {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (rt) {
        rt->screen_encoder.set_user_resolution(resolution.width, resolution.height);
        rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;

        if (rt->ecps_gui.stream_settings.bitrate_mode == 0) {
          rt->screen_encoder.config_.bitrate = dynamic_config_t::get_adaptive_bitrate();
          {
            std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
            rt->screen_encoder.update_flags |= codec_update_e::rate_control;
          }
        }

        last_applied_width = resolution.width;
        last_applied_height = resolution.height;
      }
    }

    void parse_and_apply_custom_resolution() {
      size_t x_pos = custom_resolution.find('x');
      if (x_pos == std::string::npos) x_pos = custom_resolution.find('X');

      if (x_pos != std::string::npos && x_pos > 0 && x_pos < custom_resolution.length() - 1) {
        try {
          uint32_t width = std::stoul(custom_resolution.substr(0, x_pos));
          uint32_t height = std::stoul(custom_resolution.substr(x_pos + 1));

          if (width >= 640 && width <= 7680 && height >= 480 && height <= 4320) {
            resolution_system_t::resolution_t custom_res;
            custom_res.width = width;
            custom_res.height = height;
            custom_res.name = std::to_string(width) + "x" + std::to_string(height);
            custom_res.category = "Custom";
            apply_resolution(custom_res);
          }
          else {
            fan::print("Resolution out of bounds: " + custom_resolution);
          }
        }
        catch (const std::exception& e) {
          fan::print("Invalid resolution format: " + custom_resolution);
        }
      }
    }
  }resolution_controls;

#undef This
#define This this

  void request_idr_reset() {
    backend_queue([]() -> fan::event::task_t {
      co_await ecps_backend.request_idr_reset();
      });
  }

  void render_fullscreen_stream() {
    auto* viewport = gui::get_main_viewport();
    fan::vec2 window_pos = fan::vec2(viewport->WorkPos.x, viewport->WorkPos.y);
    fan::vec2 window_size = fan::vec2(viewport->WorkSize.x, viewport->WorkSize.y);
    gui::set_next_window_pos(window_pos);
    gui::set_next_window_size(window_size);
    gui::set_next_window_bg_alpha(1.0f);

    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_window_border_size, 0.0f);
    gui::push_style_var(gui::style_var_window_rounding, 0.0f);
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_frame_padding, fan::vec2(0, 0));

    if (gui::begin("##FullscreenStream", nullptr,
      gui::window_flags_no_docking |
      gui::window_flags_no_saved_settings |
      gui::window_flags_no_move |
      gui::window_flags_no_collapse |
      gui::window_flags_no_resize |
      gui::window_flags_no_title_bar |
      gui::window_flags_no_scrollbar |
      gui::window_flags_no_scroll_with_mouse |
      gui::window_flags_no_background)) {

      fan::vec2 window_content_size = gui::get_content_region_avail();

      gui::set_viewport(engine.orthographic_render_view);

      fan::vec2 stream_area = window_content_size;
      if (This->stream_settings.show_in_stream_view) {
        stream_area.x -= This->stream_settings.settings_panel_width;
      }

      auto rt = get_render_thread();
      if (!rt) {
        gui::pop_style_var(5);
        gui::end();
        return;
      }

      This->network_frame_helper.setup_network_frame(stream_area);
      This->render_fps_counter(stream_area.x);
      This->draw_host_cursor();

      gui::pop_style_var(5);

      if (This->stream_settings.show_in_stream_view) {
        gui::set_cursor_pos(fan::vec2(stream_area.x, 0));
        gui::begin_child("##fullscreen_settings_panel",
          fan::vec2(This->stream_settings.settings_panel_width, window_content_size.y),
          true);

        gui::dummy(fan::vec2(12, 0));
        gui::same_line(0, 0);
        gui::begin_group();

        window_handler.render_stream_settings_content_compact();

        gui::end_group();
        gui::end_child();
      }

      auto& io = gui::get_io();
      auto now = std::chrono::steady_clock::now();
      f32_t frame_height = gui::get_frame_height();
      f32_t zone = frame_height * 3.f;
      bool mouse_near = io.MousePos.x >= window_pos.x && io.MousePos.x <= window_pos.x + window_size.x &&
        io.MousePos.y >= window_pos.y + window_size.y - zone && io.MousePos.y <= window_pos.y + window_size.y;
      bool has_picture = rt->network_frame.get_image() != engine.default_texture;
      if (mouse_near || !has_picture) {
        fullscreen_toolbar_visible_until = now + std::chrono::milliseconds(1500);
      }
      f32_t target = now < fullscreen_toolbar_visible_until ? 1.f : 0.f;
      fullscreen_toolbar_alpha += (target - fullscreen_toolbar_alpha) * std::min(1.f, io.DeltaTime * 14.f);
      if (target == 0.f && fullscreen_toolbar_alpha < 0.02f) {
        fullscreen_toolbar_alpha = 0.f;
      }
      else {
        gui::push_style_var(gui::style_var_alpha, std::max(fullscreen_toolbar_alpha, 0.02f));

        gui::push_style_var(gui::style_var_frame_padding, fan::vec2(12, 8));

        stream_button_renderer_t::render_centered_stream_button(This->selected_channel_id, stream_area, window_content_size);

        gui::pop_style_var(1);

        f32_t icon_size = 24.0f;
        f32_t button_spacing = 10.0f;
        f32_t margin = 20.0f;

        f32_t settings_x = stream_area.x - (icon_size * 2) - button_spacing - margin;
        f32_t settings_y = window_size.y - icon_size - margin;
        gui::set_cursor_pos(fan::vec2(settings_x, settings_y));

        if (icon_button("##btn_fullscreen_settings", icon_e::gear,
          This->stream_settings.show_in_stream_view ? "Hide settings" : "Settings", icon_size + 12.f, icon_style_e::overlay)) {
          This->stream_settings.show_in_stream_view = !This->stream_settings.show_in_stream_view;
        }

        gui::same_line();

        if (icon_button("##btn_exit_fullscreen", icon_e::exit_fullscreen, "Leave full screen", icon_size + 12.f, icon_style_e::overlay)) {
          is_fullscreen_stream = false;
          engine.window.set_windowed();
        }

        gui::pop_style_var();
      }
    }
    else {
      gui::pop_style_var(5);
    }

    gui::end();
  }

  void render_fps_counter(f32_t stream_width, f32_t font_size = 15.f) {
    auto rt = get_render_thread();
    if (!rt || rt->network_frame.get_image() == engine.default_texture) {
      return;
    }

    f32_t decoder_fps = rt->displayed_fps;
    std::string fps_text = fan::to_string(decoder_fps, 1);

    gui::push_font(gui::get_font(font_size));

    fan::vec2 text_size = gui::calc_text_size(fps_text);
    fan::vec2 bg_padding = fan::vec2(gui::get_font_size() * 0.4f, gui::get_font_size() * 0.2f);
    fan::vec2 fps_pos = fan::vec2(stream_width - text_size.x - 20, 10);

    gui::set_cursor_pos(fps_pos);

    fan::vec2 bg_min = gui::get_cursor_screen_pos() - bg_padding;
    fan::vec2 bg_max = bg_min + text_size + bg_padding * 2;

    gui::get_window_draw_list()->AddRectFilled(
      bg_min, bg_max,
      fan::color(0, 0, 0, 0.7f).get_gui_color(),
      gui::get_font_size() * 0.2f
    );

    gui::text(fan::color(0.5f, 1, 0.5f, 1), fps_text);
    gui::pop_font();
  }

  // what the host tells the channel it allows: mouse and keyboard while streaming, MIDI whenever
  std::uint8_t wanted_channel_flags() const {
    std::uint8_t flags = 0;
    if (stream_settings.input_control != 0) {
      flags |= ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl;
    }
    if (allow_midi) {
      flags |= ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl;
    }
    return flags;
  }

  // sends the flags of every channel we host when they changed, and again every 10 s in case the server lost them
  void update_channel_flags() {
    auto now = std::chrono::steady_clock::now();
    bool refresh = now - flags_sent_at > std::chrono::seconds(10);
    std::uint8_t wanted = wanted_channel_flags();
    for (const auto& channel : ecps_backend.channel_info) {
      if (!ecps_backend.is_hosting_channel(channel.channel_id)) {
        continue;
      }
      auto sent = sent_channel_flags.find((std::uint32_t)channel.channel_id.i);
      if (!refresh && sent != sent_channel_flags.end() && sent->second == wanted) {
        continue;
      }
      sent_channel_flags[(std::uint32_t)channel.channel_id.i] = wanted;
      backend_queue([channel_id = channel.channel_id, session_id = channel.session_id, wanted]() -> fan::event::task_t {
        try {
          ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag_t rest;
          rest.ChannelID = channel_id;
          rest.ChannelSessionID = session_id;
          rest.Flag = wanted;
          co_await ecps_backend.tcp_write(
            ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag,
            &rest,
            sizeof(rest)
          );
        }
        catch (...) {}
      });
    }
    if (refresh) {
      flags_sent_at = now;
    }
  }

  void send_input_control_flag() {
    if (stream_settings.input_control == 0) {
      backend_queue([]() -> fan::event::task_t {
        host_input.close();
        co_return;
      });
    }
    // forgetting what was sent makes the next frame send it
    sent_channel_flags.clear();
  }

  // ---- secure file transfer ----

  std::string file_peer_name(ecps_backend_t::Protocol_SessionID_t peer) {
    for (const auto& [channel_id, sessions] : ecps_backend.channel_sessions) {
      for (const auto& session : sessions) {
        if (session.session_id.i == peer.i) {
          return session.username;
        }
      }
    }
    return "User " + std::to_string(peer.i);
  }

  void send_file_dialog(ecps_backend_t::Protocol_ChannelID_t channel_id, ecps_backend_t::Protocol_SessionID_t peer) {
    fan::graphics::open_file("", [this, channel_id, peer](std::string_view path) {
      // the task is returned, not run inside the lambda, so nothing dangles when the queue drops it
      backend_queue([channel_id, peer, path = std::string(path)]() -> fan::event::task_t {
        return ecps_backend.send_file(channel_id, peer, path);
      });
    });
  }

  void queue_file_action(uint32_t id, ecps_backend_t::Protocol_SessionID_t peer, bool outgoing, int action) {
    backend_queue([id, peer, outgoing, action]() -> fan::event::task_t {
      switch (action) {
      case 0: return ecps_backend.accept_file(id, peer);
      case 1: return ecps_backend.decline_file(id, peer);
      default: return ecps_backend.cancel_file(id, peer, outgoing);
      }
    });
  }

  // ---- drag and drop ----

  fan::window_t::drop_handle_t file_drop_handle;
  std::vector<std::string> dropped_files;
  bool drop_popup_requested = false;

  // dropped files go to the selected channel if we are in it, else to the only one we are in
  bool drop_channel(ecps_backend_t::Protocol_ChannelID_t& out) {
    for (const auto& c : ecps_backend.channel_info) {
      if (c.channel_id.i == selected_channel_id.i) {
        out = c.channel_id;
        return true;
      }
    }
    if (ecps_backend.channel_info.size() == 1) {
      out = ecps_backend.channel_info[0].channel_id;
      return true;
    }
    return false;
  }

  std::vector<ecps_backend_t::session_info_t> drop_peers(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    std::vector<ecps_backend_t::session_info_t> peers;
    auto it = ecps_backend.channel_sessions.find(channel_id.i);
    if (it != ecps_backend.channel_sessions.end()) {
      for (const auto& session : it->second) {
        if (session.session_id.i != ecps_backend.session_id.i) {
          peers.push_back(session);
        }
      }
    }
    return peers;
  }

  void send_dropped_files(ecps_backend_t::Protocol_ChannelID_t channel_id, ecps_backend_t::Protocol_SessionID_t peer) {
    for (const auto& path : dropped_files) {
      backend_queue([channel_id, peer, path]() -> fan::event::task_t {
        return ecps_backend.send_file(channel_id, peer, path);
      });
    }
    dropped_files.clear();
  }

  void render_file_drop() {
    static bool popup_shown = false;

    if (drop_popup_requested) {
      drop_popup_requested = false;
      ecps_backend_t::Protocol_ChannelID_t channel_id;
      bool has_channel = drop_channel(channel_id);
      auto peers = has_channel ? drop_peers(channel_id) : std::vector<ecps_backend_t::session_info_t>{};
      if (peers.size() == 1) {
        send_dropped_files(channel_id, peers[0].session_id);
      }
      else {
        if (has_channel) {
          // the user list may not be loaded yet, the popup picks it up when it arrives
          backend_queue([channel_id]() -> fan::event::task_t {
            return ecps_backend.request_channel_session_list(channel_id);
          });
        }
        gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
        gui::open_popup("Send dropped files");
        popup_shown = true;
      }
    }
    if (!popup_shown) {
      return;
    }
    if (!gui::begin_popup_modal("Send dropped files")) {
      popup_shown = false;
      dropped_files.clear();
      return;
    }
    popup_size = gui::get_window_size();

    std::string what = ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(dropped_files.empty() ? std::string() : dropped_files[0]).filename());
    if (dropped_files.size() > 1) {
      what += " and " + std::to_string(dropped_files.size() - 1) + " more";
    }
    gui::text(what);
    gui::spacing();

    ecps_backend_t::Protocol_ChannelID_t channel_id;
    auto peers = drop_channel(channel_id) ? drop_peers(channel_id) : std::vector<ecps_backend_t::session_info_t>{};
    if (peers.empty()) {
      gui::text("There is nobody to send to. Join a channel with another user first.");
    }
    else {
      gui::text("Send to:");
      for (const auto& peer : peers) {
        gui::push_id((int)peer.session_id.i);
        if (gui::button(peer.username)) {
          send_dropped_files(channel_id, peer.session_id);
          gui::close_current_popup();
          popup_shown = false;
        }
        gui::pop_id();
      }
    }
    gui::spacing();
    if (popup_shown && gui::button("Cancel")) {
      dropped_files.clear();
      gui::close_current_popup();
      popup_shown = false;
    }
    gui::end_popup();
  }

  static std::string file_progress_text(const ecps_backend_t::file_transfer_view_t& v) {
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%.1f%%", v.size ? (double)v.transferred * 100.0 / (double)v.size : 0.0);
    return std::string(percent) + "  " + ecps_backend_t::file_format_bytes(v.transferred) + " / " + ecps_backend_t::file_format_bytes(v.size);
  }

  // a modal for the oldest file somebody wants to send us
  void render_file_offers() {
    using state_e = ecps_backend_t::file_transfer_t::state_e;
    static bool popup_shown = false;

    auto views = ecps_backend.file_transfer_views();
    const ecps_backend_t::file_transfer_view_t* offer = nullptr;
    for (const auto& v : views) {
      if (!v.outgoing && v.state == state_e::offered) {
        offer = &v;
        break;
      }
    }
    if (offer && !popup_shown) {
      gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
      gui::open_popup("Incoming file");
      popup_shown = true;
    }
    if (!popup_shown) {
      return;
    }
    if (!gui::begin_popup_modal("Incoming file")) {
      popup_shown = false;
      return;
    }
    if (!offer) {
      gui::close_current_popup();
      popup_shown = false;
    }
    else {
      popup_size = gui::get_window_size();
      gui::text(file_peer_name(offer->peer) + " wants to send you a file");
      gui::spacing();
      gui::text(offer->name);
      gui::text(ecps_backend_t::file_format_bytes(offer->size));
      gui::spacing();
      text_wrapped_disabled("End-to-end encrypted. It is saved to the ecps_downloads folder.");
      gui::spacing();
      if (gui::button("Accept")) {
        queue_file_action(offer->id, offer->peer, false, 0);
      }
      gui::same_line();
      if (gui::button("Decline")) {
        queue_file_action(offer->id, offer->peer, false, 1);
      }
    }
    gui::end_popup();
  }

  void render_file_transfers() {
    using state_e = ecps_backend_t::file_transfer_t::state_e;
    auto views = ecps_backend.file_transfer_views();

    gui::spacing();
    text_wrapped_disabled("Use 'Send file' next to a user in the Users tab.");
    gui::spacing();

    if (gui::begin_child("##file_transfers", fan::vec2(0, -80), true)) {
      if (views.empty()) {
        gui::text("No file transfers yet.");
      }
      for (size_t i = views.size(); i-- > 0;) {
        const auto& v = views[i];
        gui::push_id((int)i);

        gui::text(std::string(v.outgoing ? "Sending to " : "Receiving from ") + file_peer_name(v.peer));
        gui::text(v.name + "  (" + ecps_backend_t::file_format_bytes(v.size) + ")");
        if (v.state == state_e::transferring) {
          gui::text(file_progress_text(v));
        }
        else {
          gui::text(v.status);
        }
        if (v.state == state_e::done && !v.outgoing) {
          text_wrapped_disabled("Saved to " + v.path);
        }
        if (!v.verification.empty()) {
          gui::text("Verification code: " + v.verification);
        }

        if (!v.outgoing && v.state == state_e::offered) {
          if (gui::button("Accept")) {
            queue_file_action(v.id, v.peer, false, 0);
          }
          gui::same_line();
          if (gui::button("Decline")) {
            queue_file_action(v.id, v.peer, false, 1);
          }
        }
        else if (!ecps_backend_t::file_state_finished(v.state)) {
          if (gui::button("Cancel")) {
            queue_file_action(v.id, v.peer, v.outgoing, 2);
          }
        }
        gui::separator();
        gui::pop_id();
      }
    }
    gui::end_child();

    if (gui::button("Clear finished")) {
      ecps_backend.clear_finished_file_transfers();
    }
    text_wrapped_disabled("If the verification code is the same for both of you, nobody in between,");
    text_wrapped_disabled("the server included, could have read or changed the file.");
  }

  void render_share_audio_checkbox() {
    if (!fan::audio::loopback_capture_t::is_supported()) {
      return;
    }
    if (settings_switch("Share audio", &stream_settings.share_audio, "Send your computer's sound")) {
      save_audio_settings();
    }
    if (!stream_settings.share_audio) {
      return;
    }

    auto& devices = refresh_loopback_devices(false);
    static bool combo_was_open = false;
    std::string preview = stream_settings.audio_device.empty() ? devices.front().name : "(unplugged device)";
    for (const auto& device : devices) {
      if (!stream_settings.audio_device.empty() && device.id == stream_settings.audio_device) {
        preview = device.name;
      }
    }

    bool combo_open = false;
    settings_row("Device", [&] {
    combo_open = gui::begin_combo("##audio_device", preview);
    if (combo_open) {
      // re-list on open so newly plugged devices show up
      if (!combo_was_open) {
        refresh_loopback_devices(true);
      }
      for (const auto& device : devices) {
        gui::push_id(device.id.empty() ? std::string("default") : device.id);
        if (gui::selectable(device.name, device.id == stream_settings.audio_device)) {
          stream_settings.audio_device = device.id;
          save_audio_settings();
        }
        gui::pop_id();
      }
      gui::end_combo();
    }
    });
    combo_was_open = combo_open;

    auto& settings = stream_settings.audio_devices[stream_settings.audio_device];
    bool can_exclude = fan::audio::loopback_capture_t::can_exclude_processes();
    bool exclude = can_exclude && settings.exclude_discord;
    if (can_exclude && settings_switch("Leave out Discord", &exclude,
      "Every app except Discord, so calls stay private")) {
      settings.exclude_discord = exclude;
      save_audio_settings();
    }
    if (exclude && !find_sonar_gaming().empty()) {
      text_wrapped_disabled("Sonar: recording Sonar - Gaming (Discord and mic stay on Chat/Mic)");
    }
    else if (exclude && audio_share.capture.is_running()) {
      text_wrapped_disabled(audio_share.capture.get_excluded_pid()
        ? "Recording all apps except Discord"
        : audio_share.exclude_failed ? "Could not exclude Discord, recording the device" : "Discord is not running");
    }

    settings_row("Level", [&] {
      gui::slider("##audio_gain", &settings.gain, 0.f, 2.f, gui::slider_flags_always_clamp);
      if (gui::is_item_deactivated_after_edit()) {
        save_audio_settings();
      }
    });
  }

  // the list is re-read at most every 10 s unless forced (it also detects Sonar starting or stopping)
  const std::vector<fan::audio::loopback_device_t>& refresh_loopback_devices(bool force) {
    auto now = std::chrono::steady_clock::now();
    if (force || loopback_devices.empty() || now - loopback_devices_listed > std::chrono::seconds(10)) {
      loopback_devices = fan::audio::get_loopback_devices();
      loopback_devices_listed = now;
    }
    return loopback_devices;
  }

  // SteelSeries Sonar re-plays its Chat (Discord) and Mic monitoring mixes from its own process, so
  // process loopback excluding Discord still records them; its Gaming device carries only game apps
  std::string find_sonar_gaming() {
    for (const auto& device : refresh_loopback_devices(false)) {
      if (device.name.find("Sonar - Gaming") != std::string::npos) {
        return device.id;
      }
    }
    return {};
  }

  fan::audio::loopback_capture_t::options_t get_capture_options() {
    auto& settings = stream_settings.audio_devices[stream_settings.audio_device];
    bool exclude = settings.exclude_discord && fan::audio::loopback_capture_t::can_exclude_processes();
    std::string device_id = stream_settings.audio_device;
    if (exclude && stream_settings.share_audio) {
      if (std::string sonar = find_sonar_gaming(); !sonar.empty()) {
        device_id = std::move(sonar);
        exclude = false;
      }
    }
    return {
      .device_id = device_id,
      .exclude_discord = exclude,
      .gain = settings.gain,
    };
  }

  std::vector<fan::audio::loopback_device_t> loopback_devices;
  std::chrono::steady_clock::time_point loopback_devices_listed;

  void render_play_audio_controls() {
    if (settings_switch("Play audio", &stream_settings.play_audio, "Play the stream's sound")) {
      save_audio_settings();
    }
    if (stream_settings.play_audio) {
      settings_row("Volume", [&] {
        gui::slider("##audio_volume", &stream_settings.audio_volume, 0.f, 2.f, gui::slider_flags_always_clamp);
        if (gui::is_item_deactivated_after_edit()) {
          save_audio_settings();
        }
      });
    }
  }

  void save_audio_settings() {
    fan::json audio;
    audio["share"] = stream_settings.share_audio;
    audio["play"] = stream_settings.play_audio;
    audio["volume"] = stream_settings.audio_volume;
    audio["device"] = stream_settings.audio_device;
    fan::json devices = fan::json::object();
    for (const auto& [id, settings] : stream_settings.audio_devices) {
      devices[id] = {{"gain", settings.gain}, {"exclude_discord", settings.exclude_discord}};
    }
    audio["devices"] = devices;
    write_to_config("audio", audio);
  }

  void queue_host_input(int command, auto rest) {
    backend_queue([command, rest]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          if (channel.is_viewing) {
            auto packet = rest;
            packet.ChannelID = channel.channel_id;
            packet.ChannelSessionID = channel.session_id;
            co_await ecps_backend.tcp_write(command, &packet, sizeof(packet));
            break;
          }
        }
      }
      catch (...) {}
    });
  }

  void send_host_position(const fan::vec2si& position) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseCoordinate_t rest;
    rest.pos = position;
    queue_host_input(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseCoordinate, rest);
  }

  void send_host_button(std::uint8_t button, bool state, const fan::vec2si& position) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseButton_t rest;
    rest.key = button;
    rest.state = state;
    rest.pos = position;
    queue_host_input(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseButton, rest);
  }

  void send_host_midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMidi_t rest;
    rest.Status = status;
    rest.Data1 = data1;
    rest.Data2 = data2;
    backend_queue([rest]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          // any channel we are in, but don't host, whose host lets MIDI in. No stream needed
          if (!ecps_backend.is_hosting_channel(channel.channel_id) &&
            (channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl)) {
            auto packet = rest;
            packet.ChannelID = channel.channel_id;
            packet.ChannelSessionID = channel.session_id;
            co_await ecps_backend.tcp_write(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMidi, &packet, sizeof(packet));
          }
        }
      }
      catch (...) {}
    });
  }

  // the keys of our own keyboard, for the piano in the MIDI tab
  void track_local_note(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    std::uint8_t type = status & 0xF0;
    if (data1 >= local_down.size() || (status & 0x0F) == 9) {
      return;
    }
    if (type == 0x90 && data2 != 0) {
      local_down[data1] = true;
    }
    else if (type == 0x80 || type == 0x90) {
      local_down[data1] = false;
    }
  }

  static constexpr std::uint32_t midi_col(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
  }

  // 88 keys A0..C8: ours light up blue, the ones playing here from the other side orange
  void render_midi_piano() {
    static constexpr int first_note = 21;
    static constexpr int last_note = 108;
    static constexpr int white_keys = 52;
    auto is_black = [](int note) {
      int n = note % 12;
      return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    };
    fan::vec2 avail = gui::get_content_region_avail();
    f32_t width = std::max(avail.x - 4.f, 120.f);
    f32_t white_width = width / white_keys;
    f32_t height = std::clamp(white_width * 6.f, 50.f, 130.f);
    fan::vec2 origin = gui::get_cursor_screen_pos();
    gui::dummy(fan::vec2(width, height + 4.f));

    auto* list = gui::get_window_draw_list();
    auto color_of = [&](int note, bool black) -> std::uint32_t {
      bool mine = local_down[note].load(std::memory_order_relaxed);
      bool theirs = midi_host.synth.is_down((std::uint8_t)note);
      if (mine) {
        return midi_col(70, 140, 255, 255);
      }
      if (theirs) {
        return midi_col(255, 150, 40, 255);
      }
      return black ? midi_col(25, 25, 25, 255) : midi_col(235, 235, 235, 255);
    };

    int white_index = 0;
    for (int note = first_note; note <= last_note; ++note) {
      if (is_black(note)) {
        continue;
      }
      fan::vec2 min(origin.x + white_index * white_width, origin.y);
      fan::vec2 max(min.x + white_width - 1.f, origin.y + height);
      list->AddRectFilled(min, max, color_of(note, false));
      ++white_index;
    }
    white_index = 0;
    for (int note = first_note; note <= last_note; ++note) {
      if (!is_black(note)) {
        ++white_index;
        continue;
      }
      // a black key sits on the seam after the white key before it
      f32_t center = origin.x + white_index * white_width;
      fan::vec2 min(center - white_width * 0.3f, origin.y);
      fan::vec2 max(center + white_width * 0.3f, origin.y + height * 0.62f);
      list->AddRectFilled(min, max, color_of(note, true));
    }
  }

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
    list->AddTriangleFilled(point(0, 0), point(0, 17), point(12, 12), white);
    list->AddQuadFilled(point(4, 12), point(8, 20), point(11, 18), point(8, 11), white);
    list->AddTriangle(point(0, 0), point(0, 17), point(12, 12), dark, 1.5f);
    list->AddQuad(point(4, 12), point(8, 20), point(11, 18), point(8, 11), dark, 1.5f);
    list->PopClipRect();
  }

#if defined(_WIN32)
  // ---- what is captured: the whole screen, or one window (Linux asks in the portal instead) ------------------

  struct capture_window_t {
    std::uintptr_t handle;
    std::string title;
  };
  std::vector<capture_window_t> capture_windows;
  std::string capture_window_title;

  static BOOL CALLBACK collect_capture_window(HWND hwnd, LPARAM param) {
    auto* list = (std::vector<capture_window_t>*)param;
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
      return TRUE;
    }
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
      return TRUE;
    }
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == GetCurrentProcessId()) {
      return TRUE;
    }
    wchar_t title[256];
    int length = GetWindowTextW(hwnd, title, 256);
    if (length <= 0) {
      return TRUE;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, title, length, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, title, length, utf8.data(), bytes, nullptr, nullptr);
    list->push_back({(std::uintptr_t)hwnd, utf8});
    return TRUE;
  }

  // A small picture of a window as it looks now, for choosing what to share. They are taken here, on the gui thread,
  // a couple per frame at most, and only for what is shown
  struct capture_thumb_t {
    fan::graphics::image_t image;
    bool loaded = false;
    fan::vec2 size;
    std::chrono::steady_clock::time_point at;
  };
  std::unordered_map<std::uintptr_t, capture_thumb_t> capture_thumbs;
  std::vector<std::uint8_t> thumb_pixels;
  bool capture_picker_requested = false;
  bool preview_own_share = false;
  std::chrono::steady_clock::time_point capture_picker_opened_at;

  // The window is drawn through an sRGB swapchain, which brightens what it is given as if it were linear. Screen
  // pixels are sRGB already, so they go in through this table, or the pictures look washed out
  static inline const std::array<std::uint8_t, 256> srgb_to_linear = [] {
    std::array<std::uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
      f64_t c = i / 255.0;
      f64_t linear = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
      table[i] = (std::uint8_t)std::lround(linear * 255.0);
    }
    return table;
  }();

  // thumb_pixels gets the picture as rgba, at most max_width wide. handle 0 is the screen
  bool grab_window_rgba(std::uintptr_t handle, int max_width, fan::vec2i& size) {
    HWND hwnd = (HWND)handle;
    RECT rect;
    int width, height;
    if (handle == 0) {
      width = GetSystemMetrics(SM_CXSCREEN);
      height = GetSystemMetrics(SM_CYSCREEN);
    }
    else {
      if (!IsWindow(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &rect)) {
        return false;
      }
      width = rect.right - rect.left;
      height = rect.bottom - rect.top;
    }
    if (width < 16 || height < 16) {
      return false;
    }
    HDC screen_dc = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen_dc);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    // the screen is shrunk while it is copied, a window is shrunk after
    int bitmap_width = handle == 0 ? std::min(width, max_width) : width;
    int bitmap_height = handle == 0 ? std::max(1, height * bitmap_width / width) : height;
    info.bmiHeader.biWidth = bitmap_width;
    info.bmiHeader.biHeight = -bitmap_height;
    void* bits = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (bitmap) {
      HGDIOBJ old = SelectObject(dc, bitmap);
      if (handle == 0) {
        SetStretchBltMode(dc, HALFTONE);
        ok = StretchBlt(dc, 0, 0, bitmap_width, bitmap_height, screen_dc, 0, 0, width, height, SRCCOPY) != 0;
      }
      else {
        ok = PrintWindow(hwnd, dc, 2) != 0;
      }
      if (ok) {
        int step = handle == 0 ? 1 : std::max(1, bitmap_width / max_width);
        int out_width = bitmap_width / step;
        int out_height = bitmap_height / step;
        thumb_pixels.resize((std::size_t)out_width * out_height * 4);
        const std::uint8_t* source = (const std::uint8_t*)bits;
        for (int y = 0; y < out_height; ++y) {
          for (int x = 0; x < out_width; ++x) {
            const std::uint8_t* from = source + ((std::size_t)y * step * bitmap_width + (std::size_t)x * step) * 4;
            std::uint8_t* to = &thumb_pixels[((std::size_t)y * out_width + x) * 4];
            to[0] = srgb_to_linear[from[2]];
            to[1] = srgb_to_linear[from[1]];
            to[2] = srgb_to_linear[from[0]];
            to[3] = 255;
          }
        }
        size = fan::vec2i(out_width, out_height);
      }
      SelectObject(dc, old);
      DeleteObject(bitmap);
    }
    if (dc) {
      DeleteDC(dc);
    }
    ReleaseDC(nullptr, screen_dc);
    return ok;
  }

  // takes a new picture when the old one is older than interval_ms. budget counts the pictures left for this frame
  capture_thumb_t& refresh_thumb(std::uintptr_t handle, int max_width, int interval_ms, int& budget) {
    auto& thumb = capture_thumbs[handle];
    auto now = std::chrono::steady_clock::now();
    if (budget <= 0 || (thumb.loaded && now - thumb.at < std::chrono::milliseconds(interval_ms))) {
      return thumb;
    }
    --budget;
    thumb.at = now;
    fan::vec2i size;
    if (grab_window_rgba(handle, max_width, size)) {
      fan::image::info_t image_info {thumb_pixels.data(), size, 4};
      if (thumb.image.valid()) {
        thumb.image.reload(image_info);
      }
      else {
        thumb.image = engine.image_load(image_info);
      }
      thumb.size = fan::vec2((f32_t)size.x, (f32_t)size.y);
      thumb.loaded = true;
    }
    return thumb;
  }

  void refresh_capture_windows() {
    capture_windows.clear();
    EnumWindows(collect_capture_window, (LPARAM)&capture_windows);
    for (auto it = capture_thumbs.begin(); it != capture_thumbs.end();) {
      bool alive = it->first == 0 || std::any_of(capture_windows.begin(), capture_windows.end(), [&](const auto& w) { return w.handle == it->first; });
      if (!alive) {
        if (it->second.image.valid()) {
          engine.image_unload(it->second.image);
        }
        it = capture_thumbs.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  // a title cut to fit width pixels, so a long one doesn't push its neighbours around
  static std::string fit_title(std::string title, f32_t width) {
    if (gui::calc_text_size(title).x <= width) {
      return title;
    }
    while (title.size() > 1 && gui::calc_text_size(title + "...").x > width) {
      title.pop_back();
    }
    return title + "...";
  }

  static std::string shorten_title(const std::string& title, std::size_t length) {
    return title.size() <= length ? title : title.substr(0, length - 3) + "...";
  }

  // what is shared (a window, or the screen for 0), as it looks now
  void draw_share_preview(std::uintptr_t handle, f32_t max_width) {
    int budget = 1;
    auto& thumb = refresh_thumb(handle, 480, 300, budget);
    if (!thumb.loaded) {
      return;
    }
    f32_t width = std::min(max_width, thumb.size.x);
    gui::image(thumb.image, fan::vec2(width, width * thumb.size.y / thumb.size.x));
  }

  // The window picker: every window as a small picture, click one to share it, like the share pickers of chat apps
  fan::vec2 capture_picker_size;

  void render_capture_picker(fan::graphics::screen_capture_t& capture, render_thread_t* rt) {
    if (capture_picker_requested) {
      capture_picker_requested = false;
      refresh_capture_windows();
      capture_picker_opened_at = std::chrono::steady_clock::now();
      // as big as fits in the window, centered, so nothing is out of view
      fan::vec2 display = gui::get_io().DisplaySize;
      capture_picker_size = fan::vec2(std::min(820.f, display.x - 60.f), std::min(600.f, display.y - 60.f));
      gui::set_next_window_pos((display - capture_picker_size) * 0.5f);
      gui::open_popup("Choose what to share");
    }
    gui::push_style_color(gui::col_popup_bg, fan::color(0.006f, 0.006f, 0.009f, 0.99f));
    gui::push_style_var(gui::style_var_window_padding, fan::vec2(14.f, 12.f));
    bool open = gui::begin_popup("Choose what to share");
    gui::pop_style_var();
    gui::pop_style_color();
    if (!open) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - capture_picker_opened_at > std::chrono::milliseconds(1500)) {
      refresh_capture_windows(); // new windows show up, closed ones go
      capture_picker_opened_at = now;
    }

    gui::text(fan::color(0.2f, 0.45f, 1.f, 1.f), "CHOOSE WHAT TO SHARE");
    gui::same_line();
    gui::text(fan::color(0.5f, 0.5f, 0.55f, 1.f), "  click one, Esc closes");

    // the cards scroll inside a box of a fixed size, the popup itself never grows past the window
    fan::vec2 box = capture_picker_size - fan::vec2(28.f, gui::get_frame_height() + 30.f);
    gui::push_style_color(gui::col_child_bg, fan::color(0, 0, 0, 0));
    gui::begin_child("##capture_cards", box);
    std::uintptr_t current = capture.target_window.load();
    f32_t title_height = gui::get_font_size() + 10.f;
    const fan::vec2 image_area(200.f, 112.f);
    const fan::vec2 card_size(image_area.x + 12.f, image_area.y + 12.f + title_height);
    f32_t spacing = 10.f;
    int columns = std::max(1, (int)((gui::get_content_region_avail().x + spacing) / (card_size.x + spacing)));
    int budget = 2;
    bool changed = false;
    int index = 0;
    auto* list = gui::get_window_draw_list();

    auto card = [&](std::uintptr_t handle, const std::string& title) {
      if (index % columns != 0) {
        gui::same_line(0, spacing);
      }
      ++index;
      fan::vec2 at = gui::get_cursor_screen_pos();
      gui::push_id((int)(handle & 0x7fffffff));
      bool clicked = gui::invisible_button("##card", card_size);
      gui::pop_id();
      bool hovered = gui::is_item_hovered();
      bool selected = handle == current;
      if (hovered) {
        gui::set_tooltip(title);
      }
      list->AddRectFilled(at, at + card_size, hovered ? 0xff120b09u : 0xff070505u, 8.f);
      if (selected) {
        list->AddRect(at, at + card_size, accent_u32, 8.f, 0, 2.5f);
      }
      else if (hovered) {
        list->AddRect(at, at + card_size, 0x90ff5919u, 8.f, 0, 1.5f);
      }

      auto& thumb = refresh_thumb(handle, 320, handle == 0 ? 1500 : 2500, budget);
      if (thumb.loaded) {
        f32_t scale = std::min(image_area.x / thumb.size.x, image_area.y / thumb.size.y);
        fan::vec2 size = thumb.size * scale;
        gui::set_cursor_screen_pos(at + fan::vec2(6.f, 6.f) + (image_area - size) * 0.5f);
        gui::image(thumb.image, size);
      }
      else {
        fan::vec2 text = gui::calc_text_size("...");
        list->AddText(at + fan::vec2(6.f, 6.f) + (image_area - text) * 0.5f, 0xff808080u, "...");
      }
      std::string shown = fit_title(title, image_area.x);
      list->AddText(at + fan::vec2(8.f, image_area.y + 14.f), selected ? 0xffffffffu : 0xffb4b4b4u, shown.c_str());
      // back to where the card ends, so the next one lines up
      gui::set_cursor_screen_pos(at);
      gui::dummy(card_size);

      if (clicked) {
        if (handle != 0) {
          capture_window_title = title;
        }
        capture.target_window = handle;
        changed = true;
      }
    };

    card(0, "Whole screen");
    for (const auto& window : capture_windows) {
      card(window.handle, window.title);
    }
    gui::end_child();
    gui::pop_style_color();

    if (changed) {
      // viewers need a new keyframe, the picture changed size and content
      rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
      gui::close_current_popup();
    }
    gui::end_popup();
  }

  void render_capture_source() {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    auto& capture = rt->screen_encoder.mdscr;
    std::uintptr_t current = capture.target_window.load();
    if (current == 0) {
      capture_window_title.clear();
    }
    std::string label = current == 0 ? std::string("Whole screen") : shorten_title(capture_window_title, 30);
    settings_row("Source", [&] {
      std::string shown = fit_title(label, gui::get_content_region_avail().x - gui::get_style().FramePadding.x * 2.f);
      if (gui::button(shown + "##capture_source", fan::vec2(-1.f, 0.f))) {
        capture_picker_requested = true;
      }
      if (gui::is_item_hovered()) {
        gui::set_tooltip("Choose what to share");
      }
    });
    render_capture_picker(capture, rt);
    settings_switch("Preview", &preview_own_share, "See what you share");
    if (preview_own_share) {
      draw_share_preview(current, gui::get_content_region_avail().x);
    }
  }

  // The host's pointer goes to the viewers apart from the picture. Windows hands out the picture without it, and
  // a pointer the host's system doesn't show (a game hides it) must not show for the viewers either.
  // Linux hosts have the portal draw it into the picture, so they send nothing
  void update_host_cursor() {
    auto* rt = get_render_thread();
    if (!rt || !ecps_backend.is_streaming_to_any_channel() || !ecps_backend.server_new.load()) {
      cursor_last_visible = -1;
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - cursor_checked_at < std::chrono::milliseconds(8)) {
      return;
    }
    cursor_checked_at = now;

    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) {
      return;
    }
    auto& geometry = rt->screen_encoder.mdscr.Geometry;
    if (geometry.Resolution.x <= 0 || geometry.Resolution.y <= 0) {
      return;
    }
    fan::vec2i origin = rt->screen_encoder.mdscr.desktop_origin;
    f32_t nx = (f32_t)(info.ptScreenPos.x - origin.x) / (f32_t)geometry.Resolution.x;
    f32_t ny = (f32_t)(info.ptScreenPos.y - origin.y) / (f32_t)geometry.Resolution.y;
    // showing for the system, and on the screen that is being shared
    bool visible = (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr &&
      nx >= 0.f && nx <= 1.f && ny >= 0.f && ny <= 1.f;
    std::uint16_t x = (std::uint16_t)(std::clamp(nx, 0.f, 1.f) * 65535.f);
    std::uint16_t y = (std::uint16_t)(std::clamp(ny, 0.f, 1.f) * 65535.f);

    bool changed = (int)visible != cursor_last_visible || (visible && (x != cursor_last_x || y != cursor_last_y));
    bool repeat = now - cursor_sent_at > std::chrono::seconds(1);
    if (!changed && !repeat) {
      return;
    }
    cursor_last_visible = visible;
    cursor_last_x = x;
    cursor_last_y = y;
    cursor_sent_at = now;

    backend_queue([x, y, visible]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          if (channel.is_streaming) {
            ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewCursor_t rest;
            rest.ChannelID = channel.channel_id;
            rest.ChannelSessionID = channel.session_id;
            rest.X = x;
            rest.Y = y;
            rest.Visible = visible ? 1 : 0;
            co_await ecps_backend.tcp_write(
              ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewCursor, &rest, sizeof(rest));
          }
        }
      }
      catch (...) {}
    });
  }

  int cursor_last_visible = -1;
  std::uint16_t cursor_last_x = 0;
  std::uint16_t cursor_last_y = 0;
  std::chrono::steady_clock::time_point cursor_checked_at;
  std::chrono::steady_clock::time_point cursor_sent_at;
#else
  void update_host_cursor() {}
#endif

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
    text_wrapped_disabled("1 to 24 letters, digits, spaces, _ . or -. Nobody else can use the same name at the same time.");
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
    if (!ecps_backend.connected || ecps_backend.session_id.i == 0) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now < next_auto_join) {
      return;
    }
    next_auto_join = now + std::chrono::seconds(2);
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
    }
    catch (...) {}
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

  // ---- the theme -------------------------------------------------------------------------------------------
  // Pushed around the main window, so every panel, popup and control in it shares it. Dark fields and buttons,
  // blue where something is chosen or used. (The gui is drawn in linear colour and shown through sRGB: small
  // numbers here look much lighter on screen)
  int theme_colors_pushed = 0;
  int theme_vars_pushed = 0;

  void push_theme() {
    theme_colors_pushed = 0;
    theme_vars_pushed = 0;
    auto color = [this](gui::col_t index, fan::color value) {
      gui::push_style_color(index, value);
      ++theme_colors_pushed;
    };
    auto var = [this](auto index, auto value) {
      gui::push_style_var(index, value);
      ++theme_vars_pushed;
    };
    fan::color field(0.016f, 0.018f, 0.026f, 1.f);
    fan::color field_hover(0.035f, 0.045f, 0.08f, 1.f);
    fan::color blue_dim(0.045f, 0.08f, 0.2f, 1.f);
    fan::color blue(0.06f, 0.13f, 0.38f, 1.f);
    fan::color blue_bright(0.1f, 0.35f, 1.f, 1.f);
    fan::color line(0.028f, 0.031f, 0.045f, 1.f);

    color(gui::col_frame_bg, field);
    color(gui::col_frame_bg_hovered, field_hover);
    color(gui::col_frame_bg_active, blue_dim);
    color(gui::col_button, fan::color(0.022f, 0.025f, 0.036f, 1.f));
    color(gui::col_button_hovered, blue_dim);
    color(gui::col_button_active, blue);
    color(gui::col_header, blue_dim);
    color(gui::col_header_hovered, blue);
    color(gui::col_header_active, fan::color(0.08f, 0.18f, 0.5f, 1.f));
    color(gui::col_check_mark, blue_bright);
    color(gui::col_slider_grab, blue_bright);
    color(gui::col_slider_grab_active, fan::color(0.2f, 0.5f, 1.f, 1.f));
    color(gui::col_tab, fan::color(0.012f, 0.013f, 0.019f, 1.f));
    color(gui::col_tab_hovered, blue_dim);
    color(gui::col_tab_selected, blue);
    color(gui::col_tab_selected_overline, blue_bright);
    color(gui::col_tab_dimmed, fan::color(0.012f, 0.013f, 0.019f, 1.f));
    color(gui::col_tab_dimmed_selected, blue_dim);
    color(gui::col_border, line);
    color(gui::col_separator, line);
    color(gui::col_separator_hovered, blue);
    color(gui::col_separator_active, blue_bright);
    color(gui::col_popup_bg, fan::color(0.006f, 0.006f, 0.009f, 0.98f));
    color(gui::col_table_header_bg, fan::color(0.012f, 0.014f, 0.021f, 1.f));
    color(gui::col_table_border_strong, line);
    color(gui::col_table_border_light, fan::color(0.016f, 0.018f, 0.026f, 1.f));
    color(gui::col_table_row_bg_alt, fan::color(1.f, 1.f, 1.f, 0.025f));
    color(gui::col_scrollbar_bg, fan::color(0.f, 0.f, 0.f, 0.f));
    color(gui::col_scrollbar_grab, fan::color(0.03f, 0.034f, 0.05f, 1.f));
    color(gui::col_scrollbar_grab_hovered, blue_dim);
    color(gui::col_scrollbar_grab_active, blue);
    color(gui::col_text_selected_bg, fan::color(0.1f, 0.35f, 1.f, 0.35f));
    color(gui::col_resize_grip, fan::color(0.f, 0.f, 0.f, 0.f));

    var(gui::style_var_frame_rounding, 5.f);
    var(gui::style_var_child_rounding, 8.f);
    var(gui::style_var_popup_rounding, 8.f);
    var(gui::style_var_grab_rounding, 4.f);
    var(gui::style_var_tab_rounding, 5.f);
    var(gui::style_var_scrollbar_rounding, 6.f);
  }

  void pop_theme() {
    gui::pop_style_color(theme_colors_pushed);
    gui::pop_style_var(theme_vars_pushed);
  }

  // ---- building blocks of the settings panel ---------------------------------------------------------------
  // (the gui is drawn in linear colour and shown through sRGB: small numbers here look much lighter on screen)

  static constexpr std::uint32_t accent_u32 = 0xffff5919u;       // a clear blue
  static constexpr std::uint32_t accent_hover_u32 = 0xffff7a33u;

  // a section title: small, blue, with a thin line under it
  static void settings_section(const char* title) {
    gui::dummy(fan::vec2(0, 6.f));
    gui::text(fan::color(0.2f, 0.45f, 1.f, 1.f), title);
    fan::vec2 at = gui::get_cursor_screen_pos() - fan::vec2(0, 4.f);
    gui::get_window_draw_list()->AddLine(at, at + fan::vec2(gui::get_content_region_avail().x, 0), 0x50ff5919u, 1.f);
    gui::dummy(fan::vec2(0, 1.f));
  }

  // One setting: its name at the left, the control filling the rest of the row. A narrow panel puts the name above
  static void settings_row(const char* label, auto&& control) {
    f32_t start = gui::get_cursor_pos_x();
    f32_t width = gui::get_content_region_avail().x;
    f32_t label_width = gui::calc_text_size("Resolution").x + 14.f;
    gui::push_id(label);
    if (width - label_width < 190.f) {
      gui::text(fan::color(0.6f, 0.6f, 0.66f, 1.f), label);
    }
    else {
      gui::align_text_to_frame_padding();
      gui::text(fan::color(0.6f, 0.6f, 0.66f, 1.f), label);
      gui::same_line();
      gui::set_cursor_pos_x(start + label_width);
    }
    gui::push_item_width(-1);
    control();
    gui::pop_item_width();
    gui::pop_id();
  }

  // an on/off switch with its name at the left, the whole row is clickable
  static bool settings_switch(const char* label, bool* value, const char* tooltip = nullptr) {
    f32_t height = gui::get_frame_height();
    f32_t width = gui::get_content_region_avail().x;
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::push_id(label);
    bool clicked = gui::invisible_button("##switch", fan::vec2(width, height));
    gui::pop_id();
    bool hovered = gui::is_item_hovered();
    if (clicked) {
      *value = !*value;
    }
    if (hovered && tooltip) {
      gui::set_tooltip(tooltip);
    }
    auto* list = gui::get_window_draw_list();
    f32_t track_height = height * 0.62f;
    f32_t track_width = track_height * 1.85f;
    std::string shown = fit_title(label, width - track_width - 14.f);
    if (hovered && !tooltip && shown != label) {
      gui::set_tooltip(label);
    }
    list->AddText(at + fan::vec2(0, (height - gui::get_font_size()) * 0.5f), hovered ? 0xffffffffu : 0xffc8c8c8u, shown.c_str());
    fan::vec2 track = at + fan::vec2(width - track_width - 2.f, (height - track_height) * 0.5f);
    std::uint32_t track_color = *value ? (hovered ? accent_hover_u32 : accent_u32) : (hovered ? 0xff221f1eu : 0xff141312u);
    list->AddRectFilled(track, track + fan::vec2(track_width, track_height), track_color, track_height * 0.5f);
    f32_t knob_x = *value ? track_width - track_height * 0.5f : track_height * 0.5f;
    list->AddCircleFilled(track + fan::vec2(knob_x, track_height * 0.5f), track_height * 0.5f - 2.5f, *value ? 0xffffffffu : 0xffb0b0b0u);
    return clicked;
  }

  // a part of the panel that folds away: an arrow and its name on a darker strip
  static bool settings_fold(const char* title, bool* open) {
    gui::dummy(fan::vec2(0, 2.f));
    f32_t height = gui::get_frame_height();
    f32_t width = gui::get_content_region_avail().x;
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::push_id(title);
    if (gui::invisible_button("##fold", fan::vec2(width, height))) {
      *open = !*open;
    }
    gui::pop_id();
    bool hovered = gui::is_item_hovered();
    auto* list = gui::get_window_draw_list();
    list->AddRectFilled(at, at + fan::vec2(width, height), hovered ? 0xff0e0a09u : 0xff070505u, 5.f);
    f32_t size = gui::get_font_size() * 0.3f;
    fan::vec2 center = at + fan::vec2(16.f, height * 0.5f);
    if (*open) {
      list->AddTriangleFilled(center + fan::vec2(-size, -size * 0.55f), center + fan::vec2(size, -size * 0.55f),
        center + fan::vec2(0, size * 0.75f), accent_u32);
    }
    else {
      list->AddTriangleFilled(center + fan::vec2(-size * 0.55f, -size), center + fan::vec2(-size * 0.55f, size),
        center + fan::vec2(size * 0.75f, 0), accent_u32);
    }
    list->AddText(at + fan::vec2(32.f, (height - gui::get_font_size()) * 0.5f), hovered ? 0xffffffffu : 0xffd8d8d8u, title);
    return *open;
  }

  // the colours of the panel's own controls: darker fields, blue when used
  static constexpr int settings_color_count = 11;
  static void push_settings_colors() {
    gui::push_style_color(gui::col_frame_bg, fan::color(0.016f, 0.018f, 0.026f, 1.f));
    gui::push_style_color(gui::col_frame_bg_hovered, fan::color(0.035f, 0.045f, 0.08f, 1.f));
    gui::push_style_color(gui::col_frame_bg_active, fan::color(0.05f, 0.1f, 0.26f, 1.f));
    gui::push_style_color(gui::col_button, fan::color(0.02f, 0.023f, 0.034f, 1.f));
    gui::push_style_color(gui::col_button_hovered, fan::color(0.045f, 0.08f, 0.2f, 1.f));
    gui::push_style_color(gui::col_button_active, fan::color(0.06f, 0.13f, 0.38f, 1.f));
    gui::push_style_color(gui::col_slider_grab, fan::color(0.1f, 0.35f, 1.f, 1.f));
    gui::push_style_color(gui::col_slider_grab_active, fan::color(0.2f, 0.5f, 1.f, 1.f));
    gui::push_style_color(gui::col_popup_bg, fan::color(0.008f, 0.009f, 0.013f, 0.98f));
    gui::push_style_color(gui::col_header, fan::color(0.045f, 0.08f, 0.2f, 1.f));
    gui::push_style_color(gui::col_header_hovered, fan::color(0.06f, 0.13f, 0.38f, 1.f));
  }

  // ---- the interface that hides ------------------------------------------------------------------------------

  void save_ui_config() {
    fan::json ui = fan::json::object();
    ui["compact_bar"] = top_bar_compact;
    ui["autohide"] = ui_autohide;
    write_to_config("ui", ui);
  }

  // on: the bar and the tabs leave the Stream View and show again near the top edge. Off pins them back in place
  void set_ui_autohide(bool on) {
    ui_autohide = on;
    autohide_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    if (on) {
      window_handler.main_tab = 1;
      window_handler.current_tab = 1;
    }
    save_ui_config();
  }

  // how far the overlays in the stream area move down while the bar is shown over it
  f32_t stream_top_offset() const {
    return ui_autohide ? autohide_alpha * (gui::get_frame_height() + 18.f) : 0.f;
  }

  // The bar and the tabs when the interface is hidden: drawn over the top of the stream, shown while the mouse is
  // near the top edge (and a moment after), with a Pin button that puts everything back
  // The bar and the tabs when the interface is hidden: a window of its own over the top of the main window, at
  // the same place as the pinned bar, shown while the mouse is near the top edge (and a moment after)
  void render_autohide_bar(fan::vec2 area_pos, fan::vec2 area_size) {
    if (!ui_autohide) {
      return;
    }
    auto& io = gui::get_io();
    f32_t frame_height = gui::get_frame_height();
    f32_t bar_height = frame_height + 18.f;

    // a thin strip brings it back, once it is there the whole bar keeps it
    f32_t reach = autohide_alpha > 0.05f ? bar_height + 16.f : frame_height * 0.9f;
    bool mouse_near = io.MousePos.x >= area_pos.x && io.MousePos.x <= area_pos.x + area_size.x &&
      io.MousePos.y >= area_pos.y && io.MousePos.y <= area_pos.y + reach;
    auto now = std::chrono::steady_clock::now();
    if (mouse_near || autohide_bar_active) {
      autohide_visible_until = now + std::chrono::milliseconds(1200);
    }
    f32_t target = now < autohide_visible_until ? 1.f : 0.f;
    autohide_alpha += (target - autohide_alpha) * std::min(1.f, io.DeltaTime * 14.f);
    if (target == 0.f && autohide_alpha < 0.02f) {
      autohide_alpha = 0.f;
      return; // no window at all, so nothing blocks the stream under it
    }

    gui::set_next_window_pos(area_pos);
    gui::set_next_window_size(fan::vec2(area_size.x, bar_height + 2.f));
    gui::push_style_var(gui::style_var_window_padding, fan::vec2(0, 0));
    gui::push_style_var(gui::style_var_window_border_size, 0.f);
    gui::push_style_var(gui::style_var_alpha, std::max(autohide_alpha, 0.02f));
    if (gui::begin("##autohide_bar", nullptr,
      gui::window_flags_topmost | gui::window_flags_no_decoration | gui::window_flags_no_background | gui::window_flags_no_move |
      gui::window_flags_no_saved_settings | gui::window_flags_no_docking | gui::window_flags_no_focus_on_appearing |
      gui::window_flags_no_nav | gui::window_flags_no_scroll_with_mouse)) {
      render_nav_bar(gui::get_window_pos(), area_size.x, bar_height, 0.f, autohide_alpha, true);
      autohide_bar_active = gui::is_any_item_active() || gui::is_popup_open("##top_bar_menu");
    }
    gui::end();
    gui::pop_style_var(3);
  }
  bool autohide_bar_active = false;

  bool ui_autohide = false;
  f32_t autohide_alpha = 0.f;
  std::chrono::steady_clock::time_point autohide_visible_until;

  // A channel stays selected whenever there is one to select: if the selection is gone (nothing picked yet, or the
  // channel closed), yours is taken, else the first one this client is in
  void update_selected_channel() {
    if (ecps_backend.channel_info.empty()) {
      return;
    }
    if (selected_channel_id.i != (uint16_t)-1) {
      bool joined = ecps_backend.is_channel_joined(selected_channel_id);
      bool listed = ecps_backend.is_channel_available(selected_channel_id);
      if (joined || listed || !ecps_backend.channel_list_received) {
        return;
      }
    }
    for (const auto& channel : ecps_backend.channel_info) {
      if (ecps_backend.is_hosting_channel(channel.channel_id)) {
        selected_channel_id = channel.channel_id;
        return;
      }
    }
    selected_channel_id = ecps_backend.channel_info.front().channel_id;
  }

  // When the channel ticked "Join automatically" has someone sharing, its stream is shown without a click: that
  // channel is selected, its sharer is watched and the Stream View opens (from the Channels tab only, another tab
  // is where you chose to be). Once per time they start sharing, so leaving the Stream View stays possible
  void update_auto_show() {
    if (auto_join_host.empty()) {
      return;
    }
    std::uint32_t me = (std::uint32_t)ecps_backend.session_id.i;
    for (const auto& channel : ecps_backend.channel_info) {
      if (ecps_backend.is_hosting_channel(channel.channel_id) || channel_name_of(channel.channel_id) != auto_join_host) {
        continue;
      }
      std::uint32_t key = (std::uint32_t)channel.channel_id.i;
      std::vector<std::uint32_t> others;
      for (std::uint32_t session : ecps_backend.streamers_of(channel.channel_id)) {
        if (session != me) {
          others.push_back(session);
        }
      }
      if (others.empty()) {
        auto_shown.erase(key); // the next time they share, it shows again
        continue;
      }
      if (!auto_shown.insert(key).second) {
        continue;
      }
      selected_channel_id = channel.channel_id;
      set_watch(channel.channel_id, others.front());
      if (window_handler.current_tab == 0) {
        window_handler.main_tab = 1;
      }
    }
  }

  std::unordered_set<std::uint32_t> auto_shown;

  // grey text that breaks at the edge of its panel, a long hint must not run out of it
  static void text_wrapped_disabled(const std::string& text) {
    gui::push_text_wrap_pos(0.f);
    gui::text_disabled(text);
    gui::pop_text_wrap_pos();
  }

  // one line whenever the sizes the UI is laid out with change: the window, what imgui thinks the display is,
  // its framebuffer scale and the main viewport. They have to agree, a stretched picture means one of them doesn't
  void report_display_sizes() {
    auto& io = gui::get_io();
    auto* viewport = gui::get_main_viewport();
    fan::vec2i window_size = engine.window.get_size();
    std::array<f32_t, 8> now = {
      (f32_t)window_size.x, (f32_t)window_size.y, io.DisplaySize.x, io.DisplaySize.y,
      io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y, viewport->Size.x, viewport->Size.y
    };
    if (now == last_display_sizes) {
      return;
    }
    last_display_sizes = now;
  #if ecps_debug_prints >= 1
    fan::print("display: window", window_size.x, "x", window_size.y, "imgui display", io.DisplaySize.x, "x", io.DisplaySize.y,
      "framebuffer scale", io.DisplayFramebufferScale.x, "x", io.DisplayFramebufferScale.y,
      "main viewport", viewport->Size.x, "x", viewport->Size.y);
  #endif
  }
  std::array<f32_t, 8> last_display_sizes{};

  // The bar at the top: the tabs as words, sharing and what goes out and comes in, then at the right the version,
  // your name and the pin. The same bar shows over the stream when the interface is hidden, see render_autohide_bar
  void render_top_bar() {
    report_display_sizes();
    updater.tick();
    updater.tick_restart();
    f32_t bar_height = gui::get_frame_height() + 18.f;
    render_nav_bar(gui::get_window_pos(), gui::get_window_size().x, bar_height, 0.f, 1.f, false);
    gui::set_cursor_pos(fan::vec2(gui::get_style().WindowPadding.x, bar_height + 10.f));
    render_about_popup();
  }

  void render_nav_bar(fan::vec2 area_pos, f32_t width, f32_t bar_height, f32_t y_offset, f32_t fade, bool overlay) {
    using state_e = updater_t::state_e;
    auto* list = gui::get_window_draw_list();
    f32_t frame_height = gui::get_frame_height();
    std::uint32_t alpha = (std::uint32_t)(std::clamp(fade, 0.f, 1.f) * 255.f);
    auto faded = [alpha](std::uint32_t color) {
      return (color & 0x00ffffffu) | ((((color >> 24) * alpha) / 255u) << 24);
    };
    // a dark strip with a thin blue line under it
    fan::vec2 top = area_pos + fan::vec2(0, y_offset > 0 ? 0 : 0);
    list->AddRectFilled(top, top + fan::vec2(width, bar_height), faded(0xf4020101u));
    list->AddLine(top + fan::vec2(0, bar_height), top + fan::vec2(width, bar_height), faded(0x70ff5919u), 1.f);

    f32_t row_y = (bar_height - frame_height) * 0.5f + y_offset;
    f32_t text_y = (bar_height - gui::get_font_size()) * 0.5f + y_offset;

    // the tabs, as words: the one you are on is white and underlined
    gui::set_cursor_pos(fan::vec2(6.f, row_y));
    auto tab = [&](const char* label, int index) {
      bool active = window_handler.current_tab == index;
      fan::vec2 text = gui::calc_text_size(label);
      fan::vec2 at = gui::get_cursor_screen_pos();
      bool clicked = gui::invisible_button(label, fan::vec2(text.x + 24.f, frame_height));
      bool hovered = gui::is_item_hovered();
      std::uint32_t color = active ? 0xffffffffu : hovered ? 0xffd0d0d0u : 0xff6e6e6eu;
      list->AddText(at + fan::vec2(12.f, (frame_height - gui::get_font_size()) * 0.5f), faded(color), label);
      if (active) {
        list->AddRectFilled(at + fan::vec2(12.f, frame_height - 3.f), at + fan::vec2(text.x + 12.f, frame_height - 1.f), faded(accent_u32), 1.f);
      }
      gui::same_line(0, 2.f);
      if (clicked && !active) {
        window_handler.main_tab = index;
      }
    };
    tab("Channels", 0);
    tab("Stream View", 1);
    tab("Debug", 2);

    // what goes out and comes in is written after the tabs, once the right side is placed and the room is known
    gui::same_line(0, 22.f);
    fan::vec2 after_tabs = gui::get_cursor_pos();

    // at the right, from the edge in: the pin, your name, the version, then notes about updates and the name
    f32_t right = width - 10.f;
    f32_t pin_size = frame_height;
    right -= pin_size;
    gui::set_cursor_pos(fan::vec2(right, row_y));
    fan::vec2 pin_at = gui::get_cursor_screen_pos();
    bool pin_clicked = gui::invisible_button("##pin_interface", fan::vec2(pin_size, pin_size));
    bool pin_hovered = gui::is_item_hovered();
    if (pin_hovered) {
      gui::set_tooltip(overlay
        ? "Pin the bar"
        : "Unpin: hide the bar in Stream View");
      list->AddCircleFilled(pin_at + fan::vec2(pin_size, pin_size) * 0.5f, pin_size * 0.5f, faded(0xff1a1310u));
    }
    {
      // a push pin: round head, short body, needle. Blue while pinned
      fan::vec2 c = pin_at + fan::vec2(pin_size, pin_size) * 0.5f;
      f32_t u = pin_size * 0.11f;
      std::uint32_t color = faded(!overlay ? (pin_hovered ? accent_hover_u32 : accent_u32) : (pin_hovered ? 0xffffffffu : 0xff9a9a9au));
      list->AddCircleFilled(c + fan::vec2(u * 1.4f, -u * 1.4f), u * 1.5f, color);
      list->AddLine(c + fan::vec2(u * 1.4f, -u * 1.4f), c + fan::vec2(-u * 0.6f, u * 0.6f), color, u * 1.3f);
      list->AddLine(c + fan::vec2(-u * 1.6f, -u * 0.2f), c + fan::vec2(u * 0.2f, u * 1.6f), color, u * 0.9f);
      list->AddLine(c + fan::vec2(-u * 0.6f, u * 0.6f), c + fan::vec2(-u * 3.f, u * 3.f), color, u * 0.5f);
    }
    if (pin_clicked) {
      set_ui_autohide(!overlay);
    }

    std::string name = ecps_backend.get_current_username();
    right -= 14.f + gui::calc_text_size(name).x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.35f, 0.5f, 1.f, 1.f), name);
    if (gui::is_item_clicked()) {
      username_popup_requested = true;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Click to change your name");
    }

    std::string version = std::string("v") + ecps_version;
    right -= 16.f + gui::calc_text_size(version).x;
    gui::set_cursor_pos(fan::vec2(right, text_y));
    gui::text(fan::color(0.45f, 0.45f, 0.5f, 1.f), version);
    if (gui::is_item_clicked()) {
      about_popup_requested = true;
    }
    if (gui::is_item_clicked(1)) {
      gui::open_popup("##top_bar_menu");
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("About and updates");
    }

    // something about updates that needs a look, or just happened
    const char* hint = nullptr;
    fan::color hint_color(1.f, 1.f, 1.f, 1.f);
    switch (updater.state.load()) {
      case state_e::available: hint = "update available"; hint_color = fan::color(1.f, 0.7f, 0.15f, 1.f); break;
      case state_e::building: hint = "updating..."; hint_color = fan::color(0.3f, 0.6f, 1.f, 1.f); break;
      case state_e::ready: hint = "restarting..."; hint_color = fan::color(0.15f, 1.f, 0.3f, 1.f); break;
      case state_e::failed:
      case state_e::build_failed: hint = "update failed"; hint_color = fan::color(1.f, 0.25f, 0.2f, 1.f); break;
      default: break;
    }
    std::string update_notice = updater.update_notice();
    std::string notice = username_notice_text();
    for (const auto& [text, color, opens_about] : {
      std::tuple<std::string, fan::color, bool>{hint ? std::string(hint) : std::string(), hint_color, true},
      std::tuple<std::string, fan::color, bool>{update_notice, fan::color(0.15f, 1.f, 0.3f, 1.f), false},
      std::tuple<std::string, fan::color, bool>{notice, fan::color(1.f, 0.3f, 0.2f, 1.f), false},
    }) {
      if (text.empty()) {
        continue;
      }
      right -= 18.f + gui::calc_text_size(text).x;
      gui::set_cursor_pos(fan::vec2(right, text_y));
      gui::text(color, text);
      if (opens_about && gui::is_item_clicked()) {
        about_popup_requested = true;
      }
    }

    // the share button, left of all that
    f32_t pill_height = std::round(frame_height * 0.82f);
    f32_t pill_width = pill_button_width(stream_button_renderer_t::share_button_text(true));
    right -= 26.f + pill_width;
    gui::set_cursor_pos(fan::vec2(right, (bar_height - pill_height) * 0.5f + y_offset));
    stream_button_renderer_t::render_stream_toggle_button(selected_channel_id, fan::vec2(pill_width, pill_height), true);

    if (!top_bar_compact) {
      gui::set_cursor_pos(fan::vec2(after_tabs.x, text_y));
      render_share_chips(right - 20.f - after_tabs.x);
    }

    if (gui::begin_popup("##top_bar_menu")) {
      if (gui::selectable(top_bar_compact ? "Show what goes out and comes in" : "Leave out what goes out and comes in")) {
        top_bar_compact = !top_bar_compact;
        save_ui_config();
      }
      gui::end_popup();
    }
  }

  // version, which build it is, and the update controls
  void render_about_popup() {
    if (about_popup_requested) {
      about_popup_requested = false;
      gui::set_next_window_pos(fan::vec2(24.f, 70.f));
      gui::open_popup("About ECPS");
    }
    if (!gui::begin_popup("About ECPS")) {
      return;
    }
    using state_e = updater_t::state_e;
    gui::text(std::string("ECPS v") + ecps_version + (ecps_update_marker[0] ? std::string(" [") + ecps_update_marker + "]" : std::string()));
    std::string build = updater.build_short();
    text_wrapped_disabled(build.empty() ? updater.build_label() : build);
    gui::separator();

    switch (updater.state.load()) {
      case state_e::idle:
        if (gui::button("Check for updates")) {
          updater.check();
        }
        break;
      case state_e::checking:
        gui::text("Checking for updates...");
        break;
      case state_e::up_to_date:
        gui::text("Up to date");
        text_wrapped_disabled(updater.build_label());
        if (gui::button("Check again")) {
          updater.check();
        }
        break;
      case state_e::failed:
        gui::text_wrapped(updater.error());
        if (gui::button("Retry")) {
          updater.check();
        }
        break;
      case state_e::available:
        gui::text_wrapped("Update available: " + updater.latest());
        if (updater_t::can_install) {
          if (gui::button("Update now")) {
            updater.start_update();
          }
        }
        else {
          text_wrapped_disabled("Download it from github.com/6413/ecps_gui");
        }
        break;
      case state_e::building:
        gui::text("Building the update...");
        text_wrapped_disabled("This can take a while. Progress is in ecps_update.log");
        break;
      case state_e::ready:
        gui::text("Update " + updater.latest() + " is ready, restarting...");
        if (gui::button("Restart now")) {
          updater.restart();
        }
        break;
      case state_e::build_failed:
        gui::text_wrapped(updater.error());
        if (gui::button("Check again")) {
          updater.check();
        }
        break;
    }
    gui::end_popup();
  }

  static std::string number_text(double value, int decimals) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return std::string(text);
  }

  // a size that was never set holds garbage
  static std::string size_label(long long width, long long height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
      return "";
    }
    return std::to_string(width) + "x" + std::to_string(height);
  }

  // Top bar: share your screen (always into your own channel) and, while it happens, what goes out and what
  // comes in, both at once. They are separate things: you can send and watch in the same moment
  // what goes out and comes in, as long as it fits in room
  void render_share_chips(f32_t room) {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    bool first = true;

    bool sending = ecps_backend.is_streaming_to_any_channel();
    bool receiving = debug_sample.view_pps > 0.5f;
    if (sending) {
      std::string text = "Sending " + size_label(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) +
        "  " + number_text(debug_sample.share_fps, 0) + " fps  " + number_text(debug_sample.share_kbps / 1000.0, 1) + " Mbit/s";
      f32_t width = gui::calc_text_size(text).x + 16.f;
      if (width <= room) {
        gui::text(fan::color(0.3f, 0.6f, 1.f, 1.f), text);
        room -= width;
        first = false;
      }
    }
    if (receiving) {
      std::string text = "Receiving " + size_label(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y) +
        "  " + number_text(debug_sample.decoded_fps, 0) + " fps  " + number_text(debug_sample.view_kbps / 1000.0, 1) + " Mbit/s";
      if (gui::calc_text_size(text).x + 16.f <= room) {
        if (!first) {
          gui::same_line(0, 16.f);
        }
        gui::text(fan::color(0.2f, 0.85f, 0.4f, 1.f), text);
      }
    }
  }

  // Over the picture of the stream view, switched on with the Stats tick in the toolbar: what this computer sends
  // and what it receives. Both show when both are happening
  void render_stream_stats_overlay() {
    if (!show_stream_stats) {
      return;
    }
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    struct line_t {
      std::string text;
      fan::color color;
    };
    std::vector<line_t> lines;
    fan::color header(1.f, 1.f, 1.f, 1.f);
    fan::color normal(0.82f, 0.82f, 0.82f, 1.f);
    if (ecps_backend.is_streaming_to_any_channel()) {
      lines.push_back({"SENDING", fan::color(0.55f, 0.8f, 1.f, 1.f)});
      lines.push_back({"Encoded as  " + size_label(rt->screen_encoder.config_.width, rt->screen_encoder.config_.height) +
        " @ " + number_text(debug_sample.share_fps, 0) + " fps", normal});
      lines.push_back({"Bitrate  " + number_text(debug_sample.share_kbps / 1000.0, 1) + " Mbit/s", normal});
      lines.push_back({"Capture " + number_text(stage_times.capture.ms.load(), 1) + " ms, encode " +
        number_text(stage_times.encode.ms.load(), 1) + " ms", normal});
    }
    if (debug_sample.view_pps > 0.5f) {
      lines.push_back({"RECEIVING", fan::color(0.55f, 1.f, 0.7f, 1.f)});
      lines.push_back({"Picture  " + size_label(rt->screen_decoder.decoded_size.x, rt->screen_decoder.decoded_size.y) +
        " @ " + number_text(debug_sample.decoded_fps, 0) + " fps", normal});
      lines.push_back({"Bitrate  " + number_text(debug_sample.view_kbps / 1000.0, 1) + " Mbit/s", normal});
      lines.push_back({"Decode " + number_text(stage_times.decode.ms.load(), 1) + " ms, lost packets " +
        std::to_string((unsigned long long)(ecps_backend.view.m_stats.Packet_HeadDrop + ecps_backend.view.m_stats.Packet_BodyDrop)), normal});
    }
    if (lines.empty()) {
      lines.push_back({"Not sending, not receiving", normal});
    }

    f32_t width = 0;
    for (const auto& line : lines) {
      width = std::max(width, gui::calc_text_size(line.text).x);
    }
    f32_t line_height = gui::get_text_line_height_with_spacing();
    // The box can be dragged anywhere inside the stream area. Until it is moved it sits at the top left
    fan::vec2 box_size(width + 16.f, lines.size() * line_height + 10.f);
    fan::vec2 area_size = gui::get_window_size();
    fan::vec2 pos = stats_moved ? stats_pos : fan::vec2(6.f, 6.f + stream_top_offset());
    pos.x = std::max(0.f, std::min(pos.x, area_size.x - box_size.x));
    pos.y = std::max(0.f, std::min(pos.y, area_size.y - box_size.y));
    gui::set_cursor_pos(pos);
    gui::invisible_button("##stats_drag", box_size);
    if (gui::is_item_active()) {
      auto delta = gui::get_io().MouseDelta;
      pos.x = std::max(0.f, std::min(pos.x + delta.x, area_size.x - box_size.x));
      pos.y = std::max(0.f, std::min(pos.y + delta.y, area_size.y - box_size.y));
      stats_moved = true;
    }
    if (stats_moved) {
      stats_pos = pos;
    }
    if (gui::is_item_hovered()) {
      gui::set_tooltip("Drag to move");
    }
    gui::set_cursor_pos(pos);
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::get_window_draw_list()->AddRectFilled(at, at + box_size, 0xb4000000u, 6.f);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      gui::set_cursor_pos(pos + fan::vec2(8.f, 6.f + i * line_height));
      gui::text(lines[i].color, lines[i].text);
    }
  }

  void render_logs_tab() {
    gui::spacing();
    bool allow = remote_logs.allow.load();
    if (gui::checkbox("Let others in this channel read my logs", &allow)) {
      remote_logs.allow = allow;
    }
    text_wrapped_disabled("They see everything this program prints. It passes through the server.");
    gui::separator();

    gui::text("This computer");
    gui::same_line();
    if (remote_logs.local.load()) {
      if (gui::button("Stop##local_logs")) {
        remote_logs.stop_local();
      }
    }
    else if (gui::button("Read logs##local_logs")) {
      remote_logs.start_local([this](const std::function<fan::event::task_t()>& task) { backend_queue(task); });
    }

    bool in_channel = selected_channel_id.i != (uint16_t)-1;
    auto it = ecps_backend.channel_sessions.find(selected_channel_id.i);
    bool any_peer = false;
    if (in_channel && it != ecps_backend.channel_sessions.end()) {
      for (const auto& session : it->second) {
        if (session.session_id.i == ecps_backend.session_id.i) {
          continue;
        }
        any_peer = true;
        bool is_watched = remote_logs.watching.load() &&
          remote_logs.watch_peer.i == session.session_id.i && remote_logs.watch_channel.i == selected_channel_id.i;
        gui::push_id((int)session.session_id.i);
        gui::text(session.username + (session.is_host ? " (Host)" : "") + "  ID: " + std::to_string(session.session_id.i));
        gui::same_line();
        auto queue = [this](const std::function<fan::event::task_t()>& task) { backend_queue(task); };
        if (is_watched) {
          if (gui::button("Stop")) {
            remote_logs.stop_watch(queue);
          }
        }
        else if (gui::button("Read logs")) {
          remote_logs.start_watch(queue, selected_channel_id, session.session_id);
        }
        gui::pop_id();
      }
    }
    if (!any_peer) {
      text_wrapped_disabled("Nobody else in this channel");
    }

    const char* status = "";
    switch (remote_logs.state.load()) {
      case remote_logs_t::state_e::asking: status = "Asking for access..."; break;
      case remote_logs_t::state_e::live: status = "Live"; break;
      case remote_logs_t::state_e::denied: status = "Denied: that user has not allowed log sharing"; break;
      case remote_logs_t::state_e::stopped: status = "Stopped by the other side"; break;
      default: break;
    }
    if (*status) {
      gui::text_wrapped(status);
    }

    gui::separator();
    gui::checkbox("stdout", &logs_show_out);
    gui::same_line();
    gui::checkbox("stderr", &logs_show_err);
    gui::same_line();
    gui::checkbox("Follow", &logs_follow);
    if (gui::button("Copy all")) {
      engine.window.set_clipboard(remote_logs.all_text(logs_show_out, logs_show_err));
    }
    gui::same_line();
    if (gui::button("Clear")) {
      remote_logs.clear_lines();
    }
    text_wrapped_disabled("Click a line to copy it");

    // the whole history stays in memory, only the rows on screen are drawn
    std::uint64_t revision = remote_logs.current_revision();
    if (gui::begin_child("##remote_log", fan::vec2(0, 0), true, gui::window_flags_horizontal_scrollbar)) {
      if (gui::is_window_hovered() && gui::get_io().MouseWheel > 0.f) {
        logs_follow = false; // scrolling up means reading, don't pull it back down
      }
      f32_t pitch = gui::get_text_line_height_with_spacing();
      f32_t gap = pitch - gui::get_text_line_height();
      f32_t view_height = gui::get_window_size().y;
      f32_t scroll = gui::get_scroll_y();
      remote_logs.visit(logs_show_out, logs_show_err, [&](std::size_t count, auto line_at) {
        std::size_t first = std::min<std::size_t>(count, (std::size_t)std::max(0.f, scroll / pitch));
        std::size_t last = std::min<std::size_t>(count, first + (std::size_t)(view_height / pitch) + 3);
        if (first > 0) {
          gui::dummy(fan::vec2(0, first * pitch - gap));
        }
        for (std::size_t i = first; i < last; ++i) {
          const auto& line = line_at(i);
          bool is_err = line.stream == 2;
          if (is_err) {
            gui::push_style_color(gui::col_text, fan::vec4(1.f, 0.45f, 0.4f, 1.f));
          }
          gui::push_id((int)i);
          if (gui::selectable(line.text.empty() ? " " : line.text)) {
            engine.window.set_clipboard(line.text);
          }
          gui::pop_id();
          if (is_err) {
            gui::pop_style_color();
          }
        }
        if (last < count) {
          gui::dummy(fan::vec2(0, (count - last) * pitch - gap));
        }
        if (logs_follow && revision != logs_seen_revision) {
          gui::set_scroll_y(count * pitch);
        }
      });
      logs_seen_revision = revision;
    }
    gui::end_child();
  }

  // any channel we are in but don't host
  bool is_in_other_channel() const {
    return std::any_of(ecps_backend.channel_info.begin(), ecps_backend.channel_info.end(), [](const auto& channel) {
      return !ecps_backend.is_hosting_channel(channel.channel_id);
    });
  }

  // a host we are connected to lets MIDI in, no stream needed
  bool can_midi_host() const {
    return std::any_of(ecps_backend.channel_info.begin(), ecps_backend.channel_info.end(), [](const auto& channel) {
      return !ecps_backend.is_hosting_channel(channel.channel_id) &&
        (channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::MidiControl);
    });
  }

  static void section_title(const char* title) {
    gui::spacing();
    gui::text(fan::color(0.6f, 0.8f, 1.f, 1.f), title);
    gui::separator();
  }

  void render_midi_tab() {
    // the keyboards are listed and opened on a worker thread, a busy driver must not stop the window
    auto now = std::chrono::steady_clock::now();
    if (now >= midi_scan_at) {
      midi_scan_at = now + std::chrono::seconds(3);
      midi_io.request_scan();
    }

    section_title("This computer");
    auto devices = midi_io.devices();
    if (!midi_io.has_scanned()) {
      text_wrapped_disabled("Looking for MIDI keyboards...");
    }
    else if (devices.empty()) {
      text_wrapped_disabled("No MIDI keyboard found on this computer");
    }
    for (const auto& name : devices) {
      gui::text_wrapped(name);
    }
    if (midi_to_host.load() || midi_to_viewers.load()) {
      text_wrapped_disabled(midi_io.is_open() ? "Keyboard connected" : midi_io.is_busy() ? "Connecting to the keyboard..." : "Keyboard not connected");
    }
    else {
      text_wrapped_disabled("The keyboard is opened when you send");
    }

    section_title("Hear MIDI on this computer");
    gui::checkbox("Play MIDI from the host", &play_host_midi);
    if (gui::checkbox("Let viewers play MIDI here", &allow_midi)) {
      sent_channel_flags.clear();
    }
    text_wrapped_disabled("Other users' keyboards play through this computer's speakers. No stream needed.");
    if (gui::slider("Volume", &midi_volume, 0.f, 2.f, gui::slider_flags_always_clamp)) {
      midi_host.player.set_volume(midi_volume);
    }

    section_title("Send my keyboard");
    gui::checkbox("To the host", &send_midi);
    if (send_midi) {
      if (!is_in_other_channel()) {
        text_wrapped_disabled("Join another user's channel first");
      }
      else if (!can_midi_host()) {
        text_wrapped_disabled("Waiting for the host to tick \"Let viewers play MIDI here\"");
      }
      else {
        text_wrapped_disabled("Sending to the host");
      }
    }
    gui::checkbox("To everyone in my channel", &send_midi_viewers);
    if (send_midi_viewers) {
      text_wrapped_disabled(ecps_backend.is_hosting_any_channel()
        ? "Sending to the viewers in your channel"
        : "You don't have a channel of your own yet");
    }

    section_title("Piano");
    gui::checkbox("Show piano", &show_midi_piano);
    if (show_midi_piano) {
      render_midi_piano();
    }
  }

  // the host's keyboard goes to everyone in the channels it hosts, streaming or not
  void send_viewers_midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMidi_t rest;
    rest.Status = status;
    rest.Data1 = data1;
    rest.Data2 = data2;
    backend_queue([rest]() -> fan::event::task_t {
      try {
        for (const auto& channel : ecps_backend.channel_info) {
          if (ecps_backend.is_hosting_channel(channel.channel_id)) {
            auto packet = rest;
            packet.ChannelID = channel.channel_id;
            packet.ChannelSessionID = channel.session_id;
            co_await ecps_backend.tcp_write(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMidi, &packet, sizeof(packet));
          }
        }
      }
      catch (...) {}
    });
  }

  // one keyboard feeds both directions: to the host while it allows MIDI, to the viewers of our channel.
  // when a direction ends nothing may keep ringing on the other side
  void update_midi_forward() {
    bool to_host = send_midi && can_midi_host();
    bool to_viewers = send_midi_viewers && ecps_backend.is_hosting_any_channel();
    bool was_to_host = midi_to_host.exchange(to_host);
    bool was_to_viewers = midi_to_viewers.exchange(to_viewers);
    if (!midi_io_started) {
      midi_io_started = true;
      midi_io.start([this](std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
        track_local_note(status, data1, data2);
        if (midi_to_host.load()) {
          send_host_midi(status, data1, data2);
        }
        if (midi_to_viewers.load()) {
          send_viewers_midi(status, data1, data2);
        }
      });
    }
    bool want = to_host || to_viewers;
    midi_io.set_wanted(want);
    if (!want) {
      for (auto& key : local_down) {
        key = false;
      }
    }
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
      if (was_to_host && !to_host) {
        send_host_midi(0xB0 | channel, 64, 0);
        send_host_midi(0xB0 | channel, 123, 0);
      }
      if (was_to_viewers && !to_viewers) {
        send_viewers_midi(0xB0 | channel, 64, 0);
        send_viewers_midi(0xB0 | channel, 123, 0);
      }
    }
  }

  void send_host_key(int key, bool state) {
    ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostKeyboard_t rest;
    rest.Scancode = fan::window::input::convert_fan_to_scancode(key);
    rest.State = state;
    queue_host_input(ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostKeyboard, rest);
  }

  // the host's mouse and keyboard can only be driven while the host's own screen is the one watched
  bool watching_host_of(ecps_backend_t::Protocol_ChannelID_t channel_id) const {
    if (!watch_active || watch_channel != (std::uint32_t)channel_id.i) {
      return true;
    }
    for (const auto& channel : ecps_backend.available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        return (std::uint32_t)channel.host_session_id.i == watch_source;
      }
    }
    return false;
  }

  bool can_control_host() {
    return std::any_of(ecps_backend.channel_info.begin(), ecps_backend.channel_info.end(), [this](const auto& channel) {
      return channel.is_viewing && watching_host_of(channel.channel_id) &&
        (channel.flag & ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::InputControl);
    });
  }

  // nothing the viewer holds down may stay down on the host
  void release_host_input() {
    for (int key : input_forward.held_keys) {
      send_host_key(key, false);
    }
    for (std::uint8_t button : input_forward.held_buttons) {
      send_host_button(button, false, fan::vec2si(-1));
    }
    input_forward = {};
  }

  // clicking the stream gives it the mouse and keyboard, clicking elsewhere takes them back
  void forward_input() {
    auto rt = get_render_thread();
    bool stream_shown = is_fullscreen_stream || window_handler.current_tab == 1;
    if (!rt || !stream_shown || !can_control_host() || rt->network_frame.get_image() == engine.default_texture) {
      release_host_input();
      return;
    }

    fan::vec2 mouse_position = engine.get_mouse_position();
    fan::vec2 frame_size = rt->network_frame.get_size() * 2;
    fan::vec2 frame_min = fan::vec2(rt->network_frame.get_position()) - frame_size / 2;
    fan::vec2 frame_max = frame_min + frame_size;
    fan::vec2ui stream_size = rt->screen_decoder.decoded_size;

    bool over_frame = frame_size.x > 0 && frame_size.y > 0 && stream_size.x > 0 && stream_size.y > 0 &&
      mouse_position.x >= frame_min.x && mouse_position.x <= frame_max.x &&
      mouse_position.y >= frame_min.y && mouse_position.y <= frame_max.y &&
      !gui::is_any_item_hovered();

    fan::vec2si position(-1);
    if (over_frame) {
      position.x = std::min((int)((mouse_position.x - frame_min.x) / frame_size.x * stream_size.x), (int)stream_size.x - 1);
      position.y = std::min((int)((mouse_position.y - frame_min.y) / frame_size.y * stream_size.y), (int)stream_size.y - 1);
    }

    // the order the host's mouse api numbers them
    static constexpr int buttons[] = {fan::mouse_left, fan::mouse_right, fan::mouse_middle, fan::mouse_scroll_up, fan::mouse_scroll_down};
    static constexpr std::uint8_t first_scroll_button = 3;

    for (std::uint8_t button = 0; button < first_scroll_button; ++button) {
      auto& held = input_forward.held_buttons;
      auto it = std::find(held.begin(), held.end(), button);
      if (it != held.end() && engine.is_mouse_released(buttons[button])) {
        held.erase(it);
        send_host_button(button, false, position);
      }
      else if (engine.is_mouse_clicked(buttons[button])) {
        if (!over_frame) {
          release_host_input();
          return;
        }
        input_forward.captured = true;
        held.push_back(button);
        send_host_button(button, true, position);
      }
    }

    if (!input_forward.captured) {
      return;
    }

    if (over_frame) {
      if (position != input_forward.last_position) {
        input_forward.last_position = position;
        send_host_position(position);
      }
      for (std::uint8_t button = first_scroll_button; button < std::size(buttons); ++button) {
        if (engine.is_mouse_clicked(buttons[button])) {
          send_host_button(button, true, position);
        }
      }
    }

    auto& held_keys = input_forward.held_keys;
    for (auto it = held_keys.begin(); it != held_keys.end();) {
      if (engine.is_key_released(*it)) {
        send_host_key(*it, false);
        it = held_keys.erase(it);
      }
      else {
        ++it;
      }
    }

    // typing into our own text fields stays here
    if (!gui::get_io().WantTextInput) {
      for (int key = fan::key_first; key <= fan::key_last; ++key) {
        if (engine.is_key_clicked(key) && fan::window::input::convert_fan_to_scancode(key) != fan::input::key_invalid) {
          held_keys.push_back(key);
          send_host_key(key, true);
        }
      }
    }
  }

  void update_input_control() {
    // every stream starts with control off, the host opts in each time
    if (!ecps_backend.is_streaming_to_any_channel() && stream_settings.input_control != 0) {
      stream_settings.input_control = 0;
      send_input_control_flag();
    }
    forward_input();
    update_channel_flags();
    update_host_cursor();
    update_username();
    update_stream_settings_persistence();
    update_selected_channel();
    update_stream_state();
    update_watch();
    update_auto_show();
    update_debug_sample();
    update_auto_join();
    update_midi_forward();
    remote_logs.tick([this](const std::function<fan::event::task_t()>& task) {
      backend_queue(task);
    });
  }

  void render() {
    audio_share.update(stream_settings.share_audio, stream_settings.play_audio, stream_settings.audio_volume, get_capture_options());
    if (is_fullscreen_stream) {
      render_fullscreen_stream();
      update_input_control();
      return;
    }

    drop_down_server.render();
    render_file_offers();
    render_file_drop();
    render_username_popup();

    gui::set_next_window_pos(0);
    gui::set_next_window_size(gui::get_io().DisplaySize);
    gui::begin("A", 0, gui::window_flags_no_docking |
      gui::window_flags_no_saved_settings |
      gui::window_flags_no_focus_on_appearing |
      gui::window_flags_no_move |
      gui::window_flags_no_collapse |
      gui::window_flags_no_background |
      gui::window_flags_no_resize |
      gui::window_flags_no_title_bar |
      gui::window_flags_no_bring_to_front_on_focus |
      gui::window_flags_no_inputs);

    if (window_handler.p_open) {
      window_handler.render();
    }

    render_stream();

    gui::end();

    update_input_control();
  }

  inline static const char* config_path = "config.json";
  void write_to_config(const std::string& key, auto value) {
    config[key] = value;
    fan::io::file::write(config_path, config.dump(2), std::ios_base::binary);
  }

  bool is_fullscreen_stream = false;

  struct input_forward_t {
    bool captured = false;
    fan::vec2si last_position {-1, -1};
    std::vector<int> held_keys;
    std::vector<std::uint8_t> held_buttons;
  } input_forward;

  fan::json config;

  bool show_own_stream = false;
  ecps_backend_t::Protocol_ChannelID_t selected_channel_id{ (uint16_t)-1 };
  ecps_backend_t::Protocol_ChannelID_t input_control_channel{ (uint16_t)-1 };
  bool send_midi = false;
  bool send_midi_viewers = false;
  bool play_host_midi = true;
  bool show_midi_piano = true;
  bool logs_show_out = true;
  bool logs_show_err = true;
  bool logs_follow = true;
  std::uint64_t logs_seen_revision = 0;
  f32_t midi_volume = 1.f;
  std::array<std::atomic<bool>, 128> local_down{};
  std::atomic<bool> midi_to_host{false};
  std::atomic<bool> midi_to_viewers{false};
  midi_worker_t midi_io;
  bool midi_io_started = false;
  std::chrono::steady_clock::time_point midi_scan_at;
  // the host lets viewers play MIDI on this computer, off until switched on
  bool allow_midi = false;
  std::unordered_map<std::uint32_t, std::uint8_t> sent_channel_flags;
  std::chrono::steady_clock::time_point flags_sent_at;
  bool show_stream_stats = false;
  bool stats_moved = false;
  fan::vec2 stats_pos = fan::vec2(6.f, 6.f);
  f32_t top_overlay_alpha = 1.f;
  std::chrono::steady_clock::time_point top_overlay_visible_until;
  bool top_bar_compact = false;
  bool about_popup_requested = false;
  f32_t toolbar_alpha = 1.f;
  std::chrono::steady_clock::time_point toolbar_visible_until;
  f32_t fullscreen_toolbar_alpha = 1.f;
  std::chrono::steady_clock::time_point fullscreen_toolbar_visible_until;

#undef This
#undef engine
};
// Part of ecps_gui_t, included inside the struct by gui.h: the share button, drawn icons, icon and pill buttons, the splitter, the stream picture placement.

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

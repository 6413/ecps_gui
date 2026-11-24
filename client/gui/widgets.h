// Part of ecps_gui_t, included inside the struct by gui.h: the share button, drawn icons, icon and pill buttons, the splitter, the stream picture placement.

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
      fan::vec2 button_size(gui::pill_button_width(share_button_text(false)), gui::get_frame_height());

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
  enum class icon_e { gear, fullscreen, exit_fullscreen, plus, server, leave, join, refresh, send_file, eye, monitor, stop, chat, copy, download, users, close, reply, pin, edit, trash, speaker, speaker_muted, mic, mic_muted, chip, palette, lock, camera, phone };

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
      case icon_e::copy:
        // two sheets: the front one whole, the back one's top and right edges
        list->AddRect(P(-0.8f, -0.4f) - fan::vec2(grow, grow), P(0.3f, 0.9f) + fan::vec2(grow, grow), color, h * 0.18f, 0, t);
        line(P(-0.35f, -0.85f), P(0.75f, -0.85f));
        line(P(0.75f, -0.85f), P(0.75f, 0.45f));
        break;
      case icon_e::chat:
        // a speech bubble with its tail down-left and three dots
        list->AddRect(P(-0.85f, -0.72f) - fan::vec2(grow, grow), P(0.85f, 0.42f) + fan::vec2(grow, grow), color, h * 0.32f, 0, t);
        line(P(-0.42f, 0.42f), P(-0.62f, 0.86f));
        line(P(-0.62f, 0.86f), P(-0.02f, 0.42f));
        dot(P(-0.4f, -0.15f), h * 0.1f);
        dot(P(0.f, -0.15f), h * 0.1f);
        dot(P(0.4f, -0.15f), h * 0.1f);
        break;
      case icon_e::reply:
        // an arrow turning back: left along the top, then down and round to the right
        line(P(-0.62f, -0.2f), P(0.15f, -0.2f));
        list->AddBezierQuadratic(P(0.15f, -0.2f), P(0.82f, -0.2f), P(0.82f, 0.45f), color, t + grow * 2.f, 12);
        arrow_head(P(-0.7f, -0.2f), fan::vec2(-1, 0));
        break;
      case icon_e::pin:
        // a push pin: round head, short body, needle
        dot(P(0.22f, -0.38f), h * 0.36f);
        line(P(0.05f, -0.1f), P(-0.45f, 0.4f), t * 1.5f);
        line(P(-0.45f, 0.4f), P(-0.8f, 0.8f), t * 0.7f);
        break;
      case icon_e::edit:
        // a pencil on the diagonal, with its tip at the lower left
        line(P(-0.3f, 0.3f), P(0.62f, -0.62f), t * 2.2f);
        list->AddTriangleFilled(P(-0.82f, 0.82f), P(-0.58f, 0.22f), P(-0.22f, 0.58f), color);
        break;
      case icon_e::trash:
        // lid with a handle, and a bin under it
        line(P(-0.72f, -0.48f), P(0.72f, -0.48f));
        line(P(-0.24f, -0.48f), P(-0.24f, -0.74f));
        line(P(-0.24f, -0.74f), P(0.24f, -0.74f));
        line(P(0.24f, -0.74f), P(0.24f, -0.48f));
        list->AddRect(P(-0.5f, -0.3f) - fan::vec2(grow, grow), P(0.5f, 0.82f) + fan::vec2(grow, grow), color, h * 0.12f, 0, t);
        line(P(-0.16f, -0.02f), P(-0.16f, 0.6f));
        line(P(0.16f, -0.02f), P(0.16f, 0.6f));
        break;
      case icon_e::close:
        // a cross of two lines, centred in its box and drawn as shapes, so it stays sharp at any size
        line(P(-0.5f, -0.5f), P(0.5f, 0.5f));
        line(P(0.5f, -0.5f), P(-0.5f, 0.5f));
        break;
      case icon_e::users:
        // a head over rounded shoulders
        dot(P(0, -0.42f), h * 0.3f);
        list->PathLineTo(P(-0.78f, 0.82f));
        list->PathBezierQuadraticCurveTo(P(-0.78f, 0.1f), P(0, 0.1f));
        list->PathBezierQuadraticCurveTo(P(0.78f, 0.1f), P(0.78f, 0.82f));
        list->PathStroke(color, 0, t);
        break;
      case icon_e::download: {
        // a thick arrow down into an open tray: an update to get
        fan::vec2 g(grow, grow);
        list->AddRectFilled(P(-0.19f, -0.95f) - g, P(0.19f, -0.08f) + g, color, h * 0.06f);
        list->AddTriangleFilled(P(-0.56f - grow / h, -0.16f - grow / h), P(0.56f + grow / h, -0.16f - grow / h),
          P(0, 0.42f + grow / h), color);
        list->PathLineTo(P(-0.86f, 0.12f));
        list->PathLineTo(P(-0.86f, 0.86f));
        list->PathLineTo(P(0.86f, 0.86f));
        list->PathLineTo(P(0.86f, 0.12f));
        list->PathStroke(color, 0, t * 1.25f);
        break;
      }
      case icon_e::speaker:
      case icon_e::speaker_muted: {
        // a speaker box with its cone opening to the right, and sound leaving it
        list->PathLineTo(P(-0.85f, -0.22f));
        list->PathLineTo(P(-0.45f, -0.22f));
        list->PathLineTo(P(0.02f, -0.62f));
        list->PathLineTo(P(0.02f, 0.62f));
        list->PathLineTo(P(-0.45f, 0.22f));
        list->PathLineTo(P(-0.85f, 0.22f));
        list->PathStroke(color, 1, t);
        if (icon == icon_e::speaker_muted) {
          line(P(0.3f, -0.32f), P(0.82f, 0.32f));
          line(P(0.3f, 0.32f), P(0.82f, -0.32f));
        }
        else {
          list->PathArcTo(P(0.12f, 0), h * 0.36f, -0.9f, 0.9f, 12);
          list->PathStroke(color, 0, t);
          list->PathArcTo(P(0.12f, 0), h * 0.66f, -0.75f, 0.75f, 16);
          list->PathStroke(color, 0, t);
        }
        break;
      }
      case icon_e::mic:
      case icon_e::mic_muted: {
        // a capsule head, a holder around its bottom and a small stand
        list->AddRect(P(-0.28f, -0.85f), P(0.28f, -0.05f), color, h * 0.28f, 0, t);
        list->PathArcTo(P(0, 0.05f), h * 0.5f, 0.f, 3.14159265f, 16);
        list->PathStroke(color, 0, t);
        line(P(0, 0.55f), P(0, 0.82f));
        line(P(-0.32f, 0.82f), P(0.32f, 0.82f));
        if (icon == icon_e::mic_muted) {
          line(P(-0.62f, -0.62f), P(0.62f, 0.62f));
        }
        break;
      }
      case icon_e::chip: {
        list->AddRect(P(-0.5f, -0.5f) - fan::vec2(grow, grow), P(0.5f, 0.5f) + fan::vec2(grow, grow), color, h * 0.12f, 0, t);
        list->AddRect(P(-0.18f, -0.18f), P(0.18f, 0.18f), color, h * 0.05f, 0, t * 0.8f);
        for (int i = -1; i <= 1; ++i) {
          f32_t p = i * 0.33f;
          line(P(p, -0.82f), P(p, -0.5f));
          line(P(p, 0.5f), P(p, 0.82f));
          line(P(-0.82f, p), P(-0.5f, p));
          line(P(0.5f, p), P(0.82f, p));
        }
        break;
      }
      case icon_e::palette:
        list->AddCircle(c, h * 0.68f, color, 20, t);
        dot(P(-0.3f, -0.26f), h * 0.13f);
        dot(P(0.04f, -0.4f), h * 0.13f);
        dot(P(0.34f, -0.08f), h * 0.13f);
        dot(P(-0.06f, 0.34f), h * 0.13f);
        break;
      case icon_e::lock: {
        list->AddRect(P(-0.6f, -0.05f), P(0.6f, 0.85f), color, h * 0.12f, 0, t);
        list->PathArcTo(P(0, -0.05f), h * 0.42f, 3.14159265f, 2.f * 3.14159265f, 16);
        list->PathStroke(color, 0, t);
        dot(P(0, 0.36f), h * 0.1f);
        line(P(0, 0.36f), P(0, 0.52f), t * 0.8f);
        break;
      }
      case icon_e::camera: {
        // a camcorder body and the cone of its lens
        list->AddRect(P(-0.85f, -0.4f), P(0.25f, 0.4f), color, h * 0.16f, 0, t);
        list->AddTriangleFilled(P(0.25f, -0.35f), P(0.85f, 0), P(0.25f, 0.35f), color);
        dot(P(-0.55f, 0), h * 0.1f);
        break;
      }
      case icon_e::phone: {
        // a phone: a rounded screen and its speaker line
        list->AddRect(P(-0.36f, -0.85f), P(0.36f, 0.85f), color, h * 0.18f, 0, t);
        line(P(-0.15f, 0.6f), P(0.15f, 0.6f), t * 0.8f);
        dot(P(0, -0.62f), h * 0.06f);
        break;
      }
    }
  }

  // with a dark edge, for icons over the picture, which can be anything from black to white
  static void draw_icon_outlined(gui::draw_list_t* list, icon_e icon, fan::vec2 c, f32_t size, std::uint32_t color) {
    draw_icon(list, icon, c, size, gui::with_style_alpha(0xc8000000u), 1.6f);
    draw_icon(list, icon, c, size, color);
  }

  enum class icon_style_e { plain, danger, accent, overlay };

  static gui::icon_button_style_t icon_button_style(icon_style_e style) {
    switch (style) {
      case icon_style_e::danger: return gui::icon_button_style_t::danger;
      case icon_style_e::accent: return gui::icon_button_style_t::accent;
      case icon_style_e::overlay: return gui::icon_button_style_t::overlay;
      default: return gui::icon_button_style_t::plain;
    }
  }

  // a square button with an icon and a tooltip; the icon paths stay here, the button itself is in fan
  static bool icon_button(const char* id, icon_e icon, std::string_view tooltip, f32_t size = 0.f, icon_style_e style = icon_style_e::plain) {
    auto style_fan = icon_button_style(style);
    return gui::icon_button(id, tooltip, size, style_fan, [=](gui::draw_list_t* list, fan::vec2 c, f32_t s,
      std::uint32_t color, bool outlined) {
      if (outlined) {
        draw_icon_outlined(list, icon, c, s, color);
      }
      else {
        draw_icon(list, icon, c, s, color);
      }
    });
  }

  static bool pill_button(const char* id, icon_e icon, std::string_view text, std::uint32_t color, std::uint32_t hover_color,
    bool enabled = true, f32_t width = 0.f, f32_t height = 0.f) {
    return gui::pill_button(id, text, color, hover_color, enabled, width, height,
      [=](gui::draw_list_t* list, fan::vec2 c, f32_t s, std::uint32_t ink, bool) {
        draw_icon(list, icon, c, s, ink);
      });
  }

  struct network_frame_helper_t {
    #undef This
    #define This OFFSETLESS(this, ecps_gui_t, network_frame_helper) 
    // Zoom: Ctrl+wheel over the stream zooms at the mouse (in full screen the wheel alone does), dragging with the
    // middle button (or Ctrl+left) moves the zoomed picture, Ctrl+middle click (in full screen the middle button)
    // goes back to the whole picture
    f32_t zoom = 1.f;
    fan::vec2 offset = 0;

    // the picture's center (in the stream area) and half size as last placed
    fan::vec2 shown_position = 0;
    fan::vec2 shown_half = 0;

    void setup_network_frame(fan::vec2 available_area, bool fullscreen = false) {
      auto rt = get_render_thread();
      if (!rt) return;

      fan::vec2 center = available_area / 2;
      fan::vec2 fitted_size = gui::fit_size(
        available_area, rt->screen_decoder.decoded_size);

      // called inside the stream's window: its top left is the stream's
      auto& io = gui::get_io();
      fan::vec2 origin = gui::get_window_pos();
      fan::vec2 mouse = fan::vec2(io.MousePos) - origin;
      bool over = gui::is_window_hovered() && mouse.x >= 0 && mouse.y >= 0 && mouse.x <= available_area.x && mouse.y <= available_area.y;
      bool ctrl = engine.window.is_key_down(fan::key_left_control) || engine.window.is_key_down(fan::key_right_control);
      // in full screen there is nothing else for the wheel to do: it zooms without Ctrl
      bool zoom_mod = ctrl || fullscreen;
      if (over && zoom_mod && io.MouseWheel != 0.f) {
        f32_t old_zoom = zoom;
        zoom = std::clamp(zoom * std::pow(1.15f, io.MouseWheel), 1.f, 10.f);
        fan::vec2 at = center + offset;
        at = mouse + (at - mouse) * (zoom / old_zoom);
        offset = at - center;
      }
      if (over && zoom > 1.f && (gui::is_mouse_dragging(2, 2.f) || (zoom_mod && gui::is_mouse_dragging(0, 2.f)))) {
        offset += fan::vec2(io.MouseDelta);
      }
      if (over && zoom_mod && fan::window::is_mouse_clicked(2)) {
        zoom = 1.f;
      }
      if (zoom <= 1.f) {
        offset = 0;
      }
      // the picture keeps covering the area it can
      fan::vec2 half = fitted_size * zoom;
      fan::vec2 limit(std::max(0.f, half.x - available_area.x * 0.5f), std::max(0.f, half.y - available_area.y * 0.5f));
      offset = fan::vec2(std::clamp(offset.x, -limit.x, limit.x), std::clamp(offset.y, -limit.y, limit.y));

      // Close to its own size (an area up to 12% bigger than the stream): pixel for pixel instead,
      // on whole pixels. Stretched a few percent, or on half a pixel, every letter of a text on it went soft
      fan::vec2 position = center + offset;
      fan::vec2 native = fan::vec2(rt->screen_decoder.decoded_size) * 0.5f;
      if (zoom <= 1.f && native.x > 0.f && native.y > 0.f) {
        f32_t scale = half.x / native.x;
        if (scale >= 0.93f && scale <= 1.12f && native.x * 2.f <= available_area.x + 1.f && native.y * 2.f <= available_area.y + 1.f) {
          half = native;
        }
      }
      fan::vec2 corner = position - half;
      position = fan::vec2(std::round(corner.x), std::round(corner.y)) + half;
      rt->network_frame.set_position(position);
      // where the picture is, for the pointer over it and remote control (render_stream_area)
      shown_position = position;
      shown_half = half;
      if (rt->network_frame.get_image() != engine.default_texture) {
        rt->network_frame.set_size(half);
      }
      else {
        rt->network_frame.set_size(fan::vec2(0, 0));
      }
    }
  }network_frame_helper;
  #undef This
  #define This this

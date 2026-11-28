// Part of ecps_gui_t, included inside the struct by gui.h: drawing on the stream, and the lines on the sharer's screen.

  // ---- the pen ---------------------------------------------------------------------------------------------------
  // A viewer draws on the screen it watches: the lines go to everyone watching that screen and to the one sharing
  // it, and fade a few seconds after they were drawn. Only with a server that answered the Hello (an older one
  // ignores it and no pen shows)

  // RGBA, red in the lowest byte: red, yellow, green, blue, white, black
  static constexpr std::uint32_t pen_colors[] = {0xff4040ffu, 0xff30d8ffu, 0xff50d040u, 0xffff9040u, 0xffffffffu, 0xff000000u};
  static constexpr std::uint8_t pen_width = 5; // pixels on a screen 1080 high
  bool pen_on = false;
  int pen_color = 0;
  // the lines show on the shared screen itself too, in a window over everything that is left out of the share
  // (Windows). Saved, off at first
  bool draw_overlay = false;

  struct pen_line_t {
    bool active = false;
    bool sent_any = false;
    std::uint32_t id = 0;
    std::uint32_t channel = 0;
    std::uint32_t source = 0;
    std::uint32_t color = 0;
    // drawn but not sent yet; after the first piece it starts with the last point sent, so the pieces join up
    std::vector<fan::vec2> pending;
    std::chrono::steady_clock::time_point sent_at;
  } pen_line;
  std::uint32_t pen_next_id = 1;

  // the screen this client draws on: the one it watches in the channel it views, the host's by default
  bool pen_target(ecps_backend_t::Protocol_ChannelID_t& channel_id, std::uint32_t& source) {
    for (const auto& channel : ecps_backend.channel_info) {
      if (!channel.is_viewing || !ecps_backend.draw_known(channel.channel_id)) {
        continue;
      }
      channel_id = channel.channel_id;
      if (watch_active && watch_channel == (std::uint32_t)channel.channel_id.i) {
        source = watch_source;
        return true;
      }
      for (const auto& listed : ecps_backend.available_channels) {
        if (listed.channel_id.i == channel.channel_id.i) {
          source = (std::uint32_t)listed.host_session_id.i;
          return true;
        }
      }
    }
    return false;
  }

  // the points drawn since the last piece go out, and into this client's own lines
  void pen_send(bool end) {
    auto& line = pen_line;
    if (line.pending.empty() || (line.sent_any && line.pending.size() < 2)) {
      return;
    }
    std::vector<fan::vec2> points = line.pending;
    ecps_backend.on_draw_piece((std::uint32_t)ecps_backend.session_id.i, line.source, line.id, line.color, pen_width, 0, points);
    std::uint8_t flags = end ? ecps_backend_t::Protocol::DrawFlag_End : 0;
    backend_queue([channel = line.channel, source = line.source, id = line.id, color = line.color, flags, points]() -> fan::event::task_t {
      co_await ecps_backend.send_draw(ecps_backend_t::Protocol_ChannelID_t{ (uint16_t)channel }, source, id, color, pen_width, flags, points);
    });
    fan::vec2 last = line.pending.back();
    line.pending.clear();
    line.pending.push_back(last);
    line.sent_any = true;
    line.sent_at = std::chrono::steady_clock::now();
  }

  void pen_stop() {
    if (pen_line.active) {
      pen_send(true);
      pen_line = {};
    }
  }

  void pen_clear(ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t source) {
    pen_line = {};
    ecps_backend.on_draw_piece((std::uint32_t)ecps_backend.session_id.i, source, 0, 0, 0, ecps_backend_t::Protocol::DrawFlag_Clear, {});
    backend_queue([channel = (std::uint32_t)channel_id.i, source]() -> fan::event::task_t {
      co_await ecps_backend.send_draw(ecps_backend_t::Protocol_ChannelID_t{ (uint16_t)channel }, source, 0, 0, 0,
        ecps_backend_t::Protocol::DrawFlag_Clear, {});
    });
  }

  // the lines on Source's screen over a picture drawn at min, size big
  void draw_strokes(gui::draw_list_t* list, fan::vec2 min, fan::vec2 size, std::uint32_t source) {
    if (size.x <= 0 || size.y <= 0) {
      return;
    }
    auto strokes = ecps_backend.draw_strokes_of(source);
    bool live = pen_line.active && pen_line.source == source;
    if (strokes.empty() && !live) {
      return;
    }
    std::int64_t now = ecps_backend_t::draw_now_ms();
    f32_t scale = size.y / 1080.f;
    auto at = [&](fan::vec2 p) { return fan::vec2(min.x + p.x * size.x, min.y + p.y * size.y); };
    auto put = [&](const std::vector<fan::vec2>& points, std::uint32_t color, f32_t width, f32_t alpha) {
      if (points.empty() || alpha <= 0.f) {
        return;
      }
      std::uint32_t a = (std::uint32_t)(((color >> 24) & 0xffu) * alpha);
      std::uint32_t c = (color & 0x00ffffffu) | (a << 24);
      f32_t w = std::max(1.5f, width * scale);
      // round ends, so the pieces of a line join without notches
      list->AddCircleFilled(at(points.front()), w * 0.5f, c);
      if (points.size() == 1) {
        return;
      }
      for (const auto& p : points) {
        list->PathLineTo(at(p));
      }
      list->PathStroke(c, 0, w);
      list->AddCircleFilled(at(points.back()), w * 0.5f, c);
    };
    list->PushClipRect(min, min + size, true);
    for (const auto& stroke : strokes) {
      f32_t alpha = ecps_backend_t::draw_alpha(stroke, now);
      for (const auto& part : stroke.parts) {
        put(part, stroke.color, stroke.width, alpha);
      }
    }
    if (live) {
      put(pen_line.pending, pen_line.color, pen_width, 1.f);
    }
    list->PopClipRect();
  }

  // the colours, clear and close, in a strip at the picture's left while the pen is on
  void render_pen_tools(fan::vec2 frame_min, fan::vec2 frame_size, ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t source) {
    fan::vec2 restore = gui::get_cursor_screen_pos();
    f32_t button = gui::get_frame_height();
    f32_t gap = 6.f;
    std::size_t count = std::size(pen_colors);
    f32_t height = (count + 2) * button + (count + 1) * gap + 12.f;
    fan::vec2 window_min = gui::get_window_pos();
    fan::vec2 window_max = window_min + gui::get_window_size();
    f32_t x = std::max(frame_min.x, window_min.x) + 10.f;
    f32_t top = std::max(frame_min.y, window_min.y);
    f32_t bottom = std::min(frame_min.y + frame_size.y, window_max.y);
    f32_t y = std::max(top + 10.f, (top + bottom - height) * 0.5f);
    auto* list = gui::get_window_draw_list();
    list->AddRectFilled(fan::vec2(x, y), fan::vec2(x + button + 12.f, y + height), 0xb0101010u, 8.f);
    fan::vec2 at(x + 6.f, y + 6.f);
    for (std::size_t i = 0; i < count; ++i) {
      gui::set_cursor_screen_pos(at);
      gui::invisible_button(("##pen_color" + std::to_string(i)).c_str(), fan::vec2(button, button));
      if (gui::is_item_clicked()) {
        pen_color = (int)i;
      }
      bool hot = gui::is_item_hovered();
      fan::vec2 center = at + fan::vec2(button, button) * 0.5f;
      if ((int)i == pen_color) {
        list->AddCircle(center, button * 0.5f - 1.f, 0xffffffffu, 0, 2.f);
      }
      list->AddCircleFilled(center, button * (hot ? 0.36f : 0.32f), pen_colors[i]);
      list->AddCircle(center, button * (hot ? 0.36f : 0.32f), 0x80808080u, 0, 1.f);
      at.y += button + gap;
    }
    gui::set_cursor_screen_pos(at);
    if (icon_button("##pen_clear", icon_e::trash, "Clear the drawings on this screen", button, icon_style_e::overlay)) {
      pen_clear(channel_id, source);
    }
    at.y += button + gap;
    gui::set_cursor_screen_pos(at);
    if (icon_button("##pen_close", icon_e::close, "Stop drawing", button, icon_style_e::overlay)) {
      pen_on = false;
    }
    gui::set_cursor_screen_pos(restore);
  }

  // in both stream views, over the picture: the lines on the watched screen, and the pen while it is on.
  // Drag with the left button to draw; with Ctrl held the stream zooms as usual
  void draw_pen() {
    auto* rt = get_render_thread();
    if (!rt || rt->network_frame.get_image() == engine.default_texture) {
      pen_stop();
      return;
    }
    fan::vec2 frame_size = rt->network_frame.get_size() * 2;
    fan::vec2 frame_min = gui::get_window_pos() + fan::vec2(rt->network_frame.get_position()) - frame_size / 2;
    if (frame_size.x <= 0 || frame_size.y <= 0) {
      return;
    }
    ecps_backend_t::Protocol_ChannelID_t channel_id;
    std::uint32_t source = 0;
    if (!pen_target(channel_id, source)) {
      pen_on = false;
      pen_stop();
      return;
    }
    // what the pen draws on changed: the line ends
    if (pen_line.active && (pen_line.source != source || pen_line.channel != (std::uint32_t)channel_id.i)) {
      pen_stop();
    }
    draw_strokes(gui::get_window_draw_list(), frame_min, frame_size, source);
    if (!pen_on) {
      pen_stop();
      return;
    }
    render_pen_tools(frame_min, frame_size, channel_id, source);

    auto& io = gui::get_io();
    fan::vec2 mouse = gui::get_mouse_pos();
    fan::vec2 at((mouse.x - frame_min.x) / frame_size.x, (mouse.y - frame_min.y) / frame_size.y);
    bool over = at.x >= 0.f && at.x <= 1.f && at.y >= 0.f && at.y <= 1.f && !gui::is_any_item_hovered() && !gui::is_any_popup_open();
    if (over && !io.KeyCtrl) {
      gui::set_mouse_cursor(gui::mouse_cursor_hand);
    }
    auto now = std::chrono::steady_clock::now();
    if (!pen_line.active) {
      if (over && !io.KeyCtrl && io.MouseClicked[0]) {
        pen_line = {};
        pen_line.active = true;
        pen_line.id = pen_next_id++;
        pen_line.channel = (std::uint32_t)channel_id.i;
        pen_line.source = source;
        pen_line.color = pen_colors[std::clamp(pen_color, 0, (int)std::size(pen_colors) - 1)];
        pen_line.pending.push_back(at);
        pen_line.sent_at = now;
      }
      return;
    }
    at = fan::vec2(std::clamp(at.x, 0.f, 1.f), std::clamp(at.y, 0.f, 1.f));
    if (!io.MouseDown[0]) {
      pen_stop();
      return;
    }
    fan::vec2 last = pen_line.pending.back();
    f32_t dx = (at.x - last.x) * frame_size.x;
    f32_t dy = (at.y - last.y) * frame_size.y;
    if (dx * dx + dy * dy >= 4.f) {
      pen_line.pending.push_back(at);
    }
    if (pen_line.pending.size() >= ecps_backend_t::Protocol::DrawPointsMax ||
      (pen_line.pending.size() >= 2 && now - pen_line.sent_at > std::chrono::milliseconds(40))) {
      pen_send(false);
    }
  }

  // the bottom bar's pen button, only where drawing works
  void render_pen_button(f32_t frame_height) {
    ecps_backend_t::Protocol_ChannelID_t channel_id;
    std::uint32_t source = 0;
    if (!pen_target(channel_id, source)) {
      return;
    }
    gui::same_line(0, 8.f);
    if (icon_button("##btn_pen", icon_e::edit, pen_on ? "Stop drawing" : "Draw on the stream: everyone watching it and the one sharing it see the lines",
      frame_height, icon_style_e::overlay)) {
      pen_on = !pen_on;
    }
    if (pen_on) {
      fan::vec2 a = gui::get_item_rect_min();
      fan::vec2 b = gui::get_item_rect_max();
      gui::get_window_draw_list()->AddRectFilled(fan::vec2(a.x + 6.f, b.y - 2.f), fan::vec2(b.x - 6.f, b.y), accent_hover_u32, 1.f);
    }
  }

  // ---- the lines on the sharer's own screen (Windows) ---------------------------------------------------------
  // A see-through window over the shared screen that clicks go through, kept out of the capture so the viewers don't
  // get the lines twice. Drawn into a bitmap only when a line changes or while one fades, and only where lines are
#if defined(_WIN32)
  HWND pen_overlay = nullptr;
  bool pen_overlay_shown = false;
  bool pen_overlay_failed = false;
  bool pen_overlay_full = true;
  HDC pen_overlay_dc = nullptr;
  HBITMAP pen_overlay_bitmap = nullptr;
  HGDIOBJ pen_overlay_old = nullptr;
  std::uint32_t* pen_overlay_pixels = nullptr;
  fan::vec2i pen_overlay_size{ 0, 0 };
  fan::vec2i pen_overlay_at{ 0, 0 };
  RECT pen_overlay_dirty{ 0, 0, 0, 0 }; // where lines were drawn last time, cleared before the next
  std::uint64_t pen_overlay_revision = ~0ull;
  std::chrono::steady_clock::time_point pen_overlay_drawn_at;

  void pen_overlay_destroy() {
    if (pen_overlay_dc) {
      if (pen_overlay_old) {
        SelectObject(pen_overlay_dc, pen_overlay_old);
      }
      DeleteDC(pen_overlay_dc);
    }
    if (pen_overlay_bitmap) {
      DeleteObject(pen_overlay_bitmap);
    }
    if (pen_overlay) {
      DestroyWindow(pen_overlay);
    }
    pen_overlay = nullptr;
    pen_overlay_dc = nullptr;
    pen_overlay_bitmap = nullptr;
    pen_overlay_old = nullptr;
    pen_overlay_pixels = nullptr;
    pen_overlay_shown = false;
    pen_overlay_size = { 0, 0 };
    pen_overlay_dirty = { 0, 0, 0, 0 };
  }

  void pen_overlay_hide() {
    if (pen_overlay && pen_overlay_shown) {
      ShowWindow(pen_overlay, SW_HIDE);
      pen_overlay_shown = false;
    }
    pen_overlay_revision = ~0ull;
  }

  bool pen_overlay_create(fan::vec2i at, fan::vec2i size) {
    if (pen_overlay && size == pen_overlay_size) {
      if (at != pen_overlay_at) {
        SetWindowPos(pen_overlay, HWND_TOPMOST, at.x, at.y, size.x, size.y, SWP_NOACTIVATE);
        pen_overlay_at = at;
      }
      return true;
    }
    pen_overlay_destroy();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
      WNDCLASSW window_class{};
      window_class.lpfnWndProc = DefWindowProcW;
      window_class.hInstance = instance;
      window_class.lpszClassName = L"ecps_pen_overlay";
      RegisterClassW(&window_class);
      registered = true;
    }
    pen_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"ecps_pen_overlay", L"", WS_POPUP, at.x, at.y, size.x, size.y, nullptr, nullptr, instance, nullptr);
    if (!pen_overlay) {
      pen_overlay_failed = true;
      return false;
    }
    // without it the lines would be in the share as well, and the viewers would see them twice
    if (!SetWindowDisplayAffinity(pen_overlay, 0x11 /* WDA_EXCLUDEFROMCAPTURE */)) {
      fan::print("drawings on the screen: this Windows can't keep a window out of the capture, they show only in the preview");
      pen_overlay_destroy();
      pen_overlay_failed = true;
      return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size.x;
    info.bmiHeader.biHeight = -size.y;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    pen_overlay_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    void* bits = nullptr;
    pen_overlay_bitmap = pen_overlay_dc ? CreateDIBSection(pen_overlay_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!pen_overlay_bitmap || !bits) {
      pen_overlay_destroy();
      pen_overlay_failed = true;
      return false;
    }
    pen_overlay_old = SelectObject(pen_overlay_dc, pen_overlay_bitmap);
    pen_overlay_pixels = (std::uint32_t*)bits;
    std::memset(pen_overlay_pixels, 0, (std::size_t)size.x * size.y * 4);
    pen_overlay_size = size;
    pen_overlay_at = at;
    pen_overlay_full = true;
    return true;
  }

  // a round-ended line, anti-aliased, into the premultiplied bitmap. Where lines cross the stronger one stays
  void pen_overlay_segment(fan::vec2 a, fan::vec2 b, f32_t radius, std::uint32_t color, f32_t alpha, RECT& box) {
    int w = pen_overlay_size.x;
    int h = pen_overlay_size.y;
    int x0 = std::max(0, (int)std::floor(std::min(a.x, b.x) - radius - 1.f));
    int y0 = std::max(0, (int)std::floor(std::min(a.y, b.y) - radius - 1.f));
    int x1 = std::min(w - 1, (int)std::ceil(std::max(a.x, b.x) + radius + 1.f));
    int y1 = std::min(h - 1, (int)std::ceil(std::max(a.y, b.y) + radius + 1.f));
    if (x0 > x1 || y0 > y1) {
      return;
    }
    box.left = std::min<LONG>(box.left, x0);
    box.top = std::min<LONG>(box.top, y0);
    box.right = std::max<LONG>(box.right, x1 + 1);
    box.bottom = std::max<LONG>(box.bottom, y1 + 1);
    f32_t red = (f32_t)(color & 0xffu);
    f32_t green = (f32_t)((color >> 8) & 0xffu);
    f32_t blue = (f32_t)((color >> 16) & 0xffu);
    f32_t strength = ((color >> 24) & 0xffu) / 255.f * alpha;
    fan::vec2 d = b - a;
    f32_t length2 = d.x * d.x + d.y * d.y;
    for (int y = y0; y <= y1; ++y) {
      std::uint32_t* row = pen_overlay_pixels + (std::size_t)y * w;
      f32_t py = y + 0.5f;
      for (int x = x0; x <= x1; ++x) {
        f32_t px = x + 0.5f;
        f32_t t = length2 > 0.f ? std::clamp(((px - a.x) * d.x + (py - a.y) * d.y) / length2, 0.f, 1.f) : 0.f;
        f32_t ex = px - (a.x + t * d.x);
        f32_t ey = py - (a.y + t * d.y);
        f32_t cover = std::clamp(radius + 0.5f - std::sqrt(ex * ex + ey * ey), 0.f, 1.f);
        if (cover <= 0.f) {
          continue;
        }
        f32_t A = cover * strength;
        std::uint32_t a8 = (std::uint32_t)(A * 255.f + 0.5f);
        if (a8 <= (row[x] >> 24)) {
          continue;
        }
        row[x] = (a8 << 24) | ((std::uint32_t)(red * A + 0.5f) << 16) | ((std::uint32_t)(green * A + 0.5f) << 8) |
          (std::uint32_t)(blue * A + 0.5f);
      }
    }
  }

  void update_pen_overlay() {
    auto* rt = get_render_thread();
    std::vector<ecps_backend_t::draw_stroke_t> strokes;
    if (draw_overlay && !pen_overlay_failed && rt && ecps_backend.is_streaming_to_any_channel() && !stream_settings.share_camera) {
      strokes = ecps_backend.draw_strokes_of((std::uint32_t)ecps_backend.session_id.i);
    }
    if (strokes.empty()) {
      pen_overlay_hide();
      if (!draw_overlay && pen_overlay) {
        pen_overlay_destroy();
      }
      return;
    }
    auto& geometry = rt->screen_encoder.mdscr.Geometry;
    fan::vec2i size(geometry.Resolution.x, geometry.Resolution.y);
    fan::vec2i origin = rt->screen_encoder.mdscr.desktop_origin;
    if (size.x <= 0 || size.y <= 0) {
      pen_overlay_hide();
      return;
    }
    std::int64_t now_ms = ecps_backend_t::draw_now_ms();
    bool fading = std::any_of(strokes.begin(), strokes.end(), [&](const auto& stroke) { return ecps_backend_t::draw_alpha(stroke, now_ms) < 1.f; });
    std::uint64_t revision = ecps_backend.draw_revision.load();
    auto now = std::chrono::steady_clock::now();
    if (revision == pen_overlay_revision && (!fading || now - pen_overlay_drawn_at < std::chrono::milliseconds(33))) {
      return;
    }
    if (!pen_overlay_create(origin, size)) {
      return;
    }
    pen_overlay_revision = revision;
    pen_overlay_drawn_at = now;

    RECT old = pen_overlay_dirty;
    for (LONG y = old.top; y < old.bottom; ++y) {
      std::memset(pen_overlay_pixels + (std::size_t)y * size.x + old.left, 0, (std::size_t)(old.right - old.left) * 4);
    }
    RECT box{ size.x, size.y, 0, 0 };
    f32_t scale = size.y / 1080.f;
    for (const auto& stroke : strokes) {
      f32_t alpha = ecps_backend_t::draw_alpha(stroke, now_ms);
      f32_t radius = std::max(1.f, stroke.width * scale * 0.5f);
      for (const auto& part : stroke.parts) {
        for (std::size_t i = 0; i < part.size(); ++i) {
          fan::vec2 a(part[i].x * size.x, part[i].y * size.y);
          fan::vec2 b = i + 1 < part.size() ? fan::vec2(part[i + 1].x * size.x, part[i + 1].y * size.y) : a;
          if (i + 1 < part.size() || part.size() == 1) {
            pen_overlay_segment(a, b, radius, stroke.color, alpha, box);
          }
        }
      }
    }
    if (box.right <= box.left) {
      box = { 0, 0, 0, 0 };
    }
    pen_overlay_dirty = box;

    RECT dirty{ std::min(old.left, box.left), std::min(old.top, box.top), std::max(old.right, box.right), std::max(old.bottom, box.bottom) };
    if (old.right <= old.left) {
      dirty = box;
    }
    else if (box.right <= box.left) {
      dirty = old;
    }
    POINT destination{ origin.x, origin.y };
    POINT source{ 0, 0 };
    SIZE extent{ size.x, size.y };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO update{};
    update.cbSize = sizeof(update);
    update.pptDst = &destination;
    update.psize = &extent;
    update.hdcSrc = pen_overlay_dc;
    update.pptSrc = &source;
    update.pblend = &blend;
    update.dwFlags = ULW_ALPHA;
    update.prcDirty = pen_overlay_full || dirty.right <= dirty.left ? nullptr : &dirty;
    UpdateLayeredWindowIndirect(pen_overlay, &update);
    pen_overlay_full = false;
    if (!pen_overlay_shown) {
      SetWindowPos(pen_overlay, HWND_TOPMOST, origin.x, origin.y, size.x, size.y, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      pen_overlay_shown = true;
    }
  }
#else
  void update_pen_overlay() {}
#endif

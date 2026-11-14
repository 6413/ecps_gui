// Part of ecps_gui_t, included inside the struct by gui.h: choosing what to share on Windows, the window picker, the host pointer visibility.

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
    std::string label = current == 0 ? std::string("Whole screen") : capture_window_title;
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

// Part of ecps_gui_t, included inside the struct by gui.h: choosing what to share on Windows, the window picker, the host pointer visibility.

#if defined(_WIN32)
  // ---- what is captured: the whole screen, or one window (Linux asks in the portal instead) ------------------

  struct capture_window_t {
    std::uintptr_t handle;
    std::string title;
  };
  std::vector<capture_window_t> capture_windows;
  std::string capture_window_title;

  // the last few shared, by title: one click in the picker shares one again (a window's handle changes every run)
  struct recent_source_t {
    bool window = false;
    std::string title;
  };
  std::vector<recent_source_t> recent_sources;
  bool recent_sources_loaded = false;

  void load_recent_sources() {
    if (recent_sources_loaded) {
      return;
    }
    recent_sources_loaded = true;
    if (!config.contains("recent_sources") || !config["recent_sources"].is_array()) {
      return;
    }
    for (const auto& item : config["recent_sources"]) {
      if (item.is_object() && item.contains("title") && item["title"].is_string()) {
        recent_sources.push_back({item.value("window", false), item["title"].get<std::string>()});
      }
    }
  }

  void remember_source(bool window, const std::string& title) {
    if (title.empty()) {
      return;
    }
    load_recent_sources();
    std::erase_if(recent_sources, [&](const recent_source_t& r) { return r.window == window && r.title == title; });
    recent_sources.insert(recent_sources.begin(), {window, title});
    if (recent_sources.size() > 5) {
      recent_sources.resize(5);
    }
    fan::json list = fan::json::array();
    for (const auto& r : recent_sources) {
      list.push_back({{"window", r.window}, {"title", r.title}});
    }
    write_to_config("recent_sources", list);
  }

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

  // ---- the monitors, each one a choice in the picker's Screens tab --------------------------------------------
  // A monitor's thumbnail is kept under its HMONITOR with the top bit set, so it never meets a window's HWND
  static constexpr std::uintptr_t monitor_key_bit = (std::uintptr_t)1 << (sizeof(std::uintptr_t) * 8 - 1);
  struct capture_monitor_t {
    std::uintptr_t handle; // HMONITOR
    std::string title;     // the monitor's own name (Odyssey G81SF), else "Screen N"
    RECT rect;
    bool primary;
  };
  std::vector<capture_monitor_t> capture_monitors;

  static std::string utf8_of(const wchar_t* text) {
    int length = (int)wcslen(text);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
    return out;
  }

  // DISPLAY1 -> the name the monitor reports, as Windows' display settings show it
  static std::unordered_map<std::wstring, std::string> monitor_names() {
    std::unordered_map<std::wstring, std::string> names;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
      return names;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
      return names;
    }
    for (UINT32 i = 0; i < path_count; ++i) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
      source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source.header.size = sizeof(source);
      source.header.adapterId = paths[i].sourceInfo.adapterId;
      source.header.id = paths[i].sourceInfo.id;
      DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
      target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target.header.size = sizeof(target);
      target.header.adapterId = paths[i].targetInfo.adapterId;
      target.header.id = paths[i].targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS &&
        target.monitorFriendlyDeviceName[0]) {
        names[source.viewGdiDeviceName] = utf8_of(target.monitorFriendlyDeviceName);
      }
    }
    return names;
  }

  static BOOL CALLBACK collect_capture_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* list = (std::vector<capture_monitor_t>*)param;
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
      // the device name for now, swapped for the monitor's own name after
      list->push_back({(std::uintptr_t)monitor, utf8_of(info.szDevice), info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    }
    return TRUE;
  }

  void refresh_capture_monitors() {
    capture_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, collect_capture_monitor, (LPARAM)&capture_monitors);
    // the main display first, the rest from left to right as they stand
    std::sort(capture_monitors.begin(), capture_monitors.end(), [](const auto& a, const auto& b) {
      return a.primary != b.primary ? a.primary : a.rect.left < b.rect.left;
    });
    auto names = monitor_names();
    int number = 1;
    for (auto& monitor : capture_monitors) {
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      GetMonitorInfoW((HMONITOR)monitor.handle, &info);
      auto found = names.find(info.szDevice);
      monitor.title = found != names.end() ? found->second : "Screen " + std::to_string(number);
      if (monitor.primary) {
        monitor.title += " (main)";
      }
      ++number;
    }
  }

  // the screen being shared when no window is: target_monitor, 0 the main display
  const capture_monitor_t* shared_monitor(std::uintptr_t target_monitor) {
    for (const auto& monitor : capture_monitors) {
      if (target_monitor ? monitor.handle == target_monitor : monitor.primary) {
        return &monitor;
      }
    }
    return nullptr;
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
  std::chrono::steady_clock::time_point capture_picker_opened_at;

  // a window that is still open (one picked before an update restart may have closed since)
  static bool capture_window_open(std::uintptr_t handle) {
    return handle && IsWindow((HWND)handle);
  }

  // thumb_pixels gets the picture as rgba, at most max_width wide. handle 0 is the screen
  bool grab_window_rgba(std::uintptr_t handle, int max_width, fan::vec2i& size) {
    HWND hwnd = (HWND)handle;
    RECT rect;
    int width, height;
    // the screen part: the main display (0) or one monitor (its key), copied from the desktop at its place
    bool screen = handle == 0 || (handle & monitor_key_bit);
    int source_x = 0, source_y = 0;
    if (handle & monitor_key_bit) {
      const capture_monitor_t* monitor = nullptr;
      for (const auto& m : capture_monitors) {
        if (m.handle == (handle & ~monitor_key_bit)) {
          monitor = &m;
        }
      }
      if (!monitor) {
        return false;
      }
      source_x = monitor->rect.left;
      source_y = monitor->rect.top;
      width = monitor->rect.right - monitor->rect.left;
      height = monitor->rect.bottom - monitor->rect.top;
    }
    else if (handle == 0) {
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
    int bitmap_width = screen ? std::min(width, max_width) : width;
    int bitmap_height = screen ? std::max(1, height * bitmap_width / width) : height;
    info.bmiHeader.biWidth = bitmap_width;
    info.bmiHeader.biHeight = -bitmap_height;
    void* bits = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (bitmap) {
      HGDIOBJ old = SelectObject(dc, bitmap);
      if (screen) {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        ok = StretchBlt(dc, 0, 0, bitmap_width, bitmap_height, screen_dc, source_x, source_y, width, height, SRCCOPY) != 0;
      }
      else {
        ok = PrintWindow(hwnd, dc, 2) != 0;
      }
      const std::uint8_t* source = (const std::uint8_t*)bits;
      int out_width = bitmap_width;
      int out_height = bitmap_height;
      HDC small_dc = nullptr;
      HBITMAP small_bitmap = nullptr;
      HGDIOBJ small_old = nullptr;
      // a window comes at full size, it is shrunk the same smooth way the screen is (averaging, not skipping pixels)
      if (ok && bitmap_width > max_width) {
        out_width = max_width;
        out_height = std::max(1, bitmap_height * out_width / bitmap_width);
        BITMAPINFO small_info = info;
        small_info.bmiHeader.biWidth = out_width;
        small_info.bmiHeader.biHeight = -out_height;
        void* small_bits = nullptr;
        small_dc = CreateCompatibleDC(screen_dc);
        small_bitmap = small_dc ? CreateDIBSection(small_dc, &small_info, DIB_RGB_COLORS, &small_bits, nullptr, 0) : nullptr;
        ok = small_bitmap != nullptr;
        if (ok) {
          small_old = SelectObject(small_dc, small_bitmap);
          SetStretchBltMode(small_dc, HALFTONE);
          SetBrushOrgEx(small_dc, 0, 0, nullptr);
          ok = StretchBlt(small_dc, 0, 0, out_width, out_height, dc, 0, 0, bitmap_width, bitmap_height, SRCCOPY) != 0;
          source = (const std::uint8_t*)small_bits;
        }
      }
      if (ok) {
        thumb_pixels.resize((std::size_t)out_width * out_height * 4);
        for (std::size_t i = 0, count = (std::size_t)out_width * out_height; i < count; ++i) {
          const std::uint8_t* from = source + i * 4;
          std::uint8_t* to = &thumb_pixels[i * 4];
          // kept as sRGB, the texture is an sRGB one (made linear here too, the thumbnails came out too dark)
          to[0] = from[2];
          to[1] = from[1];
          to[2] = from[0];
          to[3] = 255;
        }
        size = fan::vec2i(out_width, out_height);
      }
      if (small_dc) {
        if (small_old) {
          SelectObject(small_dc, small_old);
        }
        if (small_bitmap) {
          DeleteObject(small_bitmap);
        }
        DeleteDC(small_dc);
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
    return refresh_thumb_into(capture_thumbs[handle], handle, max_width, interval_ms, budget);
  }

  capture_thumb_t& refresh_thumb_into(capture_thumb_t& thumb, std::uintptr_t handle, int max_width, int interval_ms, int& budget) {
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
        thumb.image.reload(image_info, smooth_image());
      }
      else {
        thumb.image = engine.image_load(image_info, smooth_image());
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
      bool alive = it->first == 0 || (it->first & monitor_key_bit) || std::any_of(capture_windows.begin(), capture_windows.end(), [&](const auto& w) { return w.handle == it->first; });
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

  // what is shared (a window, or the screen for 0), as it looks now
  capture_thumb_t share_preview_thumb;
  std::uintptr_t share_preview_handle = (std::uintptr_t)-1;

  void draw_share_preview(std::uintptr_t handle, f32_t) {
    if (capture_preview_recent() && draw_captured_preview()) {
      return;
    }
    if (handle != share_preview_handle) {
      share_preview_handle = handle;
      share_preview_thumb.loaded = false; // another source, take a new picture now
    }
    int budget = 1;
    auto& thumb = refresh_thumb_into(share_preview_thumb, handle, 1920,
      1000 / std::clamp(preview_fps, 1, 60), budget);
    if (thumb.loaded) {
      draw_resizable_preview(thumb.image, thumb.size);
    }
  }

  // The window picker: every window as a small picture, click one to share it, like the share pickers of chat apps
  fan::vec2 capture_picker_size;
  // 0 Screens, 1 Applications. The picker opens on the kind being shared
  int capture_picker_tab = 0;
  int capture_picker_open_tab = -1;
  std::chrono::steady_clock::time_point capture_monitors_listed;

  void render_capture_picker(fan::graphics::screen_capture_t& capture, render_thread_t* rt) {
    if (capture_picker_requested) {
      capture_picker_requested = false;
      refresh_capture_windows();
      refresh_capture_monitors();
      capture_picker_open_tab = capture.target_window.load() != 0 ? 1 : 0;
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
      refresh_capture_monitors();
      capture_picker_opened_at = now;
    }

    gui::text(gui::palette_color(gui::widget_palette.accent_text), "CHOOSE WHAT TO SHARE");
    gui::same_line();
    gui::text(fan::color(0.5f, 0.5f, 0.55f, 1.f), "  click one, Esc closes");

    // the recent ones still there, one click each
    bool recent_changed = false;
    f32_t recent_height = 0.f;
    {
      load_recent_sources();
      bool any = false;
      auto recent = recent_sources;
      for (std::size_t i = 0; i < recent.size(); ++i) {
        const auto& r = recent[i];
        const capture_window_t* window = nullptr;
        const capture_monitor_t* monitor = nullptr;
        if (r.window) {
          for (const auto& w : capture_windows) {
            if (w.title == r.title) {
              window = &w;
              break;
            }
          }
        }
        else {
          for (const auto& m : capture_monitors) {
            if (m.title == r.title) {
              monitor = &m;
              break;
            }
          }
        }
        if (!window && !monitor) {
          continue;
        }
        if (!any) {
          any = true;
          gui::align_text_to_frame_padding();
          gui::text_disabled("Recent");
        }
        gui::same_line();
        std::string shown = gui::fit_title(r.title, 180.f) + "##recent_source_" + std::to_string(i);
        if (gui::button(shown.c_str())) {
          if (window) {
            capture_window_title = window->title;
            capture.target_window = window->handle;
          }
          else {
            capture.target_window = 0;
            capture.target_monitor = monitor->primary ? 0 : monitor->handle;
          }
          remember_source(r.window, r.title);
          recent_changed = true;
        }
        if (gui::is_item_hovered()) {
          gui::set_tooltip(r.title);
        }
      }
      if (any) {
        recent_height = gui::get_frame_height() + gui::get_style().ItemSpacing.y;
      }
    }

    // Screens and Applications as tabs, like the share pickers of chat apps
    int open_tab = capture_picker_open_tab;
    capture_picker_open_tab = -1;
    if (gui::begin_tab_bar("##share_kinds")) {
      if (gui::begin_tab_item("Screens", nullptr, open_tab == 0 ? gui::tab_item_flags_set_selected : 0)) {
        capture_picker_tab = 0;
        gui::end_tab_item();
      }
      if (gui::begin_tab_item("Applications", nullptr, open_tab == 1 ? gui::tab_item_flags_set_selected : 0)) {
        capture_picker_tab = 1;
        gui::end_tab_item();
      }
      gui::end_tab_bar();
    }

    // the cards scroll inside a box of a fixed size, the popup itself never grows past the window
    fan::vec2 box = capture_picker_size - fan::vec2(28.f, gui::get_frame_height() * 2.f + 40.f + recent_height);
    gui::push_style_color(gui::col_child_bg, fan::color(0, 0, 0, 0));
    gui::begin_child("##capture_cards", box);
    std::uintptr_t current = capture.target_window.load();
    std::uintptr_t current_monitor = capture.target_monitor.load();
    f32_t title_height = gui::get_font_size() + 10.f;
    const fan::vec2 image_area(200.f, 112.f);
    const fan::vec2 card_size(image_area.x + 12.f, image_area.y + 12.f + title_height);
    f32_t spacing = 10.f;
    int columns = std::max(1, (int)((gui::get_content_region_avail().x + spacing) / (card_size.x + spacing)));
    int budget = 2;
    bool changed = false;
    int index = 0;
    auto* list = gui::get_window_draw_list();

    // one picture with its name; true when clicked. handle is the thumbnail's key
    auto card = [&](std::uintptr_t handle, const std::string& title, bool selected) -> bool {
      if (index % columns != 0) {
        gui::same_line(0, spacing);
      }
      ++index;
      fan::vec2 at = gui::get_cursor_screen_pos();
      gui::push_id((int)(handle & 0x7fffffff));
      bool clicked = gui::invisible_button("##card", card_size);
      gui::pop_id();
      bool hovered = gui::is_item_hovered();
      if (hovered) {
        gui::set_tooltip(title);
      }
      list->AddRectFilled(at, at + card_size, hovered ? gui::widget_palette.surface_hover : gui::widget_palette.surface, 8.f);
      if (selected) {
        list->AddRect(at, at + card_size, accent_u32, 8.f, 0, 2.5f);
      }
      else if (hovered) {
        list->AddRect(at, at + card_size, accent_alpha(accent_u32, 0x90), 8.f, 0, 1.5f);
      }

      auto& thumb = refresh_thumb(handle, 320, (handle & monitor_key_bit) ? 1500 : 2500, budget);
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
      std::string shown = gui::fit_title(title, image_area.x);
      list->AddText(at + fan::vec2(8.f, image_area.y + 14.f), selected ? 0xffffffffu : 0xffb4b4b4u, shown.c_str());
      // back to where the card ends, so the next one lines up
      gui::set_cursor_screen_pos(at);
      gui::dummy(card_size);

      return clicked;
    };

    if (capture_picker_tab == 0) {
      for (const auto& monitor : capture_monitors) {
        bool selected = current == 0 && (current_monitor ? monitor.handle == current_monitor : monitor.primary);
        if (card(monitor.handle | monitor_key_bit, monitor.title, selected)) {
          capture.target_window = 0;
          // the main display as 0: it stays the main one even if Windows changes which that is
          capture.target_monitor = monitor.primary ? 0 : monitor.handle;
          remember_source(false, monitor.title);
          changed = true;
        }
      }
    }
    else {
      for (const auto& window : capture_windows) {
        if (card(window.handle, window.title, window.handle == current)) {
          capture_window_title = window.title;
          capture.target_window = window.handle;
          remember_source(true, window.title);
          changed = true;
        }
      }
      if (capture_windows.empty()) {
        gui::text_disabled("No windows to share");
      }
    }
    gui::end_child();
    gui::pop_style_color();

    changed = changed || recent_changed;
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
    // the monitors again now and then: one plugged in or out shows up in the label and the picker
    auto now = std::chrono::steady_clock::now();
    if (capture_monitors.empty() || now - capture_monitors_listed > std::chrono::seconds(5)) {
      refresh_capture_monitors();
      capture_monitors_listed = now;
    }
    std::string screen_label = "Whole screen";
    if (capture_monitors.size() > 1) {
      if (const auto* monitor = shared_monitor(capture.target_monitor.load())) {
        screen_label = monitor->title;
      }
    }
    std::string label = current == 0 ? screen_label : capture_window_title;
    gui::settings_row("Source", [&] {
      std::string shown = gui::fit_title(label, gui::get_content_region_avail().x - gui::get_style().FramePadding.x * 2.f);
      if (gui::button(shown + "##capture_source", fan::vec2(-1.f, 0.f))) {
        capture_picker_requested = true;
      }
      if (gui::is_item_hovered()) {
        gui::set_tooltip("Choose what to share");
      }
    });
    render_capture_picker(capture, rt);
    // an HDR screen is tone mapped for sharing, how bright that comes out is up to taste and differs per game
    capture.hdr_exposure = hdr_exposure;
    gui::settings_switch("Preview", &preview_own_share, "See what you share");
#if defined(_WIN32)
    if (gui::settings_switch("Drawings on my screen", &draw_overlay,
      "Lines the viewers draw show on your screen too, over everything, and fade after a few seconds. "
      "They are not in what you share")) {
      save_ui_config();
    }
#endif
    // the one sharing may wipe what was drawn on its screen
    if (!ecps_backend.draw_strokes_of((std::uint32_t)ecps_backend.session_id.i).empty()) {
      for (const auto& channel : ecps_backend.channel_info) {
        if (channel.is_streaming && ecps_backend.draw_known(channel.channel_id)) {
          if (gui::button("Clear drawings##own_drawings")) {
            pen_clear(channel.channel_id, (std::uint32_t)ecps_backend.session_id.i);
          }
          break;
        }
      }
    }
    capture_preview_wanted = preview_own_share;
    if (preview_own_share) {
      render_preview_fps();
      draw_share_preview(current, gui::get_content_region_avail().x);
    }
  }

  // the HDR brightness slider, in the Advanced fold: it only exists on an HDR screen
  void render_hdr_light() {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    auto& capture = rt->screen_encoder.mdscr;
    capture.hdr_exposure = hdr_exposure;
    if (!capture.hdr_active.load()) {
      return;
    }
    gui::settings_row("HDR light", [&] {
      gui::slider("##hdr_brightness", &hdr_exposure, 0.2f, 2.f, gui::slider_flags_always_clamp);
      if (gui::is_item_hovered()) {
        gui::set_tooltip("How bright the HDR picture looks");
      }
      if (gui::is_item_deactivated_after_edit()) {
        save_ui_config();
      }
    });
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
    bool visible = !stream_settings.share_camera && (info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr &&
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
      for (const auto& channel : ecps_backend.channel_info) {
        if (channel.is_streaming) {
          ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewCursor_t rest;
          rest.ChannelID = channel.channel_id;
          rest.ChannelSessionID = channel.session_id;
          rest.X = x;
          rest.Y = y;
          rest.Visible = visible ? 1 : 0;
          if (auto sent = co_await ecps_expected(ecps_backend.tcp_write(
            ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewCursor, &rest, sizeof(rest))); !sent) {
            co_return;
          }
        }
      }
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

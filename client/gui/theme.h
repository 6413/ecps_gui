// Part of ecps_gui_t, included inside the struct by gui.h: the colours and the settings panel building blocks.

  // ---- the backdrop ----------------------------------------------------------------------------------------
  // A deep blue behind everything, with large soft glows that drift and swell slowly; the panels are see-through
  // so it shows through them like glass. Nothing of it is drawn over the stream picture

  fan::graphics::image_t glow_image;
  std::vector<std::uint8_t> glow_pixels;
  std::chrono::steady_clock::time_point backdrop_started = std::chrono::steady_clock::now();
  // where the stream picture was drawn last frame, the glows leave it alone
  bool picture_shown = false;
  fan::vec2 picture_min, picture_max;
  // the stream view's area, black behind the picture
  bool stream_area_shown = false;
  fan::vec2 stream_area_min, stream_area_max;
  // the window is cleared to black while the stream view is up (render_backdrop)
  bool backdrop_black = false;

  // ---- the colours the user picks ---------------------------------------------------------------------------
  // A theme is one accent colour and a few sliders: the dark colour behind the panels, the soft lights of the
  // backdrop and the buttons' blues all follow the accent, so nothing else has to be picked. The accent is in the
  // gui's own colour space (what the colour picker shows is what the buttons look like)
  struct theme_t {
    fan::color accent = fan::color(0.1f, 0.35f, 1.f, 1.f);
    // the backdrop's own colour, picked separately from the accent (gui colour space like the accent). Only its hue
    // and how colourful it is count, how light the backdrop is comes from the slider below
    fan::color background = fan::color(0.0245f, 0.2f, 0.7f, 1.f);
    f32_t background_brightness = 0.14f; // how light the backdrop is
    f32_t panel_opacity = 0.72f;         // how see-through the panels and fields are
    f32_t glow = 1.f;                    // how strong the soft lights in the backdrop are
    f32_t contrast = 0.f;                // 0 normal, 1 high contrast: white text, solid panels, clear borders
  };
  theme_t theme;
  // themes the user saved, alongside the built-in presets; kept in config.json under saved_themes
  struct saved_theme_t {
    std::string name;
    theme_t value;
    fan::json to_json() const {
      fan::json j = fan::json::object();
      j["name"] = name;
      fan::json accent = fan::json::array();
      accent.push_back((double)value.accent.r);
      accent.push_back((double)value.accent.g);
      accent.push_back((double)value.accent.b);
      j["accent"] = accent;
      fan::json background = fan::json::array();
      background.push_back((double)value.background.r);
      background.push_back((double)value.background.g);
      background.push_back((double)value.background.b);
      j["background"] = background;
      j["background_brightness"] = value.background_brightness;
      j["panel_opacity"] = value.panel_opacity;
      j["glow"] = value.glow;
      j["contrast"] = value.contrast;
      return j;
    }
    static saved_theme_t from_json(const fan::json& j) {
      saved_theme_t out;
      out.name = j.value("name", std::string("Theme"));
      auto read_color = [](const fan::json& a, fan::color& c) {
        if (a.is_array() && a.size() == 3) {
          c = fan::color(std::clamp((f32_t)a[0].get<double>(), 0.f, 1.f),
            std::clamp((f32_t)a[1].get<double>(), 0.f, 1.f), std::clamp((f32_t)a[2].get<double>(), 0.f, 1.f), 1.f);
        }
      };
      if (j.contains("accent")) read_color(j["accent"], out.value.accent);
      if (j.contains("background")) read_color(j["background"], out.value.background);
      out.value.background_brightness = std::clamp((f32_t)j.value("background_brightness", 0.14), 0.02f, 0.4f);
      out.value.panel_opacity = std::clamp((f32_t)j.value("panel_opacity", 0.72), 0.3f, 1.f);
      out.value.glow = std::clamp((f32_t)j.value("glow", 1.0), 0.f, 1.5f);
      out.value.contrast = std::clamp((f32_t)j.value("contrast", 0.0), 0.f, 1.f);
      return out;
    }
  };
  std::vector<saved_theme_t> saved_themes;
  std::string theme_name_input;
  bool backdrop_linear = true;
  bool theme_dirty = false;

  struct theme_preset_t {
    const char* name;
    theme_t value;
  };
  static const std::vector<theme_preset_t>& theme_presets() {
    // colours as they look on screen, taken into the gui's space
    auto lin = [](f32_t r, f32_t g, f32_t b) { return fan::color(std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f), 1.f); };
    // a preset's backdrop has the accent's hue, as many of the colours did before they were picked separately
    auto make = [](fan::color accent, f32_t brightness, f32_t tint, f32_t opacity, f32_t glow, f32_t contrast = 0.f) {
      f32_t h, s, v;
      theme_rgb_to_hsv(std::pow(accent.r, 1.f / 2.2f), std::pow(accent.g, 1.f / 2.2f), std::pow(accent.b, 1.f / 2.2f), h, s, v);
      fan::color shown = theme_hsv_to_rgb(h, tint, 1.f);
      theme_t t;
      t.accent = accent;
      t.background = fan::color(std::pow(shown.r, 2.2f), std::pow(shown.g, 2.2f), std::pow(shown.b, 2.2f), 1.f);
      t.background_brightness = brightness;
      t.panel_opacity = opacity;
      t.glow = glow;
      t.contrast = contrast;
      return t;
    };
    static const std::vector<theme_preset_t> list = {
      {"Blue", make(fan::color(0.1f, 0.35f, 1.f, 1.f), 0.14f, 0.64f, 0.72f, 1.f)},
      {"Violet", make(lin(0.62f, 0.45f, 1.f), 0.15f, 0.60f, 0.72f, 1.f)},
      {"Emerald", make(lin(0.30f, 0.85f, 0.55f), 0.11f, 0.55f, 0.72f, 1.f)},
      {"Crimson", make(lin(1.f, 0.38f, 0.42f), 0.14f, 0.60f, 0.72f, 1.f)},
      {"Amber", make(lin(1.f, 0.72f, 0.28f), 0.13f, 0.55f, 0.72f, 1.f)},
      {"Graphite", make(lin(0.75f, 0.78f, 0.85f), 0.09f, 0.12f, 0.80f, 0.5f)},
      // near black, no colour and no lights, solid panels
      {"Dark", make(lin(0.62f, 0.66f, 0.74f), 0.03f, 0.f, 1.f, 0.f)},
      // black, yellow and white: everything solid and outlined, for reading it easily
      {"High contrast", make(lin(1.f, 0.86f, 0.f), 0.02f, 0.f, 1.f, 0.f, 1.f)},
    };
    return list;
  }

  static void theme_rgb_to_hsv(f32_t r, f32_t g, f32_t b, f32_t& h, f32_t& s, f32_t& v) {
    f32_t hi = std::max({r, g, b});
    f32_t lo = std::min({r, g, b});
    f32_t d = hi - lo;
    v = hi;
    s = hi <= 0.f ? 0.f : d / hi;
    if (d <= 0.f) {
      h = 0.f;
    }
    else if (hi == r) {
      h = std::fmod((g - b) / d, 6.f);
    }
    else if (hi == g) {
      h = (b - r) / d + 2.f;
    }
    else {
      h = (r - g) / d + 4.f;
    }
    h /= 6.f;
    if (h < 0.f) {
      h += 1.f;
    }
  }
  static fan::color theme_hsv_to_rgb(f32_t h, f32_t s, f32_t v, f32_t a = 1.f) {
    h = h - std::floor(h);
    f32_t c = v * s;
    f32_t x = c * (1.f - std::fabs(std::fmod(h * 6.f, 2.f) - 1.f));
    f32_t m = v - c;
    f32_t r = 0, g = 0, b = 0;
    int sector = (int)(h * 6.f) % 6;
    switch (sector) {
      case 0: r = c; g = x; break;
      case 1: r = x; g = c; break;
      case 2: g = c; b = x; break;
      case 3: g = x; b = c; break;
      case 4: r = x; b = c; break;
      default: r = c; b = x; break;
    }
    return fan::color(r + m, g + m, b + m, a);
  }
  // the backdrop colour as it looks on screen, hue and all, for colours that follow it
  f32_t theme_hue() const {
    f32_t h, s, v;
    theme_rgb_to_hsv(std::pow(theme.background.r, 1.f / 2.2f), std::pow(theme.background.g, 1.f / 2.2f), std::pow(theme.background.b, 1.f / 2.2f), h, s, v);
    return h;
  }
  // how colourful the backdrop colour is, 0 for black, white and grays: those have no hue, so nothing may follow it
  f32_t theme_saturation() const {
    f32_t h, s, v;
    theme_rgb_to_hsv(std::pow(theme.background.r, 1.f / 2.2f), std::pow(theme.background.g, 1.f / 2.2f), std::pow(theme.background.b, 1.f / 2.2f), h, s, v);
    return s;
  }
  // the backdrop's colour as it looks on screen
  fan::color theme_background_display() const {
    return theme_hsv_to_rgb(theme_hue(), theme_saturation(), std::clamp(theme.background_brightness, 0.f, 1.f));
  }
  void apply_backdrop_color();

  // after the theme changed: what draws with fixed numbers has to be told
  void apply_theme();

  // the tab bars of docked windows, the title bars and the menu bar are drawn outside push_theme(), with the
  // style's own colours, so these are set there too
  void apply_global_colors();

  void save_theme();

  void save_saved_themes();

  void load_saved_themes(const fan::json& saved);


  void load_theme(const fan::json& saved);


  static std::uint32_t theme_pack(const fan::color& c) {
    auto byte = [](f32_t v) { return (std::uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
    return 0xff000000u | (byte(c.b) << 16) | (byte(c.g) << 8) | byte(c.r);
  }

  // ---- Ctrl + and Ctrl - make the text bigger and smaller, Ctrl 0 back to normal -------------------------------
  // Saved as fan's Text size (Graphics settings), the same value its slider changes
  int font_keys_held = 0;
  void handle_font_keys() {
    auto& window = engine.window;
    bool ctrl = window.is_key_down(fan::key_left_control) || window.is_key_down(fan::key_right_control);
    int keys = 0;
    if (ctrl) {
#if defined(_WIN32)
      // the layout's own + and - keys (a Finnish + is where a US - is), and the keypad's
      auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
      keys |= (down(VK_OEM_PLUS) || down(VK_ADD)) ? 1 : 0;
      keys |= (down(VK_OEM_MINUS) || down(VK_SUBTRACT)) ? 2 : 0;
      keys |= (down('0') || down(VK_NUMPAD0)) ? 4 : 0;
#else
      keys |= (window.is_key_down(fan::key_plus) || window.is_key_down(fan::key_numpad_add)) ? 1 : 0;
      keys |= (window.is_key_down(fan::key_minus) || window.is_key_down(fan::key_numpad_subtract)) ? 2 : 0;
      keys |= (window.is_key_down(fan::key_0) || window.is_key_down(fan::key_numpad_0)) ? 4 : 0;
#endif
    }
    int pressed = keys & ~font_keys_held;
    font_keys_held = keys;
    if (!pressed) {
      return;
    }
    f32_t scale = gui::get_font_scale();
    if (pressed & 1) {
      scale += 0.1f;
    }
    if (pressed & 2) {
      scale -= 0.1f;
    }
    if (pressed & 4) {
      scale = 1.f;
    }
    // the range fan's Text size slider allows
    scale = std::clamp(std::round(scale * 10.f) / 10.f, 0.5f, 3.f);
    gui::set_font_scale(scale);
    if (auto* settings = fan_settings_menu()) {
      settings->config.interface_.font_scale = scale;
      settings->mark_dirty();
    }
  }

  // the Theme tab of the settings panel: a few ready-made themes, and the accent and four sliders to make one
  void render_theme_tab();


  void setup_backdrop();


  // drawn into the full-window background window, before the main window
  void render_backdrop();


  // ---- the theme -------------------------------------------------------------------------------------------
  // Pushed around the main window, so every panel, popup and control in it shares it. Dark fields and buttons,
  // blue where something is chosen or used. (The gui is drawn in linear colour and shown through sRGB: small
  // numbers here look much lighter on screen)
  int theme_colors_pushed = 0;
  int theme_vars_pushed = 0;

  void push_theme();


  void pop_theme();


  // ---- the share preview's size ------------------------------------------------------------------------------
  // a part of the panel's width, changed with the handle at the picture's lower right corner and kept in the config
  f32_t preview_fraction = 1.f;
  bool preview_resizing = false;

  void draw_resizable_preview(fan::graphics::image_t image, fan::vec2 image_size);


  // ---- building blocks of the settings panel ---------------------------------------------------------------
  // (the gui is drawn in linear colour and shown through sRGB: small numbers here look much lighter on screen)

  // the accent for what is drawn by hand: a clear blue until the theme says otherwise (apply_theme)
  inline static std::uint32_t accent_u32 = 0xffff5919u;
  inline static std::uint32_t accent_hover_u32 = 0xffff7a33u;
  // the strip behind the top bar, from the backdrop's colour
  inline static std::uint32_t bar_u32 = 0xc80c0402u;
  // the accent for the places that draw with a fixed number: the same colour with another alpha, or darker
  static std::uint32_t accent_alpha(std::uint32_t color, std::uint32_t alpha) {
    return (alpha << 24) | (color & 0x00ffffffu);
  }
  static std::uint32_t accent_scaled(f32_t scale, std::uint32_t alpha = 0xff) {
    auto part = [&](int shift) { return (std::uint32_t)std::clamp((int)std::lround(((accent_u32 >> shift) & 0xff) * scale), 0, 255); };
    return (alpha << 24) | (part(16) << 16) | (part(8) << 8) | part(0);
  }

  // settings rhythm: a 4 pt half-step scaled with the text, on an 8 pt-like grid (4/8/12/16/24/32)
  static f32_t settings_unit() {
    return std::max(4.f, std::round(gui::get_font_size() * 0.25f));
  }
  static f32_t settings_space(f32_t steps) {
    return settings_unit() * steps;
  }

  // a settings section header: an icon in a tinted square, its name, and a line out to the edge
  static void settings_section_icon(icon_e icon, const char* title) {
    gui::dummy(fan::vec2(0, settings_space(3.f)));
    f32_t box = gui::get_frame_height() * 0.9f;
    fan::vec2 at = gui::get_cursor_screen_pos();
    auto* list = gui::get_window_draw_list();
    list->AddRectFilled(at, at + fan::vec2(box, box), accent_scaled(0.24f, 0x70), 5.f);
    draw_icon(list, icon, at + fan::vec2(box, box) * 0.5f, box * 0.62f, accent_u32);
    f32_t text_x = at.x + box + 10.f;
    list->AddText(fan::vec2(text_x, at.y + (box - gui::get_font_size()) * 0.5f),
      gui::widget_palette.accent_text, title);
    f32_t line_x = text_x + gui::calc_text_size(title).x + 10.f;
    f32_t right = at.x + gui::get_content_region_avail().x;
    if (right > line_x + 4.f) {
      f32_t y = at.y + box * 0.5f;
      list->AddLine(fan::vec2(line_x, y), fan::vec2(right, y), accent_alpha(accent_u32, 0x40), 1.f);
    }
    gui::dummy(fan::vec2(0, box + settings_space(2.f)));
  }

  // a dim line of help under a control: smaller, indented, closer to its row than to the next
  static void settings_hint(const std::string& text) {
    gui::font_scope_t small(gui::get_font_size() * 0.9f);
    gui::set_cursor_pos_x(gui::get_cursor_pos_x() + 10.f);
    fan::color dim = gui::palette_color(gui::widget_palette.label);
    dim.a *= 0.55f;
    gui::push_style_color(gui::col_text, dim);
    gui::push_text_wrap_pos(0.f);
    gui::text(text);
    gui::pop_text_wrap_pos();
    gui::pop_style_color();
    gui::dummy(fan::vec2(0, settings_space(1.f)));
  }

  // an amber line for a choice with a catch, quieter than an error
  static void settings_warning(const std::string& text) {
    gui::font_scope_t small(gui::get_font_size() * 0.9f);
    gui::set_cursor_pos_x(gui::get_cursor_pos_x() + 10.f);
    fan::color warm(0.75f, 0.35f, 0.08f, 1.f);
    gui::push_style_color(gui::col_text, warm);
    gui::push_text_wrap_pos(0.f);
    gui::text(text);
    gui::pop_text_wrap_pos();
    gui::pop_style_color();
    gui::dummy(fan::vec2(0, settings_space(1.f)));
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
    list->AddRectFilled(at, at + fan::vec2(width, height), hovered ? gui::widget_palette.surface_hover : gui::widget_palette.surface, 5.f);
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
    list->AddText(at + fan::vec2(32.f, (height - gui::get_font_size()) * 0.5f), hovered ? gui::widget_palette.text_hot : gui::widget_palette.text, title);
    return *open;
  }

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
  };
  theme_t theme;
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
    auto make = [](fan::color accent, f32_t brightness, f32_t tint, f32_t opacity, f32_t glow) {
      f32_t h, s, v;
      theme_rgb_to_hsv(std::pow(accent.r, 1.f / 2.2f), std::pow(accent.g, 1.f / 2.2f), std::pow(accent.b, 1.f / 2.2f), h, s, v);
      fan::color shown = theme_hsv_to_rgb(h, tint, 1.f);
      theme_t t;
      t.accent = accent;
      t.background = fan::color(std::pow(shown.r, 2.2f), std::pow(shown.g, 2.2f), std::pow(shown.b, 2.2f), 1.f);
      t.background_brightness = brightness;
      t.panel_opacity = opacity;
      t.glow = glow;
      return t;
    };
    static const std::vector<theme_preset_t> list = {
      {"Blue", make(fan::color(0.1f, 0.35f, 1.f, 1.f), 0.14f, 0.64f, 0.72f, 1.f)},
      {"Violet", make(lin(0.62f, 0.45f, 1.f), 0.15f, 0.60f, 0.72f, 1.f)},
      {"Emerald", make(lin(0.30f, 0.85f, 0.55f), 0.11f, 0.55f, 0.72f, 1.f)},
      {"Crimson", make(lin(1.f, 0.38f, 0.42f), 0.14f, 0.60f, 0.72f, 1.f)},
      {"Amber", make(lin(1.f, 0.72f, 0.28f), 0.13f, 0.55f, 0.72f, 1.f)},
      {"Graphite", make(lin(0.75f, 0.78f, 0.85f), 0.09f, 0.12f, 0.80f, 0.5f)},
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
  void apply_backdrop_color() {
    fan::color shown = theme_background_display();
    engine.set_clear_color(backdrop_linear ? fan::color(std::pow(shown.r, 2.2f), std::pow(shown.g, 2.2f), std::pow(shown.b, 2.2f), 1.f)
      : fan::color(shown.r, shown.g, shown.b, 1.f));
  }
  // after the theme changed: what draws with fixed numbers has to be told
  void apply_theme() {
    accent_u32 = theme_pack(theme.accent);
    // a little lighter for what the mouse is over
    accent_hover_u32 = theme_pack(fan::color(theme.accent.r + (1.f - theme.accent.r) * 0.13f, theme.accent.g + (1.f - theme.accent.g) * 0.13f,
      theme.accent.b + (1.f - theme.accent.b) * 0.13f, 1.f));
    apply_backdrop_color();
    apply_global_colors();
  }
  // the tab bars of docked windows, the title bars and the menu bar are drawn outside push_theme(), with the
  // style's own colours, so these are set there too
  void apply_global_colors() {
    const fan::color& A = theme.accent;
    fan::color shown_bg = theme_background_display();
    fan::color bg(std::pow(shown_bg.r, 2.2f), std::pow(shown_bg.g, 2.2f), std::pow(shown_bg.b, 2.2f), 1.f);
    auto set = [](gui::col_t index, fan::color c) {
      // the members one by one: the type's name isn't visible to the Linux build
      auto& target = gui::get_style().Colors[index];
      target.x = c.r;
      target.y = c.g;
      target.z = c.b;
      target.w = c.a;
    };
    fan::color blue_dim(A.r * 0.45f, A.g * 0.23f, A.b * 0.2f, 1.f);
    fan::color blue(A.r * 0.45f, A.g * 0.38f, A.b * 0.38f, 1.f);
    fan::color field_dim(bg.r * 3.f + A.r * 0.01f, bg.g * 3.f + A.g * 0.01f, bg.b * 3.f + A.b * 0.01f, std::max(theme.panel_opacity - 0.12f, 0.2f));
    set(gui::col_tab, field_dim);
    set(gui::col_tab_hovered, blue_dim);
    set(gui::col_tab_selected, blue);
    set(gui::col_tab_selected_overline, A);
    set(gui::col_tab_dimmed, field_dim);
    set(gui::col_tab_dimmed_selected, blue_dim);
    set(gui::col_menu_bar_bg, fan::color(bg.r * 1.6f, bg.g * 1.6f, bg.b * 1.6f, 1.f));
    set(gui::col_title_bg, fan::color(bg.r * 1.6f, bg.g * 1.6f, bg.b * 1.6f, 1.f));
    set(gui::col_title_bg_active, fan::color(bg.r * 2.2f, bg.g * 2.2f, bg.b * 2.2f, 1.f));
  }
  void save_theme() {
    fan::json saved = fan::json::object();
    fan::json accent = fan::json::array();
    accent.push_back((double)theme.accent.r);
    accent.push_back((double)theme.accent.g);
    accent.push_back((double)theme.accent.b);
    saved["accent"] = accent;
    saved["background_brightness"] = theme.background_brightness;
    fan::json background = fan::json::array();
    background.push_back((double)theme.background.r);
    background.push_back((double)theme.background.g);
    background.push_back((double)theme.background.b);
    saved["background"] = background;
    saved["panel_opacity"] = theme.panel_opacity;
    saved["glow"] = theme.glow;
    write_to_config("theme", saved);
  }
  void load_theme(const fan::json& saved) {
    if (!saved.is_object()) {
      return;
    }
    if (saved.contains("accent") && saved["accent"].is_array() && saved["accent"].size() == 3) {
      theme.accent = fan::color(std::clamp((f32_t)saved["accent"][0].get<double>(), 0.f, 1.f),
        std::clamp((f32_t)saved["accent"][1].get<double>(), 0.f, 1.f), std::clamp((f32_t)saved["accent"][2].get<double>(), 0.f, 1.f), 1.f);
    }
    if (saved.contains("background") && saved["background"].is_array() && saved["background"].size() == 3) {
      theme.background = fan::color(std::clamp((f32_t)saved["background"][0].get<double>(), 0.f, 1.f),
        std::clamp((f32_t)saved["background"][1].get<double>(), 0.f, 1.f), std::clamp((f32_t)saved["background"][2].get<double>(), 0.f, 1.f), 1.f);
    }
    else {
      // a theme saved before the backdrop had a colour of its own: the accent's hue, as it was
      f32_t h, s, v;
      theme_rgb_to_hsv(std::pow(theme.accent.r, 1.f / 2.2f), std::pow(theme.accent.g, 1.f / 2.2f), std::pow(theme.accent.b, 1.f / 2.2f), h, s, v);
      fan::color shown = theme_hsv_to_rgb(h, std::clamp((f32_t)saved.value("background_tint", 0.64) * s, 0.f, 1.f), 1.f);
      theme.background = fan::color(std::pow(shown.r, 2.2f), std::pow(shown.g, 2.2f), std::pow(shown.b, 2.2f), 1.f);
    }
    theme.background_brightness = std::clamp((f32_t)saved.value("background_brightness", (double)theme.background_brightness), 0.02f, 0.4f);
    theme.panel_opacity = std::clamp((f32_t)saved.value("panel_opacity", (double)theme.panel_opacity), 0.3f, 1.f);
    theme.glow = std::clamp((f32_t)saved.value("glow", (double)theme.glow), 0.f, 1.5f);
  }

  static std::uint32_t theme_pack(const fan::color& c) {
    auto byte = [](f32_t v) { return (std::uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255.f); };
    return 0xff000000u | (byte(c.b) << 16) | (byte(c.g) << 8) | byte(c.r);
  }

  // the Theme tab of the settings panel: a few ready-made themes, and the accent and four sliders to make one
  void render_theme_tab() {
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, 8.f));
    gui::push_style_var(gui::style_var_frame_rounding, 5.f);
    bool changed = false;

    gui::settings_section("THEMES");
    f32_t swatch = std::round(gui::get_frame_height() * 1.15f);
    const auto& presets = theme_presets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
      if (i > 0 && gui::get_content_region_avail().x >= swatch + 10.f) {
        gui::same_line();
      }
      fan::vec2 at = gui::get_cursor_screen_pos();
      gui::push_id((int)i);
      bool clicked = gui::invisible_button("##theme_preset", fan::vec2(swatch, swatch));
      gui::pop_id();
      bool hovered = gui::is_item_hovered();
      auto* list = gui::get_window_draw_list();
      fan::vec2 center = at + fan::vec2(swatch, swatch) * 0.5f;
      const auto& accent = presets[i].value.accent;
      bool selected = std::fabs(accent.r - theme.accent.r) < 0.002f && std::fabs(accent.g - theme.accent.g) < 0.002f &&
        std::fabs(accent.b - theme.accent.b) < 0.002f;
      list->AddCircleFilled(center, swatch * 0.38f, theme_pack(accent), 28);
      if (selected) {
        list->AddCircle(center, swatch * 0.5f - 1.f, 0xffffffffu, 28, 2.f);
      }
      else if (hovered) {
        list->AddCircle(center, swatch * 0.5f - 1.f, 0x90ffffffu, 28, 1.5f);
      }
      if (hovered) {
        gui::set_tooltip(presets[i].name);
      }
      if (clicked) {
        theme = presets[i].value;
        changed = true;
      }
    }

    gui::settings_section("CUSTOM");
    gui::settings_row("Accent", [&] {
      changed |= gui::color_edit3("##theme_accent", &theme.accent);
    });
    gui::settings_row("Background", [&] {
      changed |= gui::color_edit3("##theme_background", &theme.background);
    });
    gui::settings_row("Lightness", [&] {
      changed |= gui::slider("##theme_backdrop", &theme.background_brightness, 0.02f, 0.4f, gui::slider_flags_always_clamp);
    });
    gui::settings_row("Panels", [&] {
      changed |= gui::slider("##theme_panels", &theme.panel_opacity, 0.3f, 1.f, gui::slider_flags_always_clamp);
    });
    gui::settings_row("Glow", [&] {
      changed |= gui::slider("##theme_glow", &theme.glow, 0.f, 1.5f, gui::slider_flags_always_clamp);
    });
    gui::dummy(fan::vec2(0, 4.f));
    if (gui::button("Reset to Blue")) {
      theme = presets[0].value;
      changed = true;
    }

    if (changed) {
      apply_theme();
      theme_dirty = true;
    }
    // written once the mouse lets go, not on every step of a drag
    if (theme_dirty && !gui::get_io().MouseDown[0]) {
      save_theme();
      theme_dirty = false;
    }
    gui::pop_style_var(2);
  }

  void setup_backdrop() {
    // the window's background colour shows as a dark gray. Whether the clear colour is taken as linear or as it
    // looks depends on the renderer, the old value tells which
    fan::color current = gui::get_color(gui::col_window_bg);
    backdrop_linear = current.r < 0.05f;
    apply_backdrop_color();

    // one soft round spot, white, its alpha falling off smoothly to the edge; tinted when drawn
    const int n = 128;
    glow_pixels.assign((std::size_t)n * n * 4, 255);
    for (int y = 0; y < n; ++y) {
      for (int x = 0; x < n; ++x) {
        f32_t dx = (x + 0.5f) / n * 2.f - 1.f;
        f32_t dy = (y + 0.5f) / n * 2.f - 1.f;
        f32_t a = std::clamp(1.f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
        a = a * a * (3.f - 2.f * a);
        glow_pixels[((std::size_t)y * n + x) * 4 + 3] = (std::uint8_t)std::lround(a * 255.f);
      }
    }
    glow_image = engine.image_load(fan::image::info_t{glow_pixels.data(), fan::vec2i(n, n), 4}, {
      .min_filter = fan::graphics::image_filter_e::linear,
      .mag_filter = fan::graphics::image_filter_e::linear,
    });
  }

  // drawn into the full-window background window, before the main window
  void render_backdrop() {
    if (!glow_image.valid()) {
      return;
    }
    fan::vec2 display = gui::get_io().DisplaySize;
    fan::vec2 origin = gui::get_window_pos();
    f32_t t = std::chrono::duration<f32_t>(std::chrono::steady_clock::now() - backdrop_started).count();
    struct glow_t {
      f32_t x, y, radius, speed, phase;
      fan::color color;
    };
    // the lights are the accent's colour and two neighbours of it on the colour wheel, in the gui's colour space
    f32_t hue = theme_hue();
    auto light = [&](f32_t shift, f32_t alpha) {
      fan::color shown = theme_hsv_to_rgb(hue + shift, 0.95f * theme_saturation(), 1.f);
      return fan::color(std::pow(shown.r, 2.2f), std::pow(shown.g, 2.2f), std::pow(shown.b, 2.2f), alpha * theme.glow);
    };
    const glow_t glows[] = {
      {0.16f, 0.20f, 0.55f, 0.050f, 0.0f, light(0.f, 0.26f)},
      {0.84f, 0.28f, 0.50f, 0.040f, 2.1f, light(0.11f, 0.20f)},
      {0.62f, 0.86f, 0.60f, 0.035f, 4.2f, light(-0.04f, 0.18f)},
      {0.12f, 0.92f, 0.45f, 0.045f, 1.3f, light(0.015f, 0.16f)},
    };
    auto draw_glows = [&] {
      for (const auto& glow : glows) {
        f32_t a = t * glow.speed * 6.2831853f + glow.phase;
        fan::vec2 center(display.x * (glow.x + 0.09f * std::sin(a)), display.y * (glow.y + 0.09f * std::cos(a * 0.8f)));
        f32_t radius = std::max(display.x, display.y) * glow.radius * (1.f + 0.1f * std::sin(a * 1.3f));
        gui::set_cursor_screen_pos(origin + center - fan::vec2(radius, radius));
        gui::image(glow_image, fan::vec2(radius * 2.f, radius * 2.f), fan::vec2(0, 0), fan::vec2(1, 1), glow.color);
      }
    };
    if (!picture_shown) {
      draw_glows();
    }
    else {
      // around the picture only: above, below, left and right of it. On whole pixels, otherwise the bands meet
      // in a seam a pixel wide where the dark clear colour shows through
      fan::vec2 pmin(std::floor(picture_min.x), std::floor(picture_min.y));
      fan::vec2 pmax(std::ceil(picture_max.x), std::ceil(picture_max.y));
      fan::vec2 bands[4][2] = {
        {origin, fan::vec2(origin.x + display.x, pmin.y)},
        {fan::vec2(origin.x, pmax.y), origin + display},
        {fan::vec2(origin.x, pmin.y), fan::vec2(pmin.x, pmax.y)},
        {fan::vec2(pmax.x, pmin.y), fan::vec2(origin.x + display.x, pmax.y)},
      };
      for (auto& band : bands) {
        if (band[1].x - band[0].x < 1.f || band[1].y - band[0].y < 1.f) {
          continue;
        }
        gui::push_clip_rect(band[0], band[1], false);
        draw_glows();
        gui::pop_clip_rect();
      }
    }

    // the stream view is black around the picture (the picture itself is drawn under the gui, it is left uncovered)
    if (stream_area_shown) {
      auto* list = gui::get_window_draw_list();
      fan::vec2 a = stream_area_min;
      fan::vec2 b = stream_area_max;
      if (!picture_shown) {
        list->AddRectFilled(a, b, 0xff000000u);
      }
      else {
        fan::vec2 p0(std::clamp(std::floor(picture_min.x), a.x, b.x), std::clamp(std::floor(picture_min.y), a.y, b.y));
        fan::vec2 p1(std::clamp(std::ceil(picture_max.x), a.x, b.x), std::clamp(std::ceil(picture_max.y), a.y, b.y));
        list->AddRectFilled(a, fan::vec2(b.x, p0.y), 0xff000000u);
        list->AddRectFilled(fan::vec2(a.x, p1.y), b, 0xff000000u);
        list->AddRectFilled(fan::vec2(a.x, p0.y), fan::vec2(p0.x, p1.y), 0xff000000u);
        list->AddRectFilled(fan::vec2(p1.x, p0.y), fan::vec2(b.x, p1.y), 0xff000000u);
      }
    }
    gui::set_cursor_screen_pos(origin);
  }

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
    // every colour below is made from the accent and the backdrop's colour. With the default theme they come out as
    // the blues this panel always had
    const fan::color& A = theme.accent;
    fan::color shown_bg = theme_background_display();
    fan::color bg(std::pow(shown_bg.r, 2.2f), std::pow(shown_bg.g, 2.2f), std::pow(shown_bg.b, 2.2f), 1.f);
    f32_t opacity = theme.panel_opacity;
    auto toward_white = [&](f32_t amount, f32_t alpha) {
      return fan::color(A.r + (1.f - A.r) * amount, A.g + (1.f - A.g) * amount, A.b + (1.f - A.b) * amount, alpha);
    };
    fan::color field(bg.r * 3.f + A.r * 0.01f, bg.g * 3.f + A.g * 0.01f, bg.b * 3.f + A.b * 0.01f, opacity);
    fan::color field_hover(field.r * 2.5f, field.g * 2.5f, field.b * 2.5f, std::min(opacity + 0.08f, 1.f));
    fan::color field_dim(field.r, field.g, field.b, std::max(opacity - 0.12f, 0.2f));
    fan::color blue_dim(A.r * 0.45f, A.g * 0.23f, A.b * 0.2f, 1.f);
    fan::color blue(A.r * 0.45f, A.g * 0.38f, A.b * 0.38f, 1.f);
    fan::color blue_bright = A;
    fan::color line(bg.r * 3.f + 0.02f, bg.g * 3.f + 0.02f, bg.b * 3.f + 0.02f, 1.f);

    color(gui::col_frame_bg, field);
    color(gui::col_frame_bg_hovered, field_hover);
    color(gui::col_frame_bg_active, blue_dim);
    color(gui::col_button, fan::color(A.r * 0.25f, A.g * 0.1f, A.b * 0.08f, std::min(opacity + 0.03f, 1.f)));
    color(gui::col_button_hovered, blue_dim);
    color(gui::col_button_active, blue);
    color(gui::col_header, blue_dim);
    color(gui::col_header_hovered, blue);
    color(gui::col_header_active, fan::color(A.r * 0.8f, A.g * 0.51f, A.b * 0.5f, 1.f));
    color(gui::col_check_mark, blue_bright);
    color(gui::col_slider_grab, blue_bright);
    color(gui::col_slider_grab_active, toward_white(0.15f, 1.f));
    color(gui::col_tab, field_dim);
    color(gui::col_tab_hovered, blue_dim);
    color(gui::col_tab_selected, blue);
    color(gui::col_tab_selected_overline, blue_bright);
    color(gui::col_tab_dimmed, field_dim);
    color(gui::col_tab_dimmed_selected, blue_dim);
    color(gui::col_border, toward_white(0.28f, 0.22f));
    color(gui::col_child_bg, toward_white(0.45f, 0.045f));
    color(gui::col_separator, line);
    color(gui::col_separator_hovered, blue);
    color(gui::col_separator_active, blue_bright);
    color(gui::col_popup_bg, fan::color(bg.r * 2.f, bg.g * 2.f, bg.b * 2.f, 0.94f));
    color(gui::col_table_header_bg, fan::color(blue_dim.r * 0.3f, blue_dim.g * 0.3f, blue_dim.b * 0.3f, 0.7f));
    color(gui::col_table_border_strong, line);
    color(gui::col_table_border_light, fan::color(line.r * 0.6f, line.g * 0.6f, line.b * 0.6f, 1.f));
    color(gui::col_table_row_bg_alt, fan::color(1.f, 1.f, 1.f, 0.025f));
    color(gui::col_scrollbar_bg, fan::color(0.f, 0.f, 0.f, 0.f));
    color(gui::col_scrollbar_grab, fan::color(blue_dim.r * 0.9f, blue_dim.g * 0.9f, blue_dim.b * 0.9f, 0.8f));
    color(gui::col_scrollbar_grab_hovered, blue_dim);
    color(gui::col_scrollbar_grab_active, blue);
    color(gui::col_menu_bar_bg, fan::color(bg.r * 1.6f, bg.g * 1.6f, bg.b * 1.6f, 1.f));
    color(gui::col_title_bg, fan::color(bg.r * 1.6f, bg.g * 1.6f, bg.b * 1.6f, 1.f));
    color(gui::col_title_bg_active, fan::color(bg.r * 2.2f, bg.g * 2.2f, bg.b * 2.2f, 1.f));
    color(gui::col_text_selected_bg, fan::color(A.r, A.g, A.b, 0.35f));
    color(gui::col_resize_grip, fan::color(0.f, 0.f, 0.f, 0.f));

    var(gui::style_var_frame_rounding, 5.f);
    var(gui::style_var_child_rounding, 8.f);
    var(gui::style_var_popup_rounding, 8.f);
    // a rounder, wider handle on the sliders: the thin tall bar looked rough
    var(gui::style_var_grab_rounding, 9.f);
    var(gui::style_var_grab_min_size, 18.f);
    var(gui::style_var_tab_rounding, 5.f);
    var(gui::style_var_scrollbar_rounding, 6.f);
  }

  void pop_theme() {
    gui::pop_style_color(theme_colors_pushed);
    gui::pop_style_var(theme_vars_pushed);
  }

  // ---- the share preview's size ------------------------------------------------------------------------------
  // a part of the panel's width, changed with the handle at the picture's lower right corner and kept in the config
  f32_t preview_fraction = 1.f;
  bool preview_resizing = false;

  void draw_resizable_preview(fan::graphics::image_t image, fan::vec2 image_size) {
    if (image_size.x <= 0 || image_size.y <= 0) {
      return;
    }
    f32_t avail = std::max(gui::get_content_region_avail().x, 1.f);
    f32_t width = std::max(80.f, avail * preview_fraction);
    fan::vec2 size(width, width * image_size.y / image_size.x);
    fan::vec2 at = gui::get_cursor_screen_pos();
    gui::image(image, size);

    f32_t grip = 16.f;
    gui::set_cursor_screen_pos(at + size - fan::vec2(grip, grip));
    gui::invisible_button("##preview_resize", fan::vec2(grip, grip));
    bool hovered = gui::is_item_hovered();
    bool active = gui::is_item_active();
    if (hovered || active) {
      gui::set_mouse_cursor(gui::mouse_cursor_resize_nwse);
    }
    if (hovered && !active) {
      gui::set_tooltip("Drag to resize");
    }
    if (active) {
      preview_fraction = std::clamp(preview_fraction + gui::get_io().MouseDelta.x / avail, 0.25f, 1.f);
    }
    else if (preview_resizing) {
      save_ui_config();
    }
    preview_resizing = active;

    // three short diagonal lines in the corner, on a dark patch so they show on any picture
    auto* list = gui::get_window_draw_list();
    fan::vec2 corner = at + size;
    list->AddTriangleFilled(corner - fan::vec2(grip, 0), corner, corner - fan::vec2(0, grip), 0xa0000000u);
    std::uint32_t ink = hovered || active ? accent_hover_u32 : 0xffd0d0d0u;
    for (f32_t d : {4.f, 8.f, 12.f}) {
      list->AddLine(corner - fan::vec2(d, 2.f), corner - fan::vec2(2.f, d), ink, 1.5f);
    }
    gui::set_cursor_screen_pos(fan::vec2(at.x, at.y + size.y + gui::get_style().ItemSpacing.y));
    gui::dummy(fan::vec2(0, 0));
  }

  // ---- building blocks of the settings panel ---------------------------------------------------------------
  // (the gui is drawn in linear colour and shown through sRGB: small numbers here look much lighter on screen)

  // the accent for what is drawn by hand: a clear blue until the theme says otherwise (apply_theme)
  inline static std::uint32_t accent_u32 = 0xffff5919u;
  inline static std::uint32_t accent_hover_u32 = 0xffff7a33u;
  // the accent for the places that draw with a fixed number: the same colour with another alpha, or darker
  static std::uint32_t accent_alpha(std::uint32_t color, std::uint32_t alpha) {
    return (alpha << 24) | (color & 0x00ffffffu);
  }
  static std::uint32_t accent_scaled(f32_t scale, std::uint32_t alpha = 0xff) {
    auto part = [&](int shift) { return (std::uint32_t)std::clamp((int)std::lround(((accent_u32 >> shift) & 0xff) * scale), 0, 255); };
    return (alpha << 24) | (part(16) << 16) | (part(8) << 8) | part(0);
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

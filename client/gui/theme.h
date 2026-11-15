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

  void setup_backdrop() {
    // the window's background colour shows as a dark gray. Whether the clear colour is taken as linear or as it
    // looks depends on the renderer, the old value tells which
    fan::color current = gui::get_color(gui::col_window_bg);
    bool linear = current.r < 0.05f;
    auto shade = [linear](f32_t r, f32_t g, f32_t b) {
      return linear ? fan::color(std::pow(r, 2.2f), std::pow(g, 2.2f), std::pow(b, 2.2f), 1.f) : fan::color(r, g, b, 1.f);
    };
    engine.set_clear_color(shade(0.05f, 0.07f, 0.14f));

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
    static const glow_t glows[] = {
      {0.16f, 0.20f, 0.55f, 0.050f, 0.0f, fan::color(0.04f, 0.22f, 1.00f, 0.26f)},  // blue
      {0.84f, 0.28f, 0.50f, 0.040f, 2.1f, fan::color(0.30f, 0.06f, 0.95f, 0.20f)},  // violet
      {0.62f, 0.86f, 0.60f, 0.035f, 4.2f, fan::color(0.00f, 0.45f, 0.80f, 0.18f)},  // teal
      {0.12f, 0.92f, 0.45f, 0.045f, 1.3f, fan::color(0.10f, 0.12f, 0.85f, 0.16f)},  // deep blue
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
      // around the picture only: above, below, left and right of it
      fan::vec2 bands[4][2] = {
        {origin, fan::vec2(origin.x + display.x, picture_min.y)},
        {fan::vec2(origin.x, picture_max.y), origin + display},
        {fan::vec2(origin.x, picture_min.y), fan::vec2(picture_min.x, picture_max.y)},
        {fan::vec2(picture_max.x, picture_min.y), fan::vec2(origin.x + display.x, picture_max.y)},
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
        fan::vec2 p0(std::clamp(picture_min.x, a.x, b.x), std::clamp(picture_min.y, a.y, b.y));
        fan::vec2 p1(std::clamp(picture_max.x, a.x, b.x), std::clamp(picture_max.y, a.y, b.y));
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
    fan::color field(0.012f, 0.018f, 0.04f, 0.72f);
    fan::color field_hover(0.03f, 0.045f, 0.1f, 0.8f);
    fan::color blue_dim(0.045f, 0.08f, 0.2f, 1.f);
    fan::color blue(0.06f, 0.13f, 0.38f, 1.f);
    fan::color blue_bright(0.1f, 0.35f, 1.f, 1.f);
    fan::color line(0.028f, 0.031f, 0.045f, 1.f);

    color(gui::col_frame_bg, field);
    color(gui::col_frame_bg_hovered, field_hover);
    color(gui::col_frame_bg_active, blue_dim);
    color(gui::col_button, fan::color(0.025f, 0.035f, 0.08f, 0.75f));
    color(gui::col_button_hovered, blue_dim);
    color(gui::col_button_active, blue);
    color(gui::col_header, blue_dim);
    color(gui::col_header_hovered, blue);
    color(gui::col_header_active, fan::color(0.08f, 0.18f, 0.5f, 1.f));
    color(gui::col_check_mark, blue_bright);
    color(gui::col_slider_grab, blue_bright);
    color(gui::col_slider_grab_active, fan::color(0.2f, 0.5f, 1.f, 1.f));
    color(gui::col_tab, fan::color(0.012f, 0.018f, 0.04f, 0.6f));
    color(gui::col_tab_hovered, blue_dim);
    color(gui::col_tab_selected, blue);
    color(gui::col_tab_selected_overline, blue_bright);
    color(gui::col_tab_dimmed, fan::color(0.012f, 0.018f, 0.04f, 0.6f));
    color(gui::col_tab_dimmed_selected, blue_dim);
    color(gui::col_border, fan::color(0.35f, 0.55f, 1.f, 0.22f));
    color(gui::col_child_bg, fan::color(0.5f, 0.68f, 1.f, 0.045f));
    color(gui::col_separator, line);
    color(gui::col_separator_hovered, blue);
    color(gui::col_separator_active, blue_bright);
    color(gui::col_popup_bg, fan::color(0.006f, 0.009f, 0.022f, 0.94f));
    color(gui::col_table_header_bg, fan::color(0.015f, 0.025f, 0.06f, 0.7f));
    color(gui::col_table_border_strong, line);
    color(gui::col_table_border_light, fan::color(0.016f, 0.018f, 0.026f, 1.f));
    color(gui::col_table_row_bg_alt, fan::color(1.f, 1.f, 1.f, 0.025f));
    color(gui::col_scrollbar_bg, fan::color(0.f, 0.f, 0.f, 0.f));
    color(gui::col_scrollbar_grab, fan::color(0.05f, 0.08f, 0.18f, 0.8f));
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

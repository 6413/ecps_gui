// Theme implementation, out of line so a theme edit does not recompile the whole client.
#include "globals.h"


void render_thread_t::ecps_gui_t::apply_backdrop_color() {
    fan::color shown = theme_background_display();
    owner->engine.set_clear_color(backdrop_linear ? fan::color(std::pow(shown.r, 2.2f), std::pow(shown.g, 2.2f), std::pow(shown.b, 2.2f), 1.f)
      : fan::color(shown.r, shown.g, shown.b, 1.f));
  }

void render_thread_t::ecps_gui_t::apply_theme() {
    accent_u32 = theme_pack(theme.accent);
    // a little lighter for what the mouse is over
    accent_hover_u32 = theme_pack(fan::color(theme.accent.r + (1.f - theme.accent.r) * 0.13f, theme.accent.g + (1.f - theme.accent.g) * 0.13f,
      theme.accent.b + (1.f - theme.accent.b) * 0.13f, 1.f));
    apply_backdrop_color();
    apply_global_colors();
  }

void render_thread_t::ecps_gui_t::apply_global_colors() {
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

void render_thread_t::ecps_gui_t::save_theme() {
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

void render_thread_t::ecps_gui_t::save_saved_themes() {
    fan::json list = fan::json::array();
    for (const auto& entry : saved_themes) {
      list.push_back(entry.to_json());
    }
    write_to_config("saved_themes", list);
  }

void render_thread_t::ecps_gui_t::load_saved_themes(const fan::json& saved) {
    saved_themes.clear();
    if (!saved.is_array()) {
      return;
    }
    for (const auto& entry : saved) {
      if (entry.is_object()) {
        saved_themes.push_back(saved_theme_t::from_json(entry));
      }
    }
  }

void render_thread_t::ecps_gui_t::load_theme(const fan::json& saved) {
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

void render_thread_t::ecps_gui_t::render_theme_tab() {
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

    gui::settings_section("SAVED");
    gui::settings_row("Name", [&] {
      bool enter = gui::input_text("##save_theme_name", &theme_name_input, gui::input_text_flags_enter_returns_true);
      gui::same_line(0, 6.f);
      if (gui::button("Save##save_theme") || (enter && !theme_name_input.empty())) {
        std::string name = theme_name_input.empty()
          ? "Theme " + std::to_string(saved_themes.size() + 1) : theme_name_input;
        saved_themes.push_back({name, theme});
        save_saved_themes();
        theme_name_input.clear();
      }
    });
    for (std::size_t i = 0; i < saved_themes.size(); ++i) {
      gui::push_id((int)i);
      if (gui::button(saved_themes[i].name + "##saved_theme_use", fan::vec2(0, 0))) {
        theme = saved_themes[i].value;
        changed = true;
      }
      gui::same_line();
      if (gui::button("x##saved_theme_delete")) {
        saved_themes.erase(saved_themes.begin() + i);
        save_saved_themes();
        gui::pop_id();
        break;
      }
      gui::pop_id();
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

void render_thread_t::ecps_gui_t::setup_backdrop() {
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
    glow_image = owner->engine.image_load(fan::image::info_t{glow_pixels.data(), fan::vec2i(n, n), 4}, {
      .min_filter = fan::graphics::image_filter_e::linear,
      .mag_filter = fan::graphics::image_filter_e::linear,
    });
  }

void render_thread_t::ecps_gui_t::render_backdrop() {
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
    // while the stream view is up the whole window is black: no blue strips around the picture or the panels.
    // The picture is drawn under the gui and is left uncovered, and the side-panel handles are drawn later on top
    if (stream_area_shown) {
      gui::get_window_draw_list()->AddRectFilled(origin, origin + display, 0xff000000u);
    }
    else {
      draw_glows();
    }
    gui::set_cursor_screen_pos(origin);
  }

void render_thread_t::ecps_gui_t::push_theme() {
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

void render_thread_t::ecps_gui_t::pop_theme() {
    gui::pop_style_color(theme_colors_pushed);
    gui::pop_style_var(theme_vars_pushed);
  }

void render_thread_t::ecps_gui_t::draw_resizable_preview(fan::graphics::image_t image, fan::vec2 image_size) {
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

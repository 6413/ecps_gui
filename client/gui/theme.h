// Part of ecps_gui_t, included inside the struct by gui.h: the colours and the settings panel building blocks.

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

// Theme implementation, out of line so a theme edit does not recompile the whole client.
#include "gui_std.h" // precompiled, first
#include <fan/stb/stb_image.h> // GIF frames, the implementation is in fan.graphics.stb
#include "ecps_app.h"


void render_thread_t::ecps_gui_t::apply_backdrop_color() {
    if (backdrop_black) {
      owner->engine.set_clear_color(fan::color(0, 0, 0, 1));
      return;
    }
    fan::color shown = theme_background_display();
    owner->engine.set_clear_color(backdrop_linear ? fan::color(std::pow(shown.r, 2.2f), std::pow(shown.g, 2.2f), std::pow(shown.b, 2.2f), 1.f)
      : fan::color(shown.r, shown.g, shown.b, 1.f));
  }

void render_thread_t::ecps_gui_t::apply_theme() {
    accent_u32 = theme_pack(theme.accent);
    // a little lighter for what the mouse is over
    accent_hover_u32 = theme_pack(fan::color(theme.accent.r + (1.f - theme.accent.r) * 0.13f, theme.accent.g + (1.f - theme.accent.g) * 0.13f,
      theme.accent.b + (1.f - theme.accent.b) * 0.13f, 1.f));
    // the backdrop's colour as the gui draws it (linear)
    fan::color shown_bg = theme_background_display();
    fan::color bg(std::pow(shown_bg.r, 2.2f), std::pow(shown_bg.g, 2.2f), std::pow(shown_bg.b, 2.2f), 1.f);
    const fan::color& A = theme.accent;
    f32_t k = theme.contrast;
    auto mix = [](fan::color a, fan::color b, f32_t t) {
      return fan::color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.f);
    };
    auto scaled = [](fan::color c, f32_t s) { return fan::color(c.r * s, c.g * s, c.b * s, 1.f); };
    auto gray = [](f32_t v) { return fan::color(v, v, v, 1.f); };
    // what fan's widgets and the hand-drawn parts here draw with: everything from the accent and the backdrop, so a
    // theme reaches all of it. High contrast: darker surfaces, white text
    auto& p = gui::widget_palette;
    p.accent = accent_u32;
    p.accent_hover = accent_hover_u32;
    p.accent_text = theme_pack(mix(mix(A, gray(1.f), 0.4f), A, k));
    p.text = theme_pack(mix(gray(0.784f), gray(1.f), k));
    p.text_hot = 0xffffffffu;
    p.label = theme_pack(mix(fan::color(0.6f, 0.6f, 0.66f, 1.f), gray(0.9f), k));
    fan::color surface = mix(fan::color(bg.r * 3.f + A.r * 0.03f, bg.g * 3.f + A.g * 0.03f, bg.b * 3.f + A.b * 0.03f, 1.f), gray(0.f), k);
    p.surface = theme_pack(surface);
    p.surface_hover = theme_pack(mix(scaled(A, 0.22f), scaled(A, 0.32f), k));
    p.surface_held = theme_pack(scaled(A, 0.38f));
    p.accent_surface = theme_pack(mix(scaled(A, 0.16f), scaled(A, 0.22f), k));
    p.accent_surface_hover = theme_pack(mix(scaled(A, 0.29f), scaled(A, 0.4f), k));
    p.track = theme_pack(mix(gray(0.075f), gray(0.2f), k));
    p.track_hover = theme_pack(mix(gray(0.13f), gray(0.3f), k));
    bar_u32 = (theme_pack(mix(scaled(bg, 0.6f), gray(0.f), k)) & 0x00ffffffu) | (k > 0.5f ? 0xff000000u : 0xc8000000u);
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
    // the accent made darker as a whole: scaling its channels by different amounts turned other hues muddy
    fan::color blue_dim(A.r * 0.2f, A.g * 0.2f, A.b * 0.2f, 1.f);
    fan::color blue(A.r * 0.38f, A.g * 0.38f, A.b * 0.38f, 1.f);
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
    saved["contrast"] = theme.contrast;
    saved["background_image"] = background_image;
    saved["background_image_dim"] = background_image_dim;
    saved["text_outline"] = text_outline;
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
    theme.contrast = std::clamp((f32_t)saved.value("contrast", (double)theme.contrast), 0.f, 1.f);
    background_image = saved.value("background_image", std::string());
    background_image_dim = std::clamp((f32_t)saved.value("background_image_dim", 0.35), 0.f, 0.9f);
    text_outline = saved.value("text_outline", false);
  }

void render_thread_t::ecps_gui_t::render_theme_tab() {
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, settings_space(3.f)));
    gui::push_style_var(gui::style_var_frame_padding, fan::vec2(gui::get_style().FramePadding.x,
      std::max(gui::get_style().FramePadding.y, gui::get_font_size() * 0.22f)));
    gui::push_style_var(gui::style_var_frame_rounding, 5.f);
    bool changed = false;

    settings_section_icon(icon_e::palette, "THEMES");
    gui::indent(settings_space(2.f));
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

    gui::unindent(settings_space(2.f));
    settings_section_icon(icon_e::download, "SAVED");
    gui::indent(settings_space(2.f));
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

    gui::unindent(settings_space(2.f));
    settings_section_icon(icon_e::edit, "CUSTOM");
    gui::indent(settings_space(2.f));
    gui::settings_row("Accent", [&] {
      changed |= gui::color_edit3("##theme_accent", &theme.accent);
    });
    gui::settings_row("Background", [&] {
      changed |= gui::color_edit3("##theme_background", &theme.background);
    });
    gui::settings_row("Image", [&] {
      bool has_image = !background_image.empty();
      if (gui::button(has_image ? "Change...##theme_background_image" : "Choose...##theme_background_image")) {
        fan::graphics::open_file("png,jpg,jpeg,webp,bmp,gif,mp4,mov,webm,mkv,m4v", [this](std::string_view path) {
          std::lock_guard<std::mutex> lock(picked_background_mutex);
          picked_background = std::string(path);
          background_was_picked = true;
        });
      }
      if (has_image) {
        gui::same_line(0, 6.f);
        if (gui::button("Remove##theme_background_image")) {
          // background_image_loaded keeps the old path: emptied too, the two matched and the picture stayed up
          background_image.clear();
          refresh_background_image();
          save_theme();
        }
      }
      if (gui::is_item_hovered()) {
        gui::set_tooltip(has_image ? background_image : "A picture, GIF or video behind the panels");
      }
    });
    if (!background_image.empty()) {
      // how much of the picture shows: 100% is the file as it is, lower darkens it so the text reads better
      gui::settings_row("Picture strength", [&] {
        f32_t strength = 1.f - background_image_dim;
        if (gui::slider("##theme_background_strength", &strength, 0.1f, 1.f, gui::slider_flags_always_clamp)) {
          background_image_dim = std::clamp(1.f - strength, 0.f, 0.9f);
        }
        if (gui::is_item_deactivated_after_edit()) {
          save_theme();
        }
      });
      if (background_image_failed) {
        settings_warning("That picture could not be loaded");
      }
    }
    if (gui::settings_switch("Text outline", &text_outline, "A dark outline around text, readable over a bright picture or video")) {
      save_theme();
    }
    gui::settings_row("Lightness", [&] {
      changed |= gui::slider("##theme_backdrop", &theme.background_brightness, 0.02f, 0.4f, gui::slider_flags_always_clamp);
    });
    gui::settings_row("Panels", [&] {
      changed |= gui::slider("##theme_panels", &theme.panel_opacity, 0.3f, 1.f, gui::slider_flags_always_clamp);
    });
    gui::settings_row("Glow", [&] {
      changed |= gui::slider("##theme_glow", &theme.glow, 0.f, 1.5f, gui::slider_flags_always_clamp);
    });
    gui::settings_row("Contrast", [&] {
      changed |= gui::slider("##theme_contrast", &theme.contrast, 0.f, 1.f, gui::slider_flags_always_clamp);
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
    gui::unindent(settings_space(2.f));
    gui::pop_style_var(3);
  }

void render_thread_t::ecps_gui_t::show_background_frame(std::size_t index) {
    if (index >= background_frames.size()) {
      return;
    }
    auto& frame = background_frames[index];
    fan::image::info_t info;
    info.data = frame.data.get();
    info.size = fan::vec2i((int)frame.size.x, (int)frame.size.y);
    info.channels = frame.channels;
    if (background_image_texture.valid()) {
      background_image_texture.reload(info, background_image_properties());
    }
    else {
      background_image_texture = owner->engine.image_load(info, background_image_properties());
    }
  }

void render_thread_t::ecps_gui_t::load_gif_background(const std::string& path) {
    auto bytes = fan::io::file::read_binary(path);
    if (bytes.empty()) {
      background_image_failed = true;
      fan::print("background image: could not read", path);
      return;
    }
    if (bytes.size() > 64ull * 1024 * 1024) {
      background_image_failed = true;
      fan::print("background image: GIF file is too large:", path);
      return;
    }
    int* delays = nullptr;
    int width = 0, height = 0, frame_count = 0, channels = 0;
    unsigned char* data = ::stbi_load_gif_from_memory(bytes.data(), (int)bytes.size(), &delays,
      &width, &height, &frame_count, &channels, 4);
    if (!data || width <= 0 || height <= 0 || frame_count <= 0) {
      if (data) {
        ::stbi_image_free(data);
      }
      if (delays) {
        ::stbi_image_free(delays);
      }
      background_image_failed = true;
      fan::print("background image: could not decode the GIF:", path);
      return;
    }
    std::uint64_t frame_bytes = (std::uint64_t)width * height * 4;
    // a background is not worth hundreds of megabytes: keep the odd huge GIF from being copied frame by frame
    constexpr std::uint64_t max_frame_bytes = 64ull * 1024 * 1024;
    constexpr std::uint64_t max_total_bytes = 256ull * 1024 * 1024;
    if (width > 8192 || height > 8192 || frame_count > 1000 || frame_bytes > max_frame_bytes ||
      frame_bytes * (std::uint64_t)frame_count > max_total_bytes) {
      ::stbi_image_free(data);
      if (delays) {
        ::stbi_image_free(delays);
      }
      background_image_failed = true;
      fan::print("background image: GIF is too large:", path);
      return;
    }
    background_frames.clear();
    background_frame_delays_ms.clear();
    background_frames.reserve((std::size_t)frame_count);
    background_frame_delays_ms.reserve((std::size_t)frame_count);
    for (int i = 0; i < frame_count; ++i) {
      fan::image::owned_t frame;
      frame.size = fan::vec2ui((std::uint32_t)width, (std::uint32_t)height);
      frame.channels = 4;
      frame.data_size = (std::size_t)frame_bytes;
      frame.data = std::shared_ptr<std::uint8_t>(new std::uint8_t[frame.data_size], std::default_delete<std::uint8_t[]>());
      std::memcpy(frame.data.get(), data + (std::size_t)i * frame.data_size, frame.data_size);
      background_frames.push_back(std::move(frame));
      int delay = delays ? delays[i] : 100;
      background_frame_delays_ms.push_back(std::max(20, delay));
    }
    ::stbi_image_free(data);
    if (delays) {
      ::stbi_image_free(delays);
    }
    background_animated = true;
    background_frame_index = 0;
    background_frame_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(background_frame_delays_ms.front());
    show_background_frame(0);
  }

void render_thread_t::ecps_gui_t::load_video_background(const std::string& path) {
    auto video = std::make_unique<background_video_t>();
    if (!video->demuxer.open(path.c_str())) {
      background_image_failed = true;
      fan::print("background video: could not open", path);
      return;
    }
    if (!video->decoder.open()) {
      background_image_failed = true;
      fan::print("background video: no decoder for", path);
      return;
    }
    video->codec_name = video->demuxer.get_codec_name();
    fan::vec2ui source = video->demuxer.get_video_size();
    if (source.x == 0 || source.y == 0) {
      source = fan::vec2ui(1280, 720);
    }
    // As sharp as the window can show it (up to 4K): the video covers the window, a frame bigger than the window
    // would only be shrunk again. Never bigger than the file
    fan::vec2 display = gui::get_io().DisplaySize;
    fan::vec2 limit(std::clamp(display.x, 1280.f, 3840.f), std::clamp(display.y, 720.f, 2160.f));
    // cover: the side that fills the window decides, the other one is cropped
    f32_t scale = std::min(std::max(limit.x / source.x, limit.y / source.y), 1.f);
    scale = std::min({ scale, 3840.f / source.x, 2160.f / source.y });
    video->target_size = fan::vec2ui(std::max(2u, (std::uint32_t)std::lround(source.x * scale) & ~1u),
      std::max(2u, (std::uint32_t)std::lround(source.y * scale) & ~1u));
    f32_t source_fps = video->demuxer.get_fps();
    if (!(source_fps > 1.f && source_fps < 1000.f)) {
      source_fps = 30.f;
    }
    video->frame_step = std::max(1, (int)std::ceil(source_fps / 60.5f));
    video->fps = source_fps / video->frame_step;
    video->next_frame = std::chrono::steady_clock::now();
    background_video = std::move(video);
    background_video_t::rgba_frame_t first;
    bool decoded = background_video->decode_next(first);
    if (decoded) {
      upload_background_frame(first);
      fan::print("background video: " + std::to_string(background_video->target_size.x) + "x" +
        std::to_string(background_video->target_size.y) + " at " + std::to_string((int)std::lround(background_video->fps)) +
        " fps, decoder " + background_video->decoder.codec_name_ +
        (background_video->decoder.using_hardware_ ? " (graphics card)" : " (CPU)"));
      background_video->start_worker();
    }
    if (!decoded) {
      // which decoder gave nothing, so a failing file can be told apart (the graphics card can't do 10-bit H.264)
      fan::print("background video: decoder " + background_video->decoder.codec_name_ +
        (background_video->decoder.using_hardware_ ? " (graphics card)" : " (CPU)") + ", codec " + background_video->codec_name);
      background_video.reset();
      background_image_failed = true;
      fan::print("background video: could not decode a frame from", path);
    }
    else {
      // the first frame stays up for its own duration, the next one comes on the tick after it
      background_video->next_frame = std::chrono::steady_clock::now() +
        std::chrono::milliseconds((int)std::lround(1000.f / background_video->fps));
    }
  }

bool render_thread_t::ecps_gui_t::background_video_t::decode_next(rgba_frame_t& out) {
    auto& video = *this;
    auto read_packet = [&] {
      return video.demuxer.needs_annexb()
        ? video.demuxer.read_annexb_packet(video.packet, video.packet_pts)
        : video.demuxer.read_raw_packet(video.packet, video.packet_pts);
    };
    // skipped frames over 60 fps: decoded (the next ones need them) but not shown
    for (int skip = 1; skip < video.frame_step; ++skip) {
      for (int attempts = 0; attempts < 256 && video.frames.empty(); ++attempts) {
        if (!read_packet()) {
          break;
        }
        auto decoded = video.decoder.decode_packet(video.packet.data(), video.packet.size(), video.codec_name,
          true, video.target_size);
        video.frames.insert(video.frames.end(), std::make_move_iterator(decoded.begin()), std::make_move_iterator(decoded.end()));
      }
      if (!video.frames.empty()) {
        video.frames.erase(video.frames.begin());
      }
    }
    for (int attempts = 0; attempts < 256 && video.frames.empty(); ++attempts) {
      if (!read_packet()) {
        // loop: the background plays until another picture is chosen
        if (!video.demuxer.seek(0.f)) {
          return false;
        }
        video.decoder.flush();
        video.frames.clear();
        video.packet.clear();
        if (!read_packet()) {
          return false;
        }
      }
      auto decoded = video.decoder.decode_packet(video.packet.data(), video.packet.size(), video.codec_name,
        true, video.target_size);
      if (!decoded.empty()) {
        video.frames.insert(video.frames.end(), std::make_move_iterator(decoded.begin()), std::make_move_iterator(decoded.end()));
      }
    }
    if (video.frames.empty()) {
      return false;
    }
    auto& frame = video.frames.front();
    out.pixels.clear();
    if (frame.success && !frame.plane_data.empty()) {
      out.pixels = std::move(frame.plane_data[0]);
      out.size = frame.image_size;
    }
    video.frames.erase(video.frames.begin());
    return true;
  }

void render_thread_t::ecps_gui_t::background_video_t::start_worker() {
    worker = std::thread([this] {
      while (true) {
        {
          std::unique_lock<std::mutex> lock(mutex);
          // three frames ahead: enough to ride over a slow one, little memory even at 4K
          wake.wait(lock, [&] { return stop || ready.size() < 3; });
          if (stop) {
            return;
          }
        }
        rgba_frame_t frame;
        bool ok = decode_next(frame);
        std::lock_guard<std::mutex> lock(mutex);
        if (!ok) {
          failed = true;
          return;
        }
        if (!frame.pixels.empty()) {
          ready.push_back(std::move(frame));
        }
      }
    });
  }

void render_thread_t::ecps_gui_t::upload_background_frame(const background_video_t::rgba_frame_t& frame) {
    if (frame.pixels.empty() || frame.size.x == 0 || frame.size.y == 0) {
      return;
    }
    fan::image::info_t info;
    info.data = (void*)frame.pixels.data();
    info.size = fan::vec2i((int)frame.size.x, (int)frame.size.y);
    info.channels = 4;
    if (background_image_texture.valid()) {
      background_image_texture.reload(info, background_image_properties());
    }
    else {
      background_image_texture = owner->engine.image_load(info, background_image_properties());
    }
  }

void render_thread_t::ecps_gui_t::refresh_background_image() {
    if (background_image_loaded == background_image) {
      return;
    }
    if (background_image_texture.valid()) {
      owner->engine.image_unload(background_image_texture);
      background_image_texture = {};
    }
    background_animated = false;
    background_frames.clear();
    background_frame_delays_ms.clear();
    background_frame_index = 0;
    background_frame_at = {};
    background_video.reset();
    background_image_loaded = background_image;
    background_image_failed = false;
    if (background_image.empty()) {
      return;
    }
    std::string extension = std::filesystem::path(background_image).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
      [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".gif") {
      load_gif_background(background_image);
      return;
    }
    if (extension == ".mp4" || extension == ".mov" || extension == ".webm" || extension == ".mkv" || extension == ".m4v") {
      load_video_background(background_image);
      return;
    }
    auto loaded = ecps_load_image(background_image);
    if (!loaded) {
      background_image_failed = true;
      fan::print("background image:", loaded.error());
      return;
    }
    fan::image::info_t info;
    info.data = loaded->data.get();
    info.size = fan::vec2i((int)loaded->size.x, (int)loaded->size.y);
    info.channels = loaded->channels;
    background_image_texture = owner->engine.image_load(info, background_image_properties());
  }

void render_thread_t::ecps_gui_t::draw_background_image() {
    // no decoding while the window is in the background: the picture stays where it is and starts again when the
    // window comes back
    bool animate = owner->engine.window.is_focused();
    if (background_video && animate) {
      auto now = std::chrono::steady_clock::now();
      auto& video = *background_video;
      if (now >= video.next_frame) {
        std::optional<background_video_t::rgba_frame_t> frame;
        {
          std::lock_guard<std::mutex> lock(video.mutex);
          if (!video.ready.empty()) {
            frame = std::move(video.ready.front());
            video.ready.pop_front();
          }
        }
        if (frame) {
          video.wake.notify_one();
          upload_background_frame(*frame);
          // on the video's own clock; after a pause (window in the background) or a slow stretch it starts from now
          auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / video.fps));
          video.next_frame += interval;
          if (now - video.next_frame > std::chrono::milliseconds(250)) {
            video.next_frame = now + interval;
          }
        }
      }
    }
    else if (background_animated && animate && !background_frames.empty()) {
      auto now = std::chrono::steady_clock::now();
      if (now >= background_frame_at) {
        background_frame_index = (background_frame_index + 1) % background_frames.size();
        background_frame_at = now + std::chrono::milliseconds(background_frame_delays_ms[background_frame_index]);
        show_background_frame(background_frame_index);
      }
    }
    if (!background_image_texture.valid()) {
      return;
    }
    fan::vec2 display = gui::get_io().DisplaySize;
    fan::vec2 image_size = background_image_texture.get_size();
    if (display.x <= 0.f || display.y <= 0.f || image_size.x <= 0.f || image_size.y <= 0.f) {
      return;
    }
    // cover the window: the picture keeps its aspect and the sides that don't fit are cropped
    f32_t scale = std::max(display.x / image_size.x, display.y / image_size.y);
    fan::vec2 shown = image_size * scale;
    fan::vec2 crop = (shown - display) / shown * 0.5f;
    fan::vec2 origin = gui::get_window_pos();
    gui::set_cursor_screen_pos(origin);
    gui::image(background_image_texture, display, fan::vec2(crop.x, crop.y), fan::vec2(1.f - crop.x, 1.f - crop.y));
    f32_t dim = std::clamp(background_image_dim, 0.f, 0.9f);
    if (dim > 0.f) {
      auto* list = gui::get_window_draw_list();
      list->AddRectFilled(origin, origin + display, (std::uint32_t)std::lround(dim * 255.f) << 24);
    }
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
    {
      std::string picked;
      bool was_picked = false;
      {
        std::lock_guard<std::mutex> lock(picked_background_mutex);
        was_picked = std::exchange(background_was_picked, false);
        picked = std::move(picked_background);
        picked_background.clear();
      }
      if (was_picked) {
        background_image = std::move(picked);
        background_image_loaded.clear();
        refresh_background_image();
        save_theme();
      }
    }
    refresh_background_image();
    if (!glow_image.valid() && !background_image_texture.valid()) {
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
    auto draw_all = [&] {
      draw_background_image();
      // the lights are for the plain backdrop: over a picture they lightened it, it looked whitish next to the file
      if (glow_image.valid() && !background_image_texture.valid()) {
        draw_glows();
      }
    };
    // The background and the lights everywhere except the stream area (last frame's): drawn over it, they covered
    // the picture, which the engine draws under the whole gui. The stream area draws its own black bars beside it
    if (stream_area_shown) {
      fan::vec2 w0 = origin;
      fan::vec2 w1 = origin + display;
      fan::vec2 a = stream_area_min;
      fan::vec2 b = stream_area_max;
      const fan::vec2 parts[4][2] = {
        {w0, fan::vec2(w1.x, a.y)},
        {fan::vec2(w0.x, b.y), w1},
        {fan::vec2(w0.x, a.y), fan::vec2(a.x, b.y)},
        {fan::vec2(b.x, a.y), fan::vec2(w1.x, b.y)},
      };
      for (const auto& part : parts) {
        if (part[1].x > part[0].x && part[1].y > part[0].y) {
          gui::push_clip_rect(part[0], part[1], true);
          draw_all();
          gui::pop_clip_rect();
        }
      }
    }
    else {
      draw_all();
    }
    gui::set_cursor_screen_pos(origin);
  }

void render_thread_t::ecps_gui_t::push_theme() {
    theme_colors_pushed = 0;
    theme_vars_pushed = 0;
    gui::set_text_outline(text_outline ? 1.f : 0.f, fan::color(0.f, 0.f, 0.f, 0.85f));
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
    // the accent made darker as a whole (each channel alike, so every hue keeps its colour)
    fan::color blue_dim(A.r * 0.2f, A.g * 0.2f, A.b * 0.2f, 1.f);
    fan::color blue(A.r * 0.38f, A.g * 0.38f, A.b * 0.38f, 1.f);
    fan::color blue_bright = A;
    f32_t k = theme.contrast;
    fan::color line(bg.r * 3.f + 0.02f, bg.g * 3.f + 0.02f, bg.b * 3.f + 0.02f, 1.f);

    color(gui::col_frame_bg, field);
    color(gui::col_frame_bg_hovered, field_hover);
    color(gui::col_frame_bg_active, blue_dim);
    color(gui::col_button, fan::color(A.r * 0.1f, A.g * 0.1f, A.b * 0.1f, std::min(opacity + 0.03f, 1.f)));
    color(gui::col_button_hovered, blue_dim);
    color(gui::col_button_active, blue);
    color(gui::col_header, blue_dim);
    color(gui::col_header_hovered, blue);
    color(gui::col_header_active, fan::color(A.r * 0.5f, A.g * 0.5f, A.b * 0.5f, 1.f));
    color(gui::col_check_mark, blue_bright);
    color(gui::col_slider_grab, blue_bright);
    color(gui::col_slider_grab_active, toward_white(0.15f, 1.f));
    color(gui::col_tab, field_dim);
    color(gui::col_tab_hovered, blue_dim);
    color(gui::col_tab_selected, blue);
    color(gui::col_tab_selected_overline, blue_bright);
    color(gui::col_tab_dimmed, field_dim);
    color(gui::col_tab_dimmed_selected, blue_dim);
    color(gui::col_border, toward_white(0.28f, 0.22f + 0.78f * k));
    // the panels: a faint veil of the Background colour over the backdrop while they are see-through, turning into the background's own
    // colour (a little lighter) as Panels goes to 1. Made from the accent alone it showed grey over a black theme
    {
      f32_t solid = std::clamp((opacity - 0.72f) / 0.28f, 0.f, 1.f);
      // the veil is the Background colour lighter, as light as the backdrop allows (dark backdrop, dark veil). It was
      // made from the accent, so the accent coloured the panels and the background only the hidden backdrop
      fan::color veil_shown = theme_hsv_to_rgb(theme_hue(), theme_saturation() * 0.7f, std::min(0.45f, theme.background_brightness * 3.2f));
      fan::color veil(std::pow(veil_shown.r, 2.2f), std::pow(veil_shown.g, 2.2f), std::pow(veil_shown.b, 2.2f), 1.f);
      fan::color own(std::min(bg.r * 2.5f + A.r * 0.015f, 1.f), std::min(bg.g * 2.5f + A.g * 0.015f, 1.f), std::min(bg.b * 2.5f + A.b * 0.015f, 1.f), 1.f);
      color(gui::col_child_bg, fan::color(veil.r + (own.r - veil.r) * solid, veil.g + (own.g - veil.g) * solid,
        veil.b + (own.b - veil.b) * solid, 0.045f + 0.955f * solid));
    }
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
    // the rest of ImGui's own colours that default to blue: links, keyboard focus, drop targets, graphs
    color(gui::col_text_link, toward_white(0.4f, 1.f));
    color(gui::col_nav_cursor, blue_bright);
    color(gui::col_drag_drop_target, blue_bright);
    color(gui::col_docking_preview, fan::color(A.r, A.g, A.b, 0.5f));
    color(gui::col_resize_grip_hovered, blue);
    color(gui::col_resize_grip_active, blue_bright);
    color(gui::col_plot_lines, toward_white(0.3f, 1.f));
    color(gui::col_plot_histogram, blue_bright);
    color(gui::col_tab_dimmed_selected_overline, blue_dim);
    // high contrast: white text, the grey of disabled text much lighter
    if (k > 0.f) {
      color(gui::col_text, fan::color(1.f, 1.f, 1.f, 1.f));
      color(gui::col_text_disabled, fan::color(0.45f + 0.3f * k, 0.45f + 0.3f * k, 0.45f + 0.3f * k, 1.f));
    }

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

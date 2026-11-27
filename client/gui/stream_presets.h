// Part of ecps_gui_t, included inside the struct by gui.h: one pick that sets every stream option, and keeps the
// choice. A preset is only the starting point: changing any of its options by hand turns the row back to Custom.

  struct stream_preset_t {
    const char* name;
    const char* tip;
    int width;        // only when the shared source's size isn't known yet: the height is the preset's real choice
    int height;       // the short side: the width follows the source's shape, so nothing is stretched
    int fps;
    int bitrate_mode; // 0 automatic, 1 Mbit/s
    f32_t bitrate_mbps;
    int codec;        // codec_config_t::codec_type_e: 0 H.264, 1 HEVC, 2 AV1
    bool sharp_text;
    bool lossless;
    int scaling;      // codec_config_t::scaling_quality_e
  };

  // 0 is not a preset: it keeps whatever the rows below say. The rest set all of them together. Saved by name, so
  // the order can change
  inline static constexpr stream_preset_t stream_presets[] = {
    {"Custom", "Use the settings below as they are", 0, 0, 0, 0, 0.f, 0, false, false, 1},
    {"Low bandwidth", "720p, 30 fps, 2.5 Mbit/s, H.264", 1280, 720, 30, 1, 2.5f, 0, false, false, 1},
    {"Performance", "720p, 60 fps, 6 Mbit/s, H.264", 1280, 720, 60, 1, 6.f, 0, false, false, 1},
    {"Balanced", "1080p, 60 fps, automatic bitrate, H.264", 1920, 1080, 60, 0, 10.f, 0, false, false, 1},
    {"High quality", "1080p, 60 fps, 15 Mbit/s, HEVC", 1920, 1080, 60, 1, 15.f, 1, false, false, 3},
    {"AV1", "1080p, 60 fps, 12 Mbit/s, AV1", 1920, 1080, 60, 1, 12.f, 2, false, false, 3},
    {"Sharp text", "1080p, 60 fps, 20 Mbit/s, HEVC 4:4:4", 1920, 1080, 60, 1, 20.f, 1, true, false, 4},
    {"Lossless", "1080p, 60 fps, HEVC 4:4:4 at QP 0, very high bitrate", 1920, 1080, 60, 1, 15.f, 1, true, true, 4},
  };
  inline static constexpr int stream_preset_count = (int)(sizeof(stream_presets) / sizeof(stream_presets[0]));

  // the saved preset: by name, or the number older versions saved (their order, before Performance moved up)
  static int stream_preset_from_config(const fan::json& stream) {
    std::string name;
    if (stream.contains("preset_name") && stream["preset_name"].is_string()) {
      name = stream["preset_name"].get<std::string>();
    }
    else {
      static constexpr const char* old_order[] = {
        "Custom", "Low bandwidth", "Balanced", "High quality", "Sharp text", "AV1", "Lossless", "Performance"
      };
      int old_index = (int)stream.value("preset", 0);
      if (old_index > 0 && old_index < (int)std::size(old_order)) {
        name = old_order[old_index];
      }
    }
    for (int i = 1; i < stream_preset_count; ++i) {
      if (name == stream_presets[i].name) {
        return i;
      }
    }
    return 0;
  }

  // 4:4:4 (Sharp text, Lossless) is made only by NVIDIA's encoder: elsewhere the picture goes out in normal colour
  bool can_encode_444() {
    if (!encode_on_gpu) {
      return false;
    }
    for (const auto& name : gpu_encoders()) {
      if (name.find("nvenc") != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  // the shared source's size (screen, window or camera), landscape. 0 while not known
  fan::vec2i stream_source_size() {
    auto* rt = get_render_thread();
    if (!rt) {
      return {0, 0};
    }
    int x = (int)rt->screen_encoder.mdscr.Geometry.Resolution.x;
    int y = (int)rt->screen_encoder.mdscr.Geometry.Resolution.y;
    if (x < 160 || y < 120) {
      return {0, 0};
    }
    return {std::max(x, y), std::min(x, y)};
  }

  // the source the preset's size was last worked out for: when what is shared changes shape, the size follows
  fan::vec2i preset_sized_for{0, 0};

  // the preset's height with the source's own shape, never bigger than the source: a 21:9 screen stays 21:9, a
  // 720p window isn't blown up to 1080p. Even numbers, encoders want them
  fan::vec2i stream_preset_size(const stream_preset_t& preset, fan::vec2i source) {
    if (source.x <= 0 || source.y <= 0) {
      return {preset.width, preset.height};
    }
    f64_t scale = std::min(1.0, (f64_t)preset.height / source.y);
    int width = std::max(160, (int)std::lround(source.x * scale / 2.0) * 2);
    int height = std::max(120, (int)std::lround(source.y * scale / 2.0) * 2);
    return {width, height};
  }

  void apply_preset_size(const stream_preset_t& preset) {
    fan::vec2i source = stream_source_size();
    fan::vec2i size = stream_preset_size(preset, source);
    resolution_controls.apply_preset_resolution(size.x, size.y);
    preset_sized_for = source;
  }

  // a preset in use and the shared source changed size (another window, another monitor): the size follows it.
  // True when it changed something
  bool follow_preset_source() {
    if (stream_settings.preset <= 0 || stream_settings.preset >= stream_preset_count) {
      return false;
    }
    fan::vec2i source = stream_source_size();
    if (source.x <= 0 || source == preset_sized_for) {
      return false;
    }
    apply_preset_size(stream_presets[stream_settings.preset]);
    return true;
  }

  void apply_stream_preset(int index) {
    index = std::clamp(index, 0, stream_preset_count - 1);
    stream_settings.preset = index;
    if (index > 0) {
      const auto& preset = stream_presets[index];
      video_codec = preset.codec;
      sharp_text = preset.sharp_text;
      lossless = preset.lossless;
      stream_settings.framerate = preset.fps;
      stream_settings.bitrate_mode = preset.bitrate_mode;
      stream_settings.use_adaptive_bitrate = preset.bitrate_mode == 0;
      stream_settings.bitrate_mbps = preset.bitrate_mbps;
      stream_settings.selected_scaling_quality = preset.scaling;
      if (auto* rt = get_render_thread()) {
        auto& encoder = rt->screen_encoder;
        apply_preset_size(preset);
        encoder.config_.frame_rate = preset.fps;
        {
          std::lock_guard<std::mutex> lock(encoder.mutex);
          encoder.update_flags |= codec_update_e::frame_rate;
        }
        encoder.update_scaling_quality(static_cast<codec_config_t::scaling_quality_e>(preset.scaling));
        // turns the old lossless off (or the new one on), then the codec choice sets the rest
        apply_lossless();
        if (!preset.lossless) {
          apply_codec_choice();
        }
        encoder.encode_write_flags |= codec_update_e::force_keyframe;
      }
      save_ui_config();
    }
    // kept at once, not after the usual second of quiet: picking a preset is a finished choice
    if (get_render_thread()) {
      save_stream_settings_now();
    }
  }

  void render_stream_preset_row() {
    if (!get_render_thread()) {
      return;
    }
    bool has_444 = can_encode_444();
    int shown = std::clamp(stream_settings.preset, 0, stream_preset_count - 1);
    gui::settings_row("Preset", [&] {
      if (gui::begin_combo("##stream_preset", stream_presets[shown].name)) {
        for (int i = 0; i < stream_preset_count; ++i) {
          const auto& preset = stream_presets[i];
          bool usable = !preset.sharp_text || has_444;
          gui::push_id(i);
          gui::begin_disabled(!usable);
          if (gui::selectable(preset.name, i == shown)) {
            apply_stream_preset(i);
          }
          gui::end_disabled();
          gui::pop_id();
          if (!usable && gui::is_item_hovered(gui::hovered_flags_allow_when_disabled)) {
            gui::set_tooltip("Needs an NVIDIA graphics card with GPU encoding on");
          }
          if (i == shown) {
            gui::set_item_default_focus();
          }
        }
        gui::end_combo();
      }
    });
    shown = std::clamp(stream_settings.preset, 0, stream_preset_count - 1);
    if (shown == 0) {
      settings_hint("Custom: the rows below are used as they are");
    }
    else if (stream_presets[shown].sharp_text && !has_444) {
      settings_warning("4:4:4 needs an NVIDIA card with GPU encoding: sent in normal colour");
    }
    else {
      settings_hint(stream_presets[shown].tip);
    }
  }

// Part of ecps_gui_t, included inside the struct by gui.h: one pick that sets every stream option, and keeps the
// choice. A preset is only the starting point: changing any of its options by hand turns the row back to Custom.

  struct stream_preset_t {
    const char* name;
    const char* tip;
    int width;
    int height;
    int fps;
    int bitrate_mode; // 0 automatic, 1 Mbit/s
    f32_t bitrate_mbps;
    int codec;        // codec_config_t::codec_type_e: 0 H.264, 1 HEVC, 2 AV1
    bool sharp_text;
    bool lossless;
    int scaling;      // codec_config_t::scaling_quality_e
  };

  // 0 is not a preset: it keeps whatever the rows below say. The rest set all of them together
  inline static constexpr stream_preset_t stream_presets[] = {
    {"Custom", "Use the settings below as they are", 0, 0, 0, 0, 0.f, 0, false, false, 1},
    {"Low bandwidth", "720p, 30 fps, 2.5 Mbit/s, H.264", 1280, 720, 30, 1, 2.5f, 0, false, false, 1},
    {"Balanced", "1080p, 60 fps, automatic bitrate, H.264", 1920, 1080, 60, 0, 10.f, 0, false, false, 1},
    {"High quality", "1080p, 60 fps, 15 Mbit/s, HEVC", 1920, 1080, 60, 1, 15.f, 1, false, false, 3},
    {"Sharp text", "1080p, 60 fps, 20 Mbit/s, HEVC 4:4:4", 1920, 1080, 60, 1, 20.f, 1, true, false, 4},
    {"AV1", "1080p, 60 fps, 12 Mbit/s, AV1", 1920, 1080, 60, 1, 12.f, 2, false, false, 3},
    {"Lossless", "HEVC 4:4:4 at QP 0, very high bitrate", 1920, 1080, 60, 1, 15.f, 1, true, true, 4},
  };
  inline static constexpr int stream_preset_count = (int)(sizeof(stream_presets) / sizeof(stream_presets[0]));

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
        if (preset.width >= 160 && preset.height >= 120) {
          resolution_controls.apply_preset_resolution(preset.width, preset.height);
        }
        else {
          encoder.reset_to_auto_resolution();
          std::lock_guard<std::mutex> lock(encoder.mutex);
          encoder.update_flags |= codec_update_e::codec;
        }
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
    static const std::vector<const char*> names = [] {
      std::vector<const char*> list;
      for (const auto& preset : stream_presets) {
        list.push_back(preset.name);
      }
      return list;
    }();
    gui::settings_row("Preset", [&] {
      if (gui::combo("##stream_preset", &stream_settings.preset, names.data(), (int)names.size())) {
        apply_stream_preset(stream_settings.preset);
      }
    });
    int shown = std::clamp(stream_settings.preset, 0, stream_preset_count - 1);
    settings_hint(shown == 0 ? "Custom: the rows below are used as they are" : stream_presets[shown].tip);
  }

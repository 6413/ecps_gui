// Part of ecps_gui_t, included inside the struct by gui.h: encode and decode on the GPU or on the CPU.
  // ---- GPU or CPU ---------------------------------------------------------------------------------------------
  // Two simple choices in the settings panel. GPU encoding picks the first video encoder of the graphics card that
  // works (NVIDIA, AMD, or VAAPI on Linux), CPU encoding is x264. GPU decoding uses the card's video decoder when
  // one opens, CPU decoding is libavcodec's own. Which GPU encoders this computer can run is found once: the
  // encoder has to be in the bundled FFmpeg and its driver library has to load

  bool encode_on_gpu = true;
  bool decode_on_gpu = true;
  // the codec shared with: 0 H.264, 1 HEVC, 2 AV1 (codec_config_t's order). A card that can't encode it falls back to
  // H.264 by itself. Viewers find out from the stream which one it is
  int video_codec = 2;
  // colour at full resolution (4:4:4): sharp coloured text, for code and documents. H.264 and HEVC on an NVIDIA card
  bool sharp_text = false;

  // HEVC 4:4:4 at QP 0: no quality knob left, but hundreds of Mbit/s. For a LAN, or a very fast link
  bool lossless = false;

  void apply_lossless() {
    if (auto* rt = get_render_thread()) {
      if (lossless) {
        video_codec = 1;
        sharp_text = true;
        rt->screen_encoder.config_.codec = codec_config_t::H265;
        rt->screen_encoder.config_.chroma_444 = true;
        rt->screen_encoder.config_.rate_control = codec_config_t::CRF;
        rt->screen_encoder.config_.crf_value = 0;
        rt->screen_encoder.config_.bitrate = 1000000000;
      }
      else {
        rt->screen_encoder.config_.chroma_444 = sharp_text;
        rt->screen_encoder.config_.rate_control = codec_config_t::CBR;
        rt->screen_encoder.config_.crf_value = 23;
        rt->screen_encoder.config_.bitrate = stream_settings.bitrate_mode == 1
          ? (int)(stream_settings.bitrate_mbps * 1000000)
          : (int)dynamic_config_t::get_adaptive_bitrate();
      }
      {
        std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
        rt->screen_encoder.update_flags |= codec_update_e::codec;
      }
      rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
    }
  }

  void apply_codec_choice() {
    if (auto* rt = get_render_thread()) {
      rt->screen_encoder.config_.codec = (codec_config_t::codec_type_e)std::clamp(video_codec, 0, 2);
      rt->screen_encoder.config_.chroma_444 = sharp_text;
      {
        std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
        rt->screen_encoder.update_flags |= codec_update_e::codec;
      }
      rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
    }
  }
  bool video_choice_applied = false;
  std::vector<std::string> gpu_encoder_list;
  bool gpu_encoders_found = false;

  const std::vector<std::string>& gpu_encoders() {
    if (!gpu_encoders_found) {
      gpu_encoders_found = true;
      gpu_encoder_list = find_gpu_encoders(fan::graphics::screen_encode_t::get_encoders());
    }
    return gpu_encoder_list;
  }

  // the choices take effect: the encoder and the decoder open again with them
  void apply_encode_choice() {
    if (auto* rt = get_render_thread()) {
      rt->screen_encoder.config_.software_only = !encode_on_gpu;
      {
        std::lock_guard<std::mutex> lock(rt->screen_encoder.mutex);
        rt->screen_encoder.update_flags |= codec_update_e::codec;
      }
      rt->screen_encoder.encode_write_flags |= codec_update_e::force_keyframe;
    }
  }

  void apply_decode_choice() {
    if (auto* rt = get_render_thread()) {
      rt->screen_decoder.decoder_.allow_hardware_ = decode_on_gpu;
      {
        std::lock_guard<std::mutex> lock(rt->screen_decoder.mutex);
        rt->screen_decoder.update_flags |= codec_update_e::codec;
      }
      request_idr_reset();
    }
  }

  // the saved choices, before anything is encoded or decoded (both open their codec on first use)
  void apply_video_choice_once() {
    if (video_choice_applied) {
      return;
    }
    if (auto* rt = get_render_thread()) {
      rt->screen_encoder.config_.software_only = !encode_on_gpu;
      rt->screen_encoder.config_.codec = (codec_config_t::codec_type_e)std::clamp(video_codec, 0, 2);
      rt->screen_encoder.config_.chroma_444 = sharp_text;
      if (lossless) {
        video_codec = 1;
        sharp_text = true;
        rt->screen_encoder.config_.codec = codec_config_t::H265;
        rt->screen_encoder.config_.chroma_444 = true;
        rt->screen_encoder.config_.rate_control = codec_config_t::CRF;
        rt->screen_encoder.config_.crf_value = 0;
        rt->screen_encoder.config_.bitrate = 1000000000;
      }
      rt->screen_decoder.decoder_.allow_hardware_ = decode_on_gpu;
      video_choice_applied = true;
    }
  }

  // two choices side by side, the chosen one blue. The first can be unusable: dimmed, with the reason on hover
  static bool choice_pair(const char* id, bool* first, const char* first_label, const char* second_label,
    bool first_usable, const char* first_unusable_reason) {
    f32_t width = (gui::get_content_region_avail().x - 4.f) * 0.5f;
    bool changed = false;
    gui::push_id(id);
    for (int i = 0; i < 2; ++i) {
      bool selected = (i == 0) == *first;
      bool usable = i == 1 || first_usable;
      if (i == 1) {
        gui::same_line(0, 4.f);
      }
      if (selected) {
        gui::push_style_color(gui::col_button, fan::color(0.06f, 0.13f, 0.38f, 1.f));
        gui::push_style_color(gui::col_button_hovered, fan::color(0.08f, 0.18f, 0.5f, 1.f));
      }
      gui::begin_disabled(!usable);
      if (gui::button(i == 0 ? first_label : second_label, fan::vec2(width, 0)) && !selected) {
        *first = i == 0;
        changed = true;
      }
      gui::end_disabled();
      if (!usable && gui::is_item_hovered(gui::hovered_flags_allow_when_disabled)) {
        gui::set_tooltip(first_unusable_reason);
      }
      if (selected) {
        gui::pop_style_color(2);
      }
    }
    gui::pop_id();
    return changed;
  }

  static std::string join_names(const std::vector<std::string>& names) {
    std::string text;
    for (const auto& name : names) {
      // just the card's make: "NVIDIA", not "NVIDIA (h264_nvenc)"
      std::string make = name.substr(0, name.find(" ("));
      if (text.find(make) != std::string::npos) {
        continue;
      }
      text += (text.empty() ? "" : ", ") + make;
    }
    return text;
  }

  void render_video_choice() {
    auto* rt = get_render_thread();
    if (!rt) {
      return;
    }
    settings_section_icon(icon_e::chip, "VIDEO");
    gui::indent(settings_space(2.f));

    // GPU stays choosable even when none was found: the check can miss a driver, and the encoder falls back to the
    // CPU by itself when no GPU encoder opens. "In use" tells what it got
    const auto& gpus = gpu_encoders();
    bool gpu_encoding_found = !gpus.empty();
    gui::settings_row("Encode", [&] {
      if (choice_pair("##encode_on", &encode_on_gpu, "GPU", "CPU", true, "")) {
        apply_encode_choice();
        save_ui_config();
      }
    });
    // H.264 plays everywhere. HEVC and AV1 look better for the same bitrate, the viewers' cards have to decode them
    static const char* names[] = {"H.264", "HEVC", "AV1"};
    static const char* tips[] = {
      "Plays on every computer",
      "Sharper than H.264; viewers need a recent GPU",
      "Sharpest; viewers need AV1 (RTX 30+), you need RTX 40+ (else H.264)",
    };
    gui::settings_row("Codec", [&] {
      f32_t width = (gui::get_content_region_avail().x - 8.f) / 3.f;
      for (int i = 0; i < 3; ++i) {
        if (i > 0) {
          gui::same_line(0, 4.f);
        }
        bool selected = video_codec == i;
        if (selected) {
          gui::push_style_color(gui::col_button, fan::color(0.06f, 0.13f, 0.38f, 1.f));
          gui::push_style_color(gui::col_button_hovered, fan::color(0.08f, 0.18f, 0.5f, 1.f));
        }
        gui::push_id(i);
        gui::begin_disabled(lossless);
        if (gui::button(names[i], fan::vec2(width, 0)) && !selected) {
          video_codec = i;
          apply_codec_choice();
          save_ui_config();
        }
        gui::end_disabled();
        gui::pop_id();
        if (gui::is_item_hovered()) {
          gui::set_tooltip(tips[i]);
        }
        if (selected) {
          fan::vec2 min = gui::get_item_rect_min();
          fan::vec2 max = gui::get_item_rect_max();
          gui::get_window_draw_list()->AddRectFilled(
            fan::vec2(min.x + 5.f, max.y - 3.f), fan::vec2(max.x - 5.f, max.y - 1.f), accent_u32, 1.f);
          gui::pop_style_color(2);
        }
      }
    });
    if (lossless) {
      settings_hint("Lossless uses HEVC 4:4:4");
    }
    else {
      settings_hint(tips[video_codec]);
    }
    gui::begin_disabled(lossless);
    if (video_codec != 2 && gui::settings_switch("Sharp text", &sharp_text,
      "Keeps text sharp. Uses 25% more bitrate; needs HEVC 4:4:4 (RTX 20+)")) {
      apply_codec_choice();
      save_ui_config();
    }
    gui::end_disabled();
    if (ecps_backend.is_streaming_to_any_channel()) {
      std::string used = rt->screen_encoder.encoder_.get_codec_name();
      std::string codec_name = used.find("av1") != std::string::npos ? "AV1"
        : (used.find("265") != std::string::npos || used.find("hevc") != std::string::npos) ? "HEVC" : "H.264";
      bool used_gpu = used.find("nvenc") != std::string::npos || used.find("amf") != std::string::npos ||
        used.find("qsv") != std::string::npos;
      // the rows above already say both; a line only when the encoder fell back to something else
      if (codec_name != names[video_codec] || used_gpu != encode_on_gpu) {
        const char* where = used.find("nvenc") != std::string::npos ? "NVIDIA GPU"
          : used.find("amf") != std::string::npos ? "AMD GPU"
          : used.find("qsv") != std::string::npos ? "Intel GPU" : "CPU";
        settings_hint("Encoding " + codec_name + " on the " + where);
      }
    }
    else {
      settings_hint(gpu_encoding_found ? "GPU: " + join_names(gpus) : std::string("No GPU encoder, using CPU"));
    }

    auto& decoder = rt->screen_decoder.decoder_;
    bool gpu_decoding_usable = decoder.has_hardware_support();
    gui::settings_row("Decode", [&] {
      if (choice_pair("##decode_on", &decode_on_gpu, "GPU", "CPU", gpu_decoding_usable, "No GPU video decoder opened on this computer")) {
        apply_decode_choice();
        save_ui_config();
      }
    });
    if (ecps_backend.is_viewing_any_channel()) {
      settings_hint("Decoding on " + std::string(decoder.using_hardware_ ? decoder.get_hardware_type_string() : "CPU") +
        (rt->screen_decoder.zero_copy_active() ? " (zero copy)" : ""));
    }
    else if (!gpu_decoding_usable) {
      settings_hint("No GPU decoder, using CPU");
    }
    gui::unindent(settings_space(2.f));
  }

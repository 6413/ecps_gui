// Part of ecps_gui_t, included inside the struct by gui.h: choosing what to share on Linux, and its preview.
#if !defined(_WIN32)
  // ---- what is captured on Linux ------------------------------------------------------------------------------
  // Wayland lets only the desktop's own share dialog (the portal) pick a screen or a window, so that dialog is the
  // picker here: "Choose..." opens it before sharing, and the choice is kept, so Share and the shares after it use
  // it without asking again. The preview is made from the captured frames themselves, so it works on any desktop.
  // The encode thread does the capturing (see the share loop in ecps.cpp), these flags tell it what is wanted

  bool preview_own_share = false;
  std::atomic<bool> capture_armed {false};     // keep the capture open while not sharing, for the preview and the next share
  std::atomic<bool> capture_reselect {false};  // close the capture, the next read opens the dialog again
  std::atomic<bool> capture_preview_wanted {false};

  // the small picture, written by the encode thread, read here
  std::mutex capture_preview_mutex;
  std::vector<std::uint8_t> capture_preview_pixels;
  fan::vec2i capture_preview_size;
  std::atomic<std::uint64_t> capture_preview_seq {0};
  std::chrono::steady_clock::time_point capture_preview_at;

  fan::graphics::image_t capture_preview_image;
  fan::vec2 capture_preview_shown_size;
  std::uint64_t capture_preview_shown_seq = 0;

  // Called by the encode thread after a frame was read: a few times a second, a small copy of it for the preview.
  // The captured frame is BGRA (the encoder takes it as that too)
  void store_capture_preview(const std::uint8_t* frame, std::int64_t width, std::int64_t height, std::int64_t line_size) {
    if (!capture_preview_wanted.load() || !frame || width < 16 || height < 16) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - capture_preview_at < std::chrono::milliseconds(200)) {
      return;
    }
    capture_preview_at = now;
    if (line_size < width * 4) {
      line_size = width * 4;
    }
    std::int64_t step = std::max<std::int64_t>(1, width / 480);
    std::int64_t out_width = width / step;
    std::int64_t out_height = height / step;
    std::lock_guard<std::mutex> lock(capture_preview_mutex);
    capture_preview_pixels.resize((std::size_t)(out_width * out_height * 4));
    for (std::int64_t y = 0; y < out_height; ++y) {
      const std::uint8_t* row = frame + y * step * line_size;
      std::uint8_t* to = &capture_preview_pixels[(std::size_t)(y * out_width * 4)];
      for (std::int64_t x = 0; x < out_width; ++x, to += 4) {
        const std::uint8_t* from = row + x * step * 4;
        to[0] = srgb_to_linear[from[2]];
        to[1] = srgb_to_linear[from[1]];
        to[2] = srgb_to_linear[from[0]];
        to[3] = 255;
      }
    }
    capture_preview_size = fan::vec2i((int)out_width, (int)out_height);
    capture_preview_seq.fetch_add(1);
  }

  void draw_capture_preview(f32_t max_width) {
    std::uint64_t seq = capture_preview_seq.load();
    if (seq != capture_preview_shown_seq) {
      std::lock_guard<std::mutex> lock(capture_preview_mutex);
      if (!capture_preview_pixels.empty()) {
        fan::image::info_t image_info {capture_preview_pixels.data(), capture_preview_size, 4};
        if (capture_preview_image.valid()) {
          capture_preview_image.reload(image_info);
        }
        else {
          capture_preview_image = engine.image_load(image_info);
        }
        capture_preview_shown_size = fan::vec2((f32_t)capture_preview_size.x, (f32_t)capture_preview_size.y);
      }
      capture_preview_shown_seq = seq;
    }
    bool capturing = capture_armed.load() || ecps_backend.is_streaming_to_any_channel();
    if (!capturing || !capture_preview_image.valid() || capture_preview_shown_size.x <= 0) {
      text_wrapped_disabled(capturing ? "Waiting for the first picture..." : "Choose a screen or window to see it here.");
      return;
    }
    f32_t width = std::min(max_width, capture_preview_shown_size.x);
    gui::image(capture_preview_image, fan::vec2(width, width * capture_preview_shown_size.y / capture_preview_shown_size.x));
  }

  void render_capture_source() {
    bool sharing = ecps_backend.is_streaming_to_any_channel();
    bool chosen = capture_armed.load() || sharing;
    settings_row("Source", [&] {
      f32_t release_width = chosen && !sharing ? gui::get_frame_height() + gui::get_style().ItemSpacing.x : 0.f;
      if (gui::button(std::string(chosen ? "Change..." : "Choose...") + "##capture_source",
        fan::vec2(gui::get_content_region_avail().x - release_width, 0.f))) {
        if (chosen) {
          capture_reselect = true;
        }
        capture_armed = true;
      }
      if (gui::is_item_hovered()) {
        gui::set_tooltip("Pick a screen or window in the system dialog");
      }
      if (chosen && !sharing) {
        gui::same_line();
        if (icon_button("##release_capture", icon_e::stop, "Stop capturing", 0.f, icon_style_e::danger)) {
          capture_armed = false;
          capture_reselect = true;
        }
      }
    });
    settings_switch("Preview", &preview_own_share, "See what you share");
    capture_preview_wanted = preview_own_share;
    if (preview_own_share) {
      draw_capture_preview(gui::get_content_region_avail().x);
    }
  }
#endif

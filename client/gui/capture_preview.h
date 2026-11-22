// Part of ecps_gui_t, included inside the struct by gui.h: the share preview made from the captured frames.
  // ---- the preview from the frames that are shared ------------------------------------------------------------
  // While sharing, the preview is made from the very frames that go out (on Windows after the HDR tone mapping),
  // so it shows exactly what the viewers get. The encode thread makes a small copy a few times a second, about the
  // size the preview is drawn at, each pixel the average of the frame pixels it covers so text stays readable

  bool preview_own_share = false;
  f32_t hdr_exposure = 0.8f;                         // how bright a tone mapped HDR screen comes out (Windows)
  int preview_fps = 5;                               // how often the preview is updated
  std::atomic<bool> capture_preview_wanted {false};
  std::atomic<int> capture_preview_width {960};     // the width the preview is drawn at, the copy is made about that wide

  std::mutex capture_preview_mutex;
  std::vector<std::uint8_t> capture_preview_pixels;
  fan::vec2i capture_preview_size;
  std::atomic<std::uint64_t> capture_preview_seq {0};
  std::atomic<std::int64_t> capture_preview_stamp_ms {0};
  std::chrono::steady_clock::time_point capture_preview_at; // the encode thread's alone
  std::vector<std::uint8_t> capture_preview_back;           // the encode thread's alone, swapped in when filled

  fan::graphics::image_t capture_preview_image;
  fan::vec2 capture_preview_shown_size;
  std::uint64_t capture_preview_shown_seq = 0;

  static std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // the encode thread asks before it spends anything on a copy: true a few times a second while the preview shows
  bool capture_preview_due() {
    if (!capture_preview_wanted.load()) {
      return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - capture_preview_at < std::chrono::milliseconds(1000 / std::clamp(preview_fps, 1, 60))) {
      return false;
    }
    capture_preview_at = now;
    return true;
  }

  void store_capture_preview(const std::uint8_t* frame, std::int64_t width, std::int64_t height, std::int64_t line_size) {
    if (capture_preview_due()) {
      store_capture_preview_now(frame, width, height, line_size);
    }
  }

  // frame is BGRA, sRGB like any screen picture
  void store_capture_preview_now(const std::uint8_t* frame, std::int64_t width, std::int64_t height, std::int64_t line_size) {
    if (!frame || width < 16 || height < 16) {
      return;
    }
    if (line_size < width * 4) {
      line_size = width * 4;
    }
    // 1080p wide at most (a 1440p screen comes out 1920 x 1080, not halved to 720p), never bigger than the frame
    std::int64_t out_width = std::min<std::int64_t>(width, 1920);
    std::int64_t out_height = std::max<std::int64_t>(1, height * out_width / width);
    // each pixel averages 4 frame pixels spread over the area it covers: smooth enough for text, and a 4k frame
    // costs about as much as a small one (reading every pixel of it took long enough to slow the whole program)
    f64_t scale_x = (f64_t)width / out_width;
    f64_t scale_y = (f64_t)height / out_height;
    auto source_x = [&](std::int64_t x, f64_t at) { return std::min<std::int64_t>(width - 1, (std::int64_t)((x + at) * scale_x)) * 4; };
    auto source_y = [&](std::int64_t y, f64_t at) { return std::min<std::int64_t>(height - 1, (std::int64_t)((y + at) * scale_y)); };
    std::vector<std::uint8_t>& out = capture_preview_back;
    out.resize((std::size_t)(out_width * out_height * 4));
    for (std::int64_t y = 0; y < out_height; ++y) {
      const std::uint8_t* row_a = frame + source_y(y, 0.25) * line_size;
      const std::uint8_t* row_b = frame + source_y(y, 0.75) * line_size;
      std::uint8_t* to = &out[(std::size_t)(y * out_width * 4)];
      for (std::int64_t x = 0; x < out_width; ++x, to += 4) {
        std::int64_t xa = source_x(x, 0.25);
        std::int64_t xb = source_x(x, 0.75);
        std::uint32_t b = row_a[xa] + row_a[xb] + row_b[xa] + row_b[xb];
        std::uint32_t g = row_a[xa + 1] + row_a[xb + 1] + row_b[xa + 1] + row_b[xb + 1];
        std::uint32_t r = row_a[xa + 2] + row_a[xb + 2] + row_b[xa + 2] + row_b[xb + 2];
        // kept as sRGB: fan loads 4 channel pictures as sRGB textures and the GPU turns them linear when drawing.
        // Made linear here as well, the preview came out darker than the screen
        to[0] = (std::uint8_t)(r / 4);
        to[1] = (std::uint8_t)(g / 4);
        to[2] = (std::uint8_t)(b / 4);
        to[3] = 255;
      }
    }
    // the gui only waits for this swap, never for the work above
    {
      std::lock_guard<std::mutex> lock(capture_preview_mutex);
      capture_preview_pixels.swap(out);
      capture_preview_size = fan::vec2i((int)out_width, (int)out_height);
    }
    capture_preview_stamp_ms = steady_ms();
    capture_preview_seq.fetch_add(1);
  }

  // a copy came in the last second: the capture is running
  bool capture_preview_recent() const {
    return capture_preview_seq.load() != 0 && steady_ms() - capture_preview_stamp_ms.load() < 1000;
  }

  static fan::graphics::image_load_properties_t smooth_image() {
    return {.min_filter = fan::graphics::image_filter_e::linear, .mag_filter = fan::graphics::image_filter_e::linear};
  }

  // the preview's frame rate, under the preview switch
  void render_preview_fps() {
    gui::settings_row("Preview fps", [&] {
      gui::slider("##preview_fps", &preview_fps, 1, 60, gui::slider_flags_always_clamp);
      if (gui::is_item_hovered()) {
        gui::set_tooltip("How often the preview updates (costs more)");
      }
      if (gui::is_item_deactivated_after_edit()) {
        save_ui_config();
      }
    });
  }

  // the newest copy, false when there is none to draw
  bool draw_captured_preview() {
    capture_preview_width = std::max(128, (int)(gui::get_content_region_avail().x * preview_fraction));
    std::uint64_t seq = capture_preview_seq.load();
    std::unique_lock<std::mutex> lock(capture_preview_mutex, std::try_to_lock);
    if (seq != capture_preview_shown_seq && lock.owns_lock()) {
      if (!capture_preview_pixels.empty()) {
        fan::image::info_t image_info {capture_preview_pixels.data(), capture_preview_size, 4};
        if (capture_preview_image.valid()) {
          capture_preview_image.reload(image_info, smooth_image());
        }
        else {
          capture_preview_image = engine.image_load(image_info, smooth_image());
        }
        capture_preview_shown_size = fan::vec2((f32_t)capture_preview_size.x, (f32_t)capture_preview_size.y);
      }
      capture_preview_shown_seq = seq;
    }
    if (!capture_preview_image.valid() || capture_preview_shown_size.x <= 0) {
      return false;
    }
    draw_resizable_preview(capture_preview_image, capture_preview_shown_size);
    return true;
  }

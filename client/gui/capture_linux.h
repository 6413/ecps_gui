// Part of ecps_gui_t, included inside the struct by gui.h: choosing what to share on Linux, and its preview.
#if !defined(_WIN32)
  // ---- what is captured on Linux ------------------------------------------------------------------------------
  // Wayland lets only the desktop's own share dialog (the portal) pick a screen or a window, so that dialog is the
  // picker here: "Choose..." opens it before sharing, and the choice is kept, so Share and the shares after it use
  // it without asking again. The preview is made from the captured frames themselves, so it works on any desktop.
  // The encode thread does the capturing (see the share loop in ecps.cpp), these flags tell it what is wanted

  std::atomic<bool> capture_armed {false};     // keep the capture open while not sharing, for the preview and the next share
  std::atomic<bool> capture_reselect {false};  // close the capture, the next read opens the dialog again

  void render_capture_source() {
    bool sharing = ecps_backend.is_streaming_to_any_channel();
    bool chosen = capture_armed.load() || sharing;
    gui::settings_row("Source", [&] {
      f32_t release_width = chosen && !sharing ? gui::get_frame_height() + gui::get_style().ItemSpacing.x : 0.f;
      if (gui::button(std::string(chosen ? "Change..." : "Choose...") + "##capture_source",
        fan::vec2(gui::get_content_region_avail().x - release_width, 0.f))) {
        if (chosen) {
          capture_reselect = true;
        }
        capture_armed = true;
      }
      if (gui::is_item_hovered()) {
        gui::set_tooltip("Choose a screen or window");
      }
      if (chosen && !sharing) {
        gui::same_line();
        if (icon_button("##release_capture", icon_e::stop, "Stop capturing", 0.f, icon_style_e::danger)) {
          capture_armed = false;
          capture_reselect = true;
        }
      }
    });
    gui::settings_switch("Preview", &preview_own_share, "See what you share");
    capture_preview_wanted = preview_own_share;
    if (preview_own_share) {
      render_preview_fps();
      if (!chosen) {
        gui::text_wrapped_disabled("Choose a screen or window to see it here.");
      }
      else if (!draw_captured_preview()) {
        gui::text_wrapped_disabled("Waiting for the first picture...");
      }
    }
  }
#endif

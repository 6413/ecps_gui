  void render_camera_source() {
    if (gui::settings_switch("Share camera", &stream_settings.share_camera, "Show your camera instead of the screen")) {
      camera_devices = fan::graphics::get_camera_devices();
    }
    if (!stream_settings.share_camera) {
      return;
    }
    std::string preview = camera_devices.empty() ? "No camera found" : camera_devices.front().name;
    for (const auto& device : camera_devices) {
      if (device.id == stream_settings.camera_device) {
        preview = device.name;
      }
    }
    bool open = false;
    gui::settings_row("Camera", [&] {
      open = gui::begin_combo("##camera_device", preview);
      if (open) {
        if (!camera_combo_open) {
          camera_devices = fan::graphics::get_camera_devices();
        }
        for (const auto& device : camera_devices) {
          gui::push_id(device.id);
          if (gui::selectable(device.name, device.id == stream_settings.camera_device)) {
            stream_settings.camera_device = device.id;
            write_to_config("camera", fan::json{{"device", stream_settings.camera_device}});
          }
          gui::pop_id();
        }
        gui::end_combo();
      }
    });
    camera_combo_open = open;
  }

  void render_camera_tab() {
    gui::push_style_var(gui::style_var_item_spacing, fan::vec2(8.f, settings_space(3.f)));
    gui::push_style_var(gui::style_var_frame_padding, fan::vec2(gui::get_style().FramePadding.x,
      std::max(gui::get_style().FramePadding.y, gui::get_font_size() * 0.22f)));
    gui::push_style_var(gui::style_var_frame_rounding, 5.f);
    settings_section_icon(icon_e::camera, "CAMERA");
    gui::indent(settings_space(2.f));
    render_camera_source();
    settings_hint(stream_settings.share_camera
      ? "The camera takes the screen's place while you share"
      : "Turn it on to share your camera instead of the screen");
    gui::unindent(settings_space(2.f));
    gui::pop_style_var(3);
    gui::dummy(fan::vec2(0, 6.f));
  }

  std::vector<fan::graphics::camera_device_t> camera_devices;
  bool camera_combo_open = false;

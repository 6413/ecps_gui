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

  std::vector<fan::graphics::camera_device_t> camera_devices;
  bool camera_combo_open = false;

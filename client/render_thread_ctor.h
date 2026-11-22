// render_thread_t's constructor, compiled in the module (ecps_app.ixx). Compiled in a source that imports the
// module, building the class's member initializers from it crashed MSVC (C1001)

render_thread_t::render_thread_t() {
    window_icon = engine.image_load("icons/ecps_logo.png");

    request_precise_timers();
    // started again by the in-app update (it leaves this file for the new process): the window comes back without
    // taking the focus from what the user is doing. A start by hand shows it focused as usual
    {
      std::error_code ec;
      if (std::filesystem::exists(updater_t::restarted_file, ec)) {
        engine.window.set_focus_on_show(false);
      }
    }
    engine.set_window_name("ECPS - Every Communication Program Sucks");    // image_load is async now, so read the icon pixels synchronously for glfw
    if (auto icon = fan::image::load_owned("icons/ecps_logo.png"); icon.valid() && icon.channels == 4) {
      fan::image::info_t icon_info;
      icon_info.data = icon.data.get();
      icon_info.size = fan::vec2i(icon.size.x, icon.size.y);
      icon_info.channels = icon.channels;
      engine.set_window_icon(icon_info);
    }

    if (!screen_encoder.open()) {
      fan::print("Failed to open modern encoder");
    }
    screen_encoder.set_user_resolution(1280, 720);

    if (!screen_decoder.open()) {
      fan::print("Failed to open modern decoder");
    }
    screen_decoder.reload_codec_cb = [this]() {
      ecps_gui.request_idr_reset();
    };
  }

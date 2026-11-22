// The render thread and the gui in it (gui.h), on top of core_state.h. Compiled as the module ecps.app
// (ecps_app.ixx), which imports ecps.core: inside that module core_state.h is not read again
#pragma once

#ifndef ECPS_APP_MODULE
#include "core_state.h"
#endif

struct render_thread_t {
  render_thread_t();

  #include "gui.h"
  #undef engine
  engine_t engine;
  ecps_gui_t ecps_gui{*this};

  fan::graphics::image_t window_icon;

  fan::graphics::screen_encode_t screen_encoder;
  fan::graphics::screen_decode_t screen_decoder;

  std::atomic<bool> should_stop {false};

  fan::graphics::universal_image_renderer_t network_frame {{
      .position = fan::vec3(fan::vec2(0), 1),
      .size = gloco()->window.get_size() / 2,
  }};
  f32_t displayed_fps = 0.0f;

  fan::vec2ui host_mouse_pos;

  // The host's mouse pointer as the host reports it: where it is on the captured screen (0..1) and whether it is
  // showing at all. A game that hides the pointer hides it here too. Not set when the host is a Linux one,
  // there the picture itself has the pointer in it
  struct host_cursor_t {
    std::atomic<f32_t> x {0.f};
    std::atomic<f32_t> y {0.f};
    std::atomic<bool> visible {false};
    std::atomic<std::int64_t> updated_ms {0};
  }host_cursor;

  void render(auto l) {
    if (engine.process_frame([this, l] { ecps_gui.render(); l(); })) {
      should_stop.store(true);
      stop_worker_threads();
      std::exit(0);
    }
  }

  latest_frame_t latest_frame;

};


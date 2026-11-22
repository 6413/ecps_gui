// The client's own macros. A module (ecps_app.ixx) passes no macros on, so the files that import it include this
// themselves, and gui_state.h includes it for the module
#pragma once

// 0 prints nothing extra. 1 adds the share/view status lines (what the capture, encoder and network are
// doing, every few seconds). Higher levels print per packet. Set it with -Decps_debug_prints=1
#ifndef ecps_debug_prints
  #define ecps_debug_prints 0
#endif

// fan::print_throttled_format is currently a no-op in fan, so throttle here:
// at most one line per call site every 250 ms
#define ecps_print_throttled(...) do { \
    static std::chrono::steady_clock::time_point ecps_last_print_{}; \
    auto ecps_now_ = std::chrono::steady_clock::now(); \
    if (ecps_now_ - ecps_last_print_ > std::chrono::milliseconds(250)) { \
      ecps_last_print_ = ecps_now_; \
      fan::print(fan::format(__VA_ARGS__)); \
    } \
  } while (0)

#define render_thread get_render_thread()

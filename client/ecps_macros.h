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

// "flick:" lines: what a viewer's picture and the buttons over it go through (keyframes asked and sent, the
// watched sharer changing, frames held back or lost, the picture cleared or stalled, the toolbar shown and hidden),
// to find why it flickers. Each line has the time in seconds; each call site says at most 10 lines a second and how
// many more it had. Off with -Decps_flicker_debug=0
#ifndef ecps_flicker_debug
  #define ecps_flicker_debug 1
#endif
#if ecps_flicker_debug
  #define ecps_flick(...) do { \
      static std::chrono::steady_clock::time_point ecps_flick_window_{}; \
      static int ecps_flick_count_ = 0; \
      static int ecps_flick_skipped_ = 0; \
      auto ecps_flick_now_ = std::chrono::steady_clock::now(); \
      double ecps_flick_t_ = std::chrono::duration<double>(ecps_flick_now_.time_since_epoch()).count(); \
      ecps_flick_t_ -= (double)((long long)(ecps_flick_t_ / 10000.0)) * 10000.0; \
      if (ecps_flick_now_ - ecps_flick_window_ >= std::chrono::seconds(1)) { \
        if (ecps_flick_skipped_ > 0) { \
          fan::print(fan::format("flick: {:.3f} ({} more lines like the next were left out)", ecps_flick_t_, ecps_flick_skipped_)); \
        } \
        ecps_flick_window_ = ecps_flick_now_; \
        ecps_flick_count_ = 0; \
        ecps_flick_skipped_ = 0; \
      } \
      if (++ecps_flick_count_ <= 10) { \
        fan::print(fan::format("flick: {:.3f} ", ecps_flick_t_) + fan::format(__VA_ARGS__)); \
      } \
      else { \
        ++ecps_flick_skipped_; \
      } \
    } while (0)
#else
  #define ecps_flick(...) do {} while (0)
#endif

#define render_thread get_render_thread()

// Errors as values. The network, file and protocol code still throws in places, so these two wrappers catch at the
// coroutine boundary once: every call site gets a std::expected and checks it instead of writing a try/catch.
#pragma once

#include <expected>
#include <string>
#include <exception>

inline std::string ecps_exception_text(const fan::exception_t& e) {
  return e.reason ? std::string(e.reason) : std::string("unknown error");
}

inline std::string ecps_exception_text(const std::exception& e) {
  return e.what();
}

template <typename T>
fan::event::runv_t<std::expected<T, std::string>> ecps_expected(fan::event::runv_t<T> task) {
  try {
    co_return co_await task;
  }
  catch (const fan::exception_t& e) {
    co_return std::unexpected(ecps_exception_text(e));
  }
  catch (const std::exception& e) {
    co_return std::unexpected(ecps_exception_text(e));
  }
  catch (...) {
    co_return std::unexpected(std::string("unknown error"));
  }
}

inline fan::event::runv_t<std::expected<void, std::string>> ecps_expected(fan::event::run_t task) {
  try {
    co_await task;
    co_return std::expected<void, std::string>{};
  }
  catch (const fan::exception_t& e) {
    co_return std::unexpected(ecps_exception_text(e));
  }
  catch (const std::exception& e) {
    co_return std::unexpected(ecps_exception_text(e));
  }
  catch (...) {
    co_return std::unexpected(std::string("unknown error"));
  }
}

// fan::json::parse throws on bad input; resume state, configs and server answers can all be bad
inline std::expected<fan::json, std::string> ecps_parse_json(const std::string& text) {
  try {
    return fan::json::parse(text);
  }
  catch (const std::exception& e) {
    return std::unexpected(ecps_exception_text(e));
  }
  catch (...) {
    return std::unexpected(std::string("invalid json"));
  }
}

// an image file that is missing or not an image
inline std::expected<fan::image::owned_t, std::string> ecps_load_image(const std::string& path) {
  try {
    return fan::image::load_owned(path);
  }
  catch (const std::exception& e) {
    return std::unexpected(ecps_exception_text(e));
  }
  catch (...) {
    return std::unexpected(std::string("could not load the picture"));
  }
}

// code that runs in one go (decoding, uploading, audio, pasting) and may throw
template <typename F>
auto ecps_expected_sync(F&& f) -> std::expected<decltype(f()), std::string> {
  using result_t = decltype(f());
  try {
    if constexpr (std::is_void_v<result_t>) {
      f();
      return {};
    }
    else {
      return f();
    }
  }
  catch (const fan::exception_t& e) {
    return std::unexpected(ecps_exception_text(e));
  }
  catch (const std::exception& e) {
    return std::unexpected(ecps_exception_text(e));
  }
  catch (...) {
    return std::unexpected(std::string("unknown error"));
  }
}

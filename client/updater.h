#pragma once

// "Check for updates" and, on Linux, "Update".
//  - the check downloads client/version.h from the main branch with curl and compares the number. It only
//    runs when the button is pressed, nothing is sent but the request for that one file
//  - updating (Linux, the Nix package) can't replace the binary, it is in the read-only store. Instead
//    `nix build` of the flake runs in the background, and when it finishes the client re-executes the new
//    build. Progress is written to ecps_update.log next to the settings
//  - Windows only tells that a newer version exists

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
  #include <unistd.h>
#endif

#include "version.h"
#include "log_capture.h"

struct updater_t {
  enum class state_e : std::uint8_t { idle, checking, up_to_date, available, failed, building, ready, build_failed };

  static constexpr const char* version_url = "https://raw.githubusercontent.com/6413/ecps_gui/main/client/version.h";
  static constexpr const char* flake_ref = "github:6413/ecps_gui#ecps";
  static constexpr const char* ready_file = "ecps_update.ready";
  static constexpr const char* failed_file = "ecps_update.failed";
  static constexpr const char* log_file = "ecps_update.log";

  static constexpr bool can_install =
#if defined(_WIN32)
    false;
#else
    true;
#endif

  std::atomic<state_e> state{state_e::idle};

  std::string latest() {
    std::lock_guard<std::mutex> lock(mutex);
    return latest_version;
  }

  std::string error() {
    std::lock_guard<std::mutex> lock(mutex);
    return error_text;
  }

  void check() {
    state_e current = state.load();
    if (current == state_e::checking || current == state_e::building) {
      return;
    }
    state = state_e::checking;
    std::thread([this] {
      std::string body = run_capture(std::string("curl -fsSL --max-time 10 ") + version_url);
      std::string found = parse_version(body);
      std::lock_guard<std::mutex> lock(mutex);
      if (found.empty()) {
        error_text = "could not read the latest version, is curl installed and the network up?";
        state = state_e::failed;
        return;
      }
      latest_version = found;
      state = is_newer(found, ecps_version) ? state_e::available : state_e::up_to_date;
    }).detach();
  }

  // Linux: build the new release in the background
  void start_update() {
    if (!can_install || state.load() != state_e::available) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(ready_file, ec);
    std::filesystem::remove(failed_file, ec);
    state = state_e::building;
#if !defined(_WIN32)
    std::string command =
      "nohup sh -c 'out=$(nix --extra-experimental-features \"nix-command flakes\" build --refresh --no-link "
      "--print-out-paths \"" + std::string(flake_ref) + "\" 2>>" + log_file + ") "
      "&& printf \"%s\" \"$out\" > " + ready_file + " || : > " + failed_file + "' >/dev/null 2>&1 &";
    if (std::system(command.c_str()) != 0) {
      std::lock_guard<std::mutex> lock(mutex);
      error_text = "could not start nix";
      state = state_e::build_failed;
    }
#endif
  }

  // GUI thread, every frame: cheap, looks at the two files once a second while a build runs
  void tick() {
    if (state.load() != state_e::building) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_poll < std::chrono::seconds(1)) {
      return;
    }
    last_poll = now;
    std::error_code ec;
    if (std::filesystem::exists(failed_file, ec)) {
      std::lock_guard<std::mutex> lock(mutex);
      error_text = std::string("nix build failed, see ") + log_file;
      state = state_e::build_failed;
    }
    else if (std::filesystem::exists(ready_file, ec)) {
      std::ifstream in(ready_file);
      std::string path;
      std::getline(in, path);
      while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ')) {
        path.pop_back();
      }
      std::lock_guard<std::mutex> lock(mutex);
      if (path.empty() || !std::filesystem::exists(path + "/bin/ecps", ec)) {
        error_text = std::string("the build finished but its result is missing, see ") + log_file;
        state = state_e::build_failed;
      }
      else {
        built_path = path;
        state = state_e::ready;
      }
    }
  }

  // replaces this process with the new build. Only returns when that fails
  void restart() {
#if !defined(_WIN32)
    std::string binary;
    {
      std::lock_guard<std::mutex> lock(mutex);
      binary = built_path + "/bin/ecps";
    }
    // the new process must not inherit our capture pipes, nobody would read them
    log_capture().restore();
    execl(binary.c_str(), binary.c_str(), (char*)nullptr);
    std::lock_guard<std::mutex> lock(mutex);
    error_text = "could not start the new version";
    state = state_e::build_failed;
#endif
  }

private:
  static std::string run_capture(const std::string& command) {
    std::string output;
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) {
      return output;
    }
    char buffer[512];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0 && output.size() < (1 << 16)) {
      output.append(buffer, n);
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
  }

  // ecps_version = "0.97";
  static std::string parse_version(const std::string& body) {
    std::size_t at = body.find("ecps_version");
    if (at == std::string::npos) {
      return {};
    }
    std::size_t open = body.find('"', at);
    if (open == std::string::npos) {
      return {};
    }
    std::size_t close = body.find('"', open + 1);
    if (close == std::string::npos || close - open > 32) {
      return {};
    }
    std::string value = body.substr(open + 1, close - open - 1);
    for (char c : value) {
      if (!((c >= '0' && c <= '9') || c == '.')) {
        return {};
      }
    }
    return value;
  }

  static std::vector<int> numbers(const std::string& version) {
    std::vector<int> parts;
    std::size_t start = 0;
    while (start <= version.size()) {
      std::size_t dot = version.find('.', start);
      std::string piece = version.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
      parts.push_back(piece.empty() ? 0 : std::atoi(piece.c_str()));
      if (dot == std::string::npos) {
        break;
      }
      start = dot + 1;
    }
    return parts;
  }

  static bool is_newer(const std::string& candidate, const std::string& current) {
    auto a = numbers(candidate);
    auto b = numbers(current);
    std::size_t length = std::max(a.size(), b.size());
    a.resize(length);
    b.resize(length);
    return a > b;
  }

  std::mutex mutex;
  std::string latest_version;
  std::string error_text;
  std::string built_path;
  std::chrono::steady_clock::time_point last_poll;
};

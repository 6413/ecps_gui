#pragma once

// "Check for updates" and, on Linux, "Update".
//  - the check only runs when the button is pressed. A Nix build knows the ecps_gui and fan commits it was
//    made from (ECPS_BUILD_INFO) and asks GitHub for the newest commit of each, so any push to either one
//    is an update. Without that (Windows, a dev build) it downloads client/version.h from the main branch
//    and compares the number. Nothing is sent but those requests
//  - updating (Linux, the Nix package) can't replace the binary, it is in the read-only store. Instead
//    `nix build` of the flake runs in the background, and when it finishes the client re-executes the new
//    build, with the newest fan, by itself a second or so after it finishes, no reopening. The result is linked as
//    ecps_update_result next to the settings, which keeps the build from being garbage collected.
//    Progress is written to ecps_update.log
//  - Windows only tells that a newer version exists

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
  // written just before the restart, the new process reads it to say it was updated and from what
  static constexpr const char* restarted_file = "ecps_update.restarted";
  // where the session was before the update, written only when the restart comes from the Update button
  static constexpr const char* state_file = "ecps_update.state";

  // set by the UI: JSON of what the new process should pick up again (channels, tab, switches)
  std::function<std::string()> collect_state;

  static constexpr bool can_install =
#if defined(_WIN32)
    false;
#else
    true;
#endif

  std::atomic<state_e> state{state_e::idle};

  // A package that is not the Nix one (the Arch PKGBUILD writes "installer=pacman" into build-info) is updated by its
  // own package manager: the commit check still runs, "Update now" (a nix build) is not offered
  static std::string installer() {
    return build_info("installer");
  }
  static bool self_updates() {
    return can_install && installer().empty();
  }

  std::string latest() {
    std::lock_guard<std::mutex> lock(mutex);
    return latest_version;
  }

  std::string error() {
    std::lock_guard<std::mutex> lock(mutex);
    return error_text;
  }

  // what this build is, and so what an update check compares: commits from a Nix build, else the number
  std::string build_label() {
    std::string ecps = build_info("ecps");
    std::string fan = build_info("fan");
    if (can_install && is_commit(ecps)) {
      return "ecps " + ecps.substr(0, 7) + (is_commit(fan) ? ", fan " + fan.substr(0, 7) : std::string());
    }
    if (can_install) {
      // says what was actually found, so the reason is on screen
      if (build_info_path().empty()) {
        return "no build-info file, so this build isn't from the current Nix package. Version check only";
      }
      return "build-info says ecps=" + (ecps.empty() ? std::string("(missing)") : ecps.substr(0, 24)) + ", not a commit. Version check only";
    }
    return "version numbers";
  }

  // "ecps 74e2526, fan 54624b9" of this build, empty when it doesn't know its commits
  std::string build_short() {
    std::string ecps = build_info("ecps");
    std::string fan = build_info("fan");
    if (!is_commit(ecps)) {
      return {};
    }
    return "ecps " + ecps.substr(0, 7) + (is_commit(fan) ? ", fan " + fan.substr(0, 7) : std::string());
  }

  // text for a few seconds after the program restarted itself into an update, empty otherwise
  std::string update_notice() {
    if (!notice_loaded) {
      notice_loaded = true;
      std::error_code ec;
      if (std::filesystem::exists(restarted_file, ec)) {
        std::ifstream in(restarted_file);
        std::string previous;
        std::getline(in, previous);
        in.close();
        std::filesystem::remove(restarted_file, ec);
        std::string now_running = build_short();
        notice_text = "Updated";
        if (!previous.empty()) {
          notice_text += " from " + previous;
        }
        notice_text += " to " + (now_running.empty() ? std::string("v") + ecps_version : now_running);
        notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      }
    }
    if (std::chrono::steady_clock::now() < notice_until) {
      return notice_text;
    }
    return {};
  }

  void check() {
    state_e current = state.load();
    if (current == state_e::checking || current == state_e::building) {
      return;
    }
    state = state_e::checking;
    std::thread([this] {
      // a Nix build knows the commits it was made from, then any push to ecps_gui or fan counts as an
      // update, no version bump needed. Anything else compares the number in version.h
      std::string built_ecps = build_info("ecps");
      std::string built_fan = build_info("fan");
      // a Nix package build has the file. One that doesn't know its commit (built from a local tree) can't say
      // whether it is behind, so it is offered the newest build: that one, made from GitHub, records its commit
      if (can_install && !build_info_path().empty()) {
        std::string latest_ecps = latest_commit("6413/ecps_gui");
        std::string latest_fan = latest_commit("6413/fan");
        std::lock_guard<std::mutex> lock(mutex);
        if (latest_ecps.empty()) {
          error_text = "could not read the latest commit, is curl installed and the network up?";
          state = state_e::failed;
          return;
        }
        if (!is_commit(built_ecps)) {
          latest_version = "ecps " + latest_ecps.substr(0, 7) + " (this build doesn't know its own commit)";
          state = state_e::available;
          return;
        }
        bool ecps_changed = latest_ecps != built_ecps;
        // a fan the build doesn't know (or GitHub didn't answer for) is not reported as a change
        bool fan_changed = is_commit(latest_fan) && is_commit(built_fan) && latest_fan != built_fan;
        latest_version.clear();
        if (ecps_changed) {
          latest_version += "ecps " + latest_ecps.substr(0, 7);
        }
        if (fan_changed) {
          latest_version += std::string(latest_version.empty() ? "" : ", ") + "fan " + latest_fan.substr(0, 7);
        }
        state = (ecps_changed || fan_changed) ? state_e::available : state_e::up_to_date;
        return;
      }
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
    if (!self_updates() || state.load() != state_e::available) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(ready_file, ec);
    std::filesystem::remove(failed_file, ec);
    state = state_e::building;
#if !defined(_WIN32)
    // --override-input makes the build use the newest fan, not the commit flake.nix pins, so a push to
    // fan alone is enough. --refresh stops nix from answering with a copy it fetched earlier
    std::string command =
      "nohup sh -c 'out=$(nix --extra-experimental-features \"nix-command flakes\" build --refresh --out-link ecps_update_result "
      "--override-input fan github:6413/fan "
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
        restart_at = now + std::chrono::milliseconds(1500);
        state = state_e::ready;
      }
    }
  }

  // GUI thread, every frame: a finished build restarts the program into itself after a short pause
  void tick_restart() {
    if (state.load() == state_e::ready && std::chrono::steady_clock::now() >= restart_at) {
      restart();
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
    {
      // what we were, for the "Updated from ... to ..." line the new process shows
      std::ofstream marker(restarted_file, std::ios::trunc);
      std::string was = build_short();
      marker << (was.empty() ? std::string("v") + ecps_version : was) << '\n';
    }
    // only here, so a normal close or a crash never brings the old session back
    if (collect_state) {
      std::string state = collect_state();
      if (!state.empty()) {
        std::ofstream out(state_file, std::ios::trunc);
        out << state;
      }
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
  // the Nix package writes "ecps=<commit>" and "fan=<commit>" lines and points ECPS_BUILD_INFO at the file
  // The wrapper sets ECPS_BUILD_INFO, but the file is found without it too: next to the executable in the
  // Nix store (libexec/ecps -> share/ecps/build-info), or linked into the working directory by the wrapper
  static std::string build_info_path() {
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("ECPS_BUILD_INFO")) {
      candidates.push_back(env);
    }
  #if !defined(_WIN32)
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
      candidates.push_back(exe.parent_path() / ".." / "share" / "ecps" / "build-info");
    }
  #endif
    candidates.push_back("build-info");
    for (const auto& path : candidates) {
      std::error_code exists_ec;
      if (std::filesystem::is_regular_file(path, exists_ec)) {
        return path.string();
      }
    }
    return {};
  }

  static std::string build_info(const std::string& key) {
    std::string path = build_info_path();
    if (path.empty()) {
      return {};
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      if (line.compare(0, key.size() + 1, key + "=") == 0) {
        std::string value = line.substr(key.size() + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
          value.pop_back();
        }
        return value;
      }
    }
    return {};
  }

  // a full git commit id: 40 hex digits. "unknown" and "-dirty" builds are not one
  static bool is_commit(const std::string& value) {
    if (value.size() != 40) {
      return false;
    }
    for (char c : value) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        return false;
      }
    }
    return true;
  }

  // the commit id GitHub has for the default branch of owner/repo, empty when it can't be read
  static std::string latest_commit(const std::string& repo) {
    std::string body = run_capture("curl -fsSL --max-time 10 -H \"Accept: application/vnd.github.sha\" "
      "https://api.github.com/repos/" + repo + "/commits/HEAD");
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) {
      body.pop_back();
    }
    return is_commit(body) ? body : std::string();
  }

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
  std::chrono::steady_clock::time_point restart_at;
  bool notice_loaded = false;
  std::string notice_text;
  std::chrono::steady_clock::time_point notice_until;
};

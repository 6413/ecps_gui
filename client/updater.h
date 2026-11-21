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
//  - Windows: the newest GitHub release (prereleases too) is compared with ecps_version. Update downloads its
//    .7z/.zip with the curl.exe and tar.exe Windows has, copies the client folder over the folder this exe is in
//    (the user's own files like config.json are kept), and restarts. The running exe can't be overwritten, so it is
//    renamed to *.update_old first and deleted on the next start

#include <algorithm>
#include <cctype>
#include <cstring>
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

  static constexpr bool can_install = true;
  static constexpr bool windows =
#if defined(_WIN32)
    true;
#else
    false;
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
    if (!windows && is_commit(ecps)) {
      return "ecps " + ecps.substr(0, 7) + (is_commit(fan) ? ", fan " + fan.substr(0, 7) : std::string());
    }
    if (!windows) {
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
#if defined(_WIN32)
      cleanup_windows();
#endif
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
      if (!windows && !build_info_path().empty()) {
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
#if defined(_WIN32)
      if (windows) {
        std::string body = run_capture(std::string("curl -fsSL --max-time 15 \"") + releases_url + "\"", 8 << 20);
        std::string tag, url;
        std::uint64_t size = 0;
        bool found = parse_release(body, tag, url, size);
        std::lock_guard<std::mutex> lock(mutex);
        if (!found) {
          error_text = body.empty() ? "could not reach GitHub, is the network up?" : "no Windows download (.7z or .zip) in the newest GitHub release";
          state = state_e::failed;
          return;
        }
        latest_version = tag;
        asset_url = url;
        asset_size = size;
        state = is_newer(tag, ecps_version) ? state_e::available : state_e::up_to_date;
        return;
      }
#endif
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
#if defined(_WIN32)
    phase = 1;
    downloaded = 0;
    std::thread([this] { install_windows(); }).detach();
#else
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

  // Looks for updates on its own: a few seconds after the start, then every 5 seconds. That uses requests GitHub doesn't
  // count against the 60 an hour its API allows without an account (the API is what check() reads); only when one
  // of them shows something new does the full check() run. A background look that fails changes nothing on screen
  void auto_check() {
    auto now = std::chrono::steady_clock::now();
    if (auto_check_running.load() || now < next_auto_check) {
      return;
    }
    next_auto_check = now + std::chrono::seconds(5);
    state_e current = state.load();
    // an update found, being installed or restarting into: nothing more to look for
    if (current != state_e::idle && current != state_e::up_to_date && current != state_e::failed) {
      return;
    }
    auto_check_running = true;
    std::thread([this] {
    bool changed = probe_for_update();
    bool probe_failed = last_probe_failed;
    // if GitHub could not be read at all, still run the full check so the About panel says why. The full check reads
    // the API, 60 an hour: at most once a minute, also when the network is down and every look fails
    auto at = std::chrono::steady_clock::now();
    if ((changed || probe_failed) && at - last_full_check >= std::chrono::minutes(1)) {
      last_full_check = at;
      check();
    }
    auto_check_running = false;
    }).detach();
  }

  // GUI thread, every frame: cheap, looks at the two files once a second while a build runs
  void tick() {
    auto_check();
    if (windows || state.load() != state_e::building) {
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
#else
    std::filesystem::path exe = exe_path();
    {
      std::ofstream marker(restarted_file, std::ios::trunc);
      marker << "v" << ecps_version << '\n';
    }
    if (collect_state) {
      std::string state = collect_state();
      if (!state.empty()) {
        std::ofstream out(state_file, std::ios::trunc);
        out << state;
      }
    }
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    std::wstring dir = exe.parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.wstring().c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      log_capture().restore();
      TerminateProcess(GetCurrentProcess(), 0);
    }
    std::error_code ec;
    std::filesystem::remove(restarted_file, ec);
    std::lock_guard<std::mutex> lock(mutex);
    error_text = "the update is installed, but the new version did not start. Start it by hand";
    state = state_e::build_failed;
#endif
  }

  // "Downloading 45%", "Installing" on Windows, empty elsewhere
  std::string progress() {
#if defined(_WIN32)
    if (phase == 1) {
      std::uint64_t total = asset_size;
      std::uint64_t done = downloaded;
      if (total) {
        return "Downloading " + std::to_string((std::min<std::uint64_t>)(done * 100 / total, 100)) + "%";
      }
      return "Downloading " + std::to_string(done >> 20) + " MB";
    }
    if (phase == 2) {
      return "Installing";
    }
#endif
    return {};
  }

private:
#if defined(_WIN32)
  static constexpr const char* releases_url = "https://api.github.com/repos/6413/ecps_gui/releases?per_page=5";
  // files the user changes, never overwritten by an update (copied only when missing)
  static bool is_user_file(const std::filesystem::path& rel) {
    static const char* names[] = {"config.json", "known_hosts", "fan_settings.json", "imgui.ini", "fan_log.txt"};
    for (const char* n : names) {
      if (rel == n) {
        return true;
      }
    }
    return false;
  }

  std::string asset_url;
  std::atomic<std::uint64_t> asset_size{0};
  std::atomic<std::uint64_t> downloaded{0};
  std::atomic<int> phase{0};

  static std::filesystem::path exe_path() {
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
    buffer.resize(n);
    return std::filesystem::path(buffer);
  }

  static std::wstring system_tool(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir, n) + L"\\" + name;
  }

  // runs a program without a console window and waits; true when it exited with 0. on_wait runs every 200 ms
  static bool run_hidden(const std::wstring& program, std::wstring arguments, const std::function<void()>& on_wait = {}) {
    std::wstring command = L"\"" + program + L"\" " + arguments;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      return false;
    }
    CloseHandle(pi.hThread);
    while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT) {
      if (on_wait) {
        on_wait();
      }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
  }

  // the newest release: its tag (without the "v") and its client archive. A release also carries the server
  // (ecps_server_v*.7z) and the piano recordings (ecps_piano.7z): the one named ecps_v* is taken, else the first
  // .7z/.zip that is neither of those
  static bool parse_release(const std::string& body, std::string& tag, std::string& url, std::uint64_t& size) {
    auto value_at = [&](std::size_t key_end) -> std::string {
      std::size_t colon = body.find(':', key_end);
      if (colon == std::string::npos) {
        return {};
      }
      std::size_t open = body.find('"', colon + 1);
      std::size_t close = open == std::string::npos ? open : body.find('"', open + 1);
      return close == std::string::npos ? std::string() : body.substr(open + 1, close - open - 1);
    };
    std::size_t at = body.find("\"tag_name\"");
    if (at == std::string::npos) {
      return false;
    }
    tag = value_at(at + 10);
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) {
      tag.erase(0, 1);
    }
    std::size_t end = body.find("\"tag_name\"", at + 10);
    int best_rank = 0;
    for (std::size_t u = body.find("\"browser_download_url\"", at); u != std::string::npos && u < end;
      u = body.find("\"browser_download_url\"", u + 1)) {
      std::string candidate = value_at(u + 22);
      auto ends = [&](const char* ext) {
        std::size_t n = std::strlen(ext);
        return candidate.size() > n && candidate.compare(candidate.size() - n, n, ext) == 0;
      };
      if (!ends(".7z") && !ends(".zip")) {
        continue;
      }
      std::string file = candidate.substr(candidate.find_last_of('/') + 1);
      int rank = file.rfind("ecps_v", 0) == 0 ? 2 :
        (file.rfind("ecps_server", 0) == 0 || file.rfind("ecps_piano", 0) == 0) ? 0 : 1;
      if (rank <= best_rank) {
        continue;
      }
      best_rank = rank;
      url = candidate;
      std::size_t s = body.rfind("\"size\":", u);
      size = (s != std::string::npos && s > at) ? std::strtoull(body.c_str() + s + 7, nullptr, 10) : 0;
    }
    return best_rank > 0 && !tag.empty();
  }

  void fail_install(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    error_text = text;
    phase = 0;
    state = state_e::build_failed;
  }

  // background thread: download, unpack, copy over the exe's folder, then the GUI thread restarts
  void install_windows() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe = exe_path();
    fs::path dir = exe.parent_path();
    fs::path work = dir / "ecps_update";
    fs::remove_all(work, ec);
    fs::create_directories(work / "unpacked", ec);
    std::string url;
    {
      std::lock_guard<std::mutex> lock(mutex);
      url = asset_url;
    }
    fs::path archive = work / (url.size() > 3 && url.compare(url.size() - 3, 3, ".7z") == 0 ? "release.7z" : "release.zip");
    bool ok = run_hidden(system_tool(L"curl.exe"),
      L"-fsSL --max-time 1800 -o \"" + archive.wstring() + L"\" \"" + fs::path(url).wstring() + L"\"",
      [&] { std::error_code e; auto n = fs::file_size(archive, e); if (!e) downloaded = n; });
    if (!ok) {
      fail_install("downloading the update failed");
      return;
    }
    phase = 2;
    if (!run_hidden(system_tool(L"tar.exe"), L"-xf \"" + archive.wstring() + L"\" -C \"" + (work / "unpacked").wstring() + L"\"")) {
      fail_install("unpacking the update failed");
      return;
    }
    // the client folder: the one with the client exe in it, a folder named "client" preferred
    fs::path source, new_exe;
    for (auto it = fs::recursive_directory_iterator(work / "unpacked", ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      const fs::path& p = it->path();
      std::string name = p.filename().string();
      if (it->is_regular_file(ec) && p.extension() == ".exe" && name.rfind("ecps", 0) == 0) {
        if (source.empty() || p.parent_path().filename() == "client") {
          source = p.parent_path();
          new_exe = p;
        }
      }
    }
    if (source.empty()) {
      fail_install("the update has no ecps exe in it");
      return;
    }
    // a file in use (this exe, a loaded dll) can't be overwritten but can be renamed
    auto replace = [&](const fs::path& from, const fs::path& to) -> bool {
      std::error_code e;
      if (fs::copy_file(from, to, fs::copy_options::overwrite_existing, e)) {
        return true;
      }
      fs::path old = to;
      old += ".update_old";
      for (int i = 1; fs::exists(old, e); ++i) {
        old = to;
        old += "." + std::to_string(i) + ".update_old";
      }
      e.clear();
      fs::rename(to, old, e);
      return !e && fs::copy_file(from, to, fs::copy_options::overwrite_existing, e);
    };
    for (auto it = fs::recursive_directory_iterator(source, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      fs::path rel = fs::relative(it->path(), source, ec);
      // the exe keeps the name it has now, so shortcuts to it keep working
      fs::path target = it->path() == new_exe ? exe : dir / rel;
      if (it->is_directory(ec)) {
        fs::create_directories(target, ec);
        continue;
      }
      if (is_user_file(rel) && fs::exists(target, ec)) {
        continue;
      }
      if (!replace(it->path(), target)) {
        fail_install("could not replace " + rel.string() + ", is it open somewhere?");
        return;
      }
    }
    fs::remove_all(work, ec);
    std::lock_guard<std::mutex> lock(mutex);
    phase = 0;
    restart_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    state = state_e::ready;
  }

  // after an update: the renamed old files and the download
  static void cleanup_windows() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = exe_path().parent_path();
    fs::remove_all(dir / "ecps_update", ec);
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->path().extension() == ".update_old") {
        std::error_code e;
        fs::remove(it->path(), e);
      }
    }
  }
#endif

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

  // for auto_check: is there something newer than this build, by the requests GitHub doesn't rate limit
  bool probe_for_update() {
    last_probe_failed = false;
#if defined(_WIN32)
    if (windows) {
      // github.com/<repo>/releases/latest redirects to .../releases/tag/v1.0.4: the headers say the newest version.
      // curl.exe without a console window (the popen in run_capture would flash one every minute)
      std::filesystem::path headers = std::filesystem::temp_directory_path() / "ecps_update_probe.txt";
      std::error_code ec;
      std::filesystem::remove(headers, ec);
      if (!run_hidden(system_tool(L"curl.exe"), L"-sI --max-time 10 -o \"" + headers.wstring() +
        L"\" https://github.com/6413/ecps_gui/releases/latest")) {
        return false;
      }
      std::ifstream in(headers, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      std::filesystem::remove(headers, ec);
      std::string tag = tag_from_redirect(text);
      return !tag.empty() && is_newer(tag, ecps_version);
    }
#endif
    if (!build_info_path().empty()) {
      // a Nix build: a new commit on ecps_gui's main or fan's master, from git's own ref listing
      std::string built_ecps = build_info("ecps");
      std::string built_fan = build_info("fan");
      if (!is_commit(built_ecps)) {
        return false;   // check() would offer every look the same "doesn't know its commit" update
      }
    std::string ecps = branch_commit("6413/ecps_gui", "main");
    std::string fan = branch_commit("6413/fan", "master");
    last_probe_failed = ecps.empty() && fan.empty();
    return (is_commit(ecps) && ecps != built_ecps) || (is_commit(fan) && is_commit(built_fan) && fan != built_fan);
    }
    // anything else compares version.h, read from GitHub's file host
    std::string found = parse_version(run_capture(std::string("curl -fsSL --max-time 10 ") + version_url));
    return !found.empty() && is_newer(found, ecps_version);
  }

  // "location: https://github.com/6413/ecps_gui/releases/tag/v1.0.4" -> "1.0.4"
  static std::string tag_from_redirect(const std::string& headers) {
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::size_t at = lower.find("\nlocation:");
    if (at == std::string::npos) {
      return {};
    }
    std::size_t end = headers.find_first_of("\r\n", at + 1);
    std::string location = headers.substr(at + 10, end == std::string::npos ? std::string::npos : end - at - 10);
    std::size_t tag = location.rfind("/tag/");
    if (tag == std::string::npos) {
      return {};
    }
    std::string value = location.substr(tag + 5);
    while (!value.empty() && (value.back() == ' ' || value.back() == '/')) {
      value.pop_back();
    }
    if (!value.empty() && (value[0] == 'v' || value[0] == 'V')) {
      value.erase(0, 1);
    }
    return value;
  }

  // the commit a branch is on, from the ref listing git clones read (github.com/<repo>.git/info/refs): not the API,
  // so not counted against its hourly limit. Each line is "<4 hex length><40 hex commit> refs/heads/<branch>"
  static std::string branch_commit(const std::string& repo, const std::string& branch) {
    std::string body = run_capture("curl -fsSL --max-time 10 \"https://github.com/" + repo +
      ".git/info/refs?service=git-upload-pack\"", 1 << 20);
    std::string ref = " refs/heads/" + branch;
    for (std::size_t at = body.find(ref); at != std::string::npos; at = body.find(ref, at + 1)) {
      std::size_t after = at + ref.size();
      bool whole = after == body.size() || body[after] == '\n' || body[after] == '\0';
      if (whole && at >= 40) {
        std::string commit = body.substr(at - 40, 40);
        if (is_commit(commit)) {
          return commit;
        }
      }
    }
    return {};
  }

  static std::string run_capture(const std::string& command, std::size_t limit = 1 << 16) {
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
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0 && output.size() < limit) {
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
    std::size_t length = (std::max)(a.size(), b.size());
    a.resize(length);
    b.resize(length);
    return a > b;
  }

  std::mutex mutex;
  std::string latest_version;
  std::string error_text;
  std::string built_path;
  std::chrono::steady_clock::time_point last_poll;
  // the first look a few seconds after the start, so it doesn't slow it down
  std::chrono::steady_clock::time_point next_auto_check = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  // the last full check() that a look started: it reads the rate limited API
  std::chrono::steady_clock::time_point last_full_check{};
  std::atomic<bool> auto_check_running{false};
bool last_probe_failed = false;
  std::chrono::steady_clock::time_point restart_at;
  bool notice_loaded = false;
  std::string notice_text;
  std::chrono::steady_clock::time_point notice_until;
};

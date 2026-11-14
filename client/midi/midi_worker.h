#pragma once

// Every WinMM call (counting devices, opening a keyboard, closing it) can block for seconds when a device or
// its driver is busy or pending. They all run on this one thread, and the UI only sets what it wants and reads
// the last result, so a stuck keyboard can never freeze the window.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "midi_input.h"

struct midi_worker_t {
  using callback_t = fan::audio::midi_input_t::callback_t;

  // starts the thread, once. cb receives every note from every open keyboard, on a WinMM thread
  void start(callback_t cb) {
    if (started.exchange(true)) {
      return;
    }
    callback = std::move(cb);
    // never joined: it may be inside a driver call that doesn't return, the process exits anyway
    std::thread([this] { run(); }).detach();
  }

  // whether the keyboards should be open. The thread opens or closes them in its own time
  void set_wanted(bool value) {
    if (wanted.exchange(value) != value) {
      wake();
    }
  }

  void request_scan() {
    scan_requested = true;
    wake();
  }

  bool is_open() const { return open.load(); }
  bool has_scanned() const { return scanned.load(); }
  bool is_busy() const { return busy.load(); }

  std::vector<std::string> devices() {
    std::lock_guard<std::mutex> lock(mutex);
    return device_names;
  }

private:
  void wake() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      dirty = true;
    }
    cv.notify_one();
  }

  void run() {
    fan::audio::midi_input_t input;
    auto retry_at = std::chrono::steady_clock::now();
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex);
        // wakes for a change, and twice a second to retry an open that failed
        cv.wait_for(lock, std::chrono::milliseconds(500), [this] { return dirty; });
        dirty = false;
      }
      bool want = wanted.load();
      auto now = std::chrono::steady_clock::now();
      if (want && !open.load() && now >= retry_at) {
        busy = true;
        std::size_t count = input.open(callback);
        open = count > 0;
        if (!open.load()) {
          retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        busy = false;
      }
      else if (!want && open.load()) {
        busy = true;
        input.close();
        open = false;
        busy = false;
      }
      if (scan_requested.exchange(false)) {
        busy = true;
        auto names = fan::audio::midi_input_t::list_devices();
        {
          std::lock_guard<std::mutex> lock(mutex);
          device_names = std::move(names);
        }
        scanned = true;
        busy = false;
      }
    }
  }

  std::atomic<bool> started{false};
  std::atomic<bool> wanted{false};
  std::atomic<bool> open{false};
  std::atomic<bool> busy{false};
  std::atomic<bool> scan_requested{false};
  std::atomic<bool> scanned{false};
  callback_t callback;
  std::mutex mutex;
  std::condition_variable cv;
  bool dirty = false;
  std::vector<std::string> device_names;
};

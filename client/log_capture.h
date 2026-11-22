#pragma once

// Keeps a copy of everything the program writes to stdout and stderr, so another client can read it.
// Both streams are redirected into pipes at the file descriptor level (fan::print, printf, std::cout and
// std::cerr all end up there) and each reader thread writes the bytes back to where they were going, so a
// terminal still shows everything. A windowed Windows build has no console, then there is nowhere to
// write back to: then everything also goes to ecps_log.txt next to the exe (the run before is ecps_log.prev.txt).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

struct log_capture_t {
  struct chunk_t {
    std::uint64_t seq;
    std::uint8_t stream; // 1 stdout, 2 stderr
    std::string data;
  };

  // effectively the whole run, a reader that joins late gets all of it
  static constexpr std::size_t max_bytes = std::size_t(256) << 20;

  void start() {
    if (started) {
      return;
    }
    started = true;
#if defined(_WIN32)
    // a windowed build (releases): no console, so what is printed goes to a file. The working folder is the exe's
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
      std::remove(log_prev_path);
      std::rename(log_path, log_prev_path);
      file = std::fopen(log_path, "wb");
    }
#endif
    hook(1);
    hook(2);
  }

  // gives stdout and stderr back to where they went, before exec'ing another program that would
  // otherwise inherit pipes nobody reads
  void restore() {
#if !defined(_WIN32)
    for (int i = 0; i < 2; ++i) {
      int target = originals[i];
      bool opened = false;
      if (target < 0) {
        target = open("/dev/null", O_WRONLY);
        opened = true;
      }
      if (target >= 0) {
        dup2(target, i + 1);
        if (opened) {
          close(target);
        }
      }
    }
#endif
  }

  // chunks after cursor, up to about limit bytes, oldest first
  std::vector<chunk_t> read_since(std::uint64_t cursor, std::size_t limit) {
    std::vector<chunk_t> out;
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t total = 0;
    // seq only grows, so skip what the reader already has without walking it
    auto first = std::upper_bound(chunks.begin(), chunks.end(), cursor, [](std::uint64_t value, const chunk_t& chunk) {
      return value < chunk.seq;
    });
    for (auto it = first; it != chunks.end(); ++it) {
      const auto& chunk = *it;
      if (total != 0 && total + chunk.data.size() > limit) {
        break;
      }
      total += chunk.data.size();
      out.push_back(chunk);
    }
    return out;
  }

  // a cursor that makes read_since return roughly the last `bytes` of history
  std::uint64_t backlog_cursor(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t total = 0;
    std::uint64_t cursor = chunks.empty() ? counter : chunks.front().seq - 1;
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
      total += it->data.size();
      if (total > bytes) {
        cursor = it->seq;
        break;
      }
    }
    return cursor;
  }

private:
  void push(std::uint8_t stream, const char* data, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    chunks.push_back({++counter, stream, std::string(data, size)});
    if (file) {
      // each line in the file starts with the time it was written ([18:46:57.123]): how long a step took (an update
      // restart, a reconnect) can be read from it. The copy kept for other clients stays as it was printed
      std::size_t start = 0;
      for (std::size_t i = 0; i <= size; ++i) {
        if (i == size || data[i] == '\n') {
          std::size_t end = i < size ? i + 1 : i;
          if (end > start) {
            if (file_at_line_start) {
              char stamp[24];
              auto now = std::chrono::system_clock::now();
              std::time_t seconds = std::chrono::system_clock::to_time_t(now);
              int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
              std::tm local {};
#if defined(_WIN32)
              localtime_s(&local, &seconds);
#else
              localtime_r(&seconds, &local);
#endif
              std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d.%03d] ", local.tm_hour, local.tm_min, local.tm_sec, ms);
              std::fwrite(stamp, 1, std::strlen(stamp), file);
            }
            std::fwrite(data + start, 1, end - start, file);
            file_at_line_start = data[end - 1] == '\n';
          }
          start = end;
        }
      }
      std::fflush(file);
    }
    stored += size;
    while (stored > max_bytes && chunks.size() > 1) {
      stored -= chunks.front().data.size();
      chunks.pop_front();
    }
  }

  void hook(int fd) {
    int pipe_fds[2];
    int original = -1;
#if defined(_WIN32)
    // a windowed program has no standard handle, duplicating the descriptor would trip the CRT
    HANDLE handle = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    bool has_original = handle != nullptr && handle != INVALID_HANDLE_VALUE;
    if (_pipe(pipe_fds, 1 << 16, _O_BINARY) != 0) {
      return;
    }
    if (has_original) {
      original = _dup(fd);
    }
    // A windowed program's stdout and stderr belong to no descriptor (_fileno is -2): pointing 1 and 2 at the pipe
    // left them unconnected, std::cout's writes failed and nothing reached the pipe or ecps_log.txt. They get one
    // first (NUL), then that one is pointed at the pipe
    FILE* std_stream = fd == 1 ? stdout : stderr;
    if (_fileno(std_stream) < 0) {
      FILE* reopened = nullptr;
      freopen_s(&reopened, "NUL", "w", std_stream);
    }
    int target = _fileno(std_stream) >= 0 ? _fileno(std_stream) : fd;
    if (_dup2(pipe_fds[1], target) != 0) {
      return;
    }
    _close(pipe_fds[1]);
    // a write that failed before this left the stream in a failed state that stops every later one
    (fd == 1 ? std::cout : std::cerr).clear();
#else
    if (pipe(pipe_fds) != 0) {
      return;
    }
    original = dup(fd);
    if (dup2(pipe_fds[1], fd) < 0) {
      return;
    }
    close(pipe_fds[1]);
#endif
    // nothing may wait in a buffer, the other side wants lines now
    originals[fd - 1] = original;
    std::setvbuf(fd == 1 ? stdout : stderr, nullptr, _IONBF, 0);
    int read_fd = pipe_fds[0];
    std::uint8_t stream = (std::uint8_t)fd;
    std::thread([this, read_fd, original, stream] {
      char buffer[4096];
      for (;;) {
#if defined(_WIN32)
        int n = _read(read_fd, buffer, sizeof(buffer));
#else
        ssize_t n = read(read_fd, buffer, sizeof(buffer));
#endif
        if (n <= 0) {
          break;
        }
        push(stream, buffer, (std::size_t)n);
        if (original >= 0) {
#if defined(_WIN32)
          _write(original, buffer, (unsigned)n);
#else
          ssize_t written = write(original, buffer, (std::size_t)n);
          (void)written;
#endif
        }
      }
    }).detach();
  }

  static constexpr const char* log_path = "ecps_log.txt";
  static constexpr const char* log_prev_path = "ecps_log.prev.txt";
  std::FILE* file = nullptr;
  bool file_at_line_start = true;
  std::mutex mutex;
  std::deque<chunk_t> chunks;
  int originals[2] = {-1, -1};
  std::size_t stored = 0;
  std::uint64_t counter = 0;
  bool started = false;
};

inline log_capture_t& log_capture() {
  // never destroyed: its reader threads outlive main
  static log_capture_t* capture = new log_capture_t;
  return *capture;
}

#pragma once

// Keeps a copy of everything the program writes to stdout and stderr, so another client can read it.
// Both streams are redirected into pipes at the file descriptor level (fan::print, printf, std::cout and
// std::cerr all end up there) and each reader thread writes the bytes back to where they were going, so a
// terminal still shows everything. A windowed Windows build has no console, then there is nowhere to
// write back to and the pipe is the only copy.

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
  #include <unistd.h>
#endif

struct log_capture_t {
  struct chunk_t {
    std::uint64_t seq;
    std::uint8_t stream; // 1 stdout, 2 stderr
    std::string data;
  };

  static constexpr std::size_t max_bytes = 1 << 20;

  void start() {
    if (started) {
      return;
    }
    started = true;
    hook(1);
    hook(2);
  }

  // chunks after cursor, up to about limit bytes, oldest first
  std::vector<chunk_t> read_since(std::uint64_t cursor, std::size_t limit) {
    std::vector<chunk_t> out;
    std::lock_guard<std::mutex> lock(mutex);
    std::size_t total = 0;
    for (const auto& chunk : chunks) {
      if (chunk.seq <= cursor) {
        continue;
      }
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
    if (_dup2(pipe_fds[1], fd) != 0) {
      return;
    }
    _close(pipe_fds[1]);
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

  std::mutex mutex;
  std::deque<chunk_t> chunks;
  std::size_t stored = 0;
  std::uint64_t counter = 0;
  bool started = false;
};

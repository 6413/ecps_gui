#pragma once

// Reading another client's stdout/stderr live.
//  - the client being read must switch "allow" on, the answer to every request is "denied" until then
//  - the reader asks with Channel_Log_Request and repeats it every few seconds, the other side drops a
//    reader it stopped hearing from, so a crash on either end cleans up by itself
//  - the bytes come from log_capture_t and are relayed by the server, nothing here is end to end encrypted
// Included in ecps.cpp after ecps_backend exists. The send side runs from tick() on the GUI thread, the
// receive side from the backend's message handlers, hence the two mutexes.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "log_capture.h"

inline log_capture_t& log_capture() {
  // never destroyed: its reader threads outlive main
  static log_capture_t* capture = new log_capture_t;
  return *capture;
}

struct remote_logs_t {
  using C2S = ecps_backend_t::Protocol_C2S_t;
  using S2C = ecps_backend_t::Protocol_S2C_t;
  using sid_t = ecps_backend_t::Protocol_SessionID_t;
  using cid_t = ecps_backend_t::Protocol_ChannelID_t;
  using clock = std::chrono::steady_clock;

  enum class state_e : std::uint8_t { idle, asking, live, denied, stopped };

  struct line_t {
    std::uint8_t stream;
    std::string text;
  };

  static constexpr std::chrono::seconds heartbeat{5};
  static constexpr std::chrono::seconds reader_timeout{20};

  // ---- being read ----------------------------------------------------------------------------

  std::atomic<bool> allow{false};

  fan::event::task_t on_request(S2C::Channel_Log_Request_t m) {
    int reply = -1;
    if (!m.Enable) {
      std::lock_guard<std::mutex> lock(watchers_mutex);
      remove_watcher(m.Peer);
      reply = 2;
    }
    else if (!allow.load()) {
      reply = 0;
    }
    else {
      std::lock_guard<std::mutex> lock(watchers_mutex);
      bool known = false;
      for (auto& w : watchers) {
        if (w.peer.i == m.Peer.i && w.channel.i == m.ChannelID.i) {
          w.seen = clock::now();
          known = true;
        }
      }
      if (!known) {
        // a new reader first gets everything printed so far
        watchers.push_back({m.Peer, m.ChannelID, log_capture().backlog_cursor(log_capture_t::max_bytes), clock::now()});
        reply = 1;
      }
    }
    if (reply >= 0) {
      co_await send_status(m.ChannelID, m.Peer, (std::uint8_t)reply);
    }
  }

  // ---- reading -------------------------------------------------------------------------------

  std::mutex view_mutex;
  std::deque<line_t> lines;
  std::string partial[3];
  // lines[idx[i]] is the i-th line the stdout/stderr filter lets through, lines never move or drop
  std::vector<std::uint32_t> idx;
  std::size_t indexed = 0;
  bool idx_out = true;
  bool idx_err = true;
  std::atomic<bool> watching{false};
  std::atomic<state_e> state{state_e::idle};
  sid_t watch_peer;
  cid_t watch_channel;
  clock::time_point last_request;
  std::uint64_t revision = 0; // changes whenever lines does, the view scrolls on it

  void on_status(const S2C::Channel_Log_Status_t& m) {
    if (!watching.load() || m.Peer.i != watch_peer.i) {
      return;
    }
    if (m.State == 1) {
      state = state_e::live;
    }
    else {
      state = m.State == 0 ? state_e::denied : state_e::stopped;
      watching = false;
    }
  }

  void on_data(const S2C::Channel_Log_Data_t& m) {
    if (!watching.load() || m.Peer.i != watch_peer.i || m.Stream < 1 || m.Stream > 2) {
      return;
    }
    std::size_t size = std::min<std::size_t>(m.Size, sizeof(m.Data));
    std::lock_guard<std::mutex> lock(view_mutex);
    std::string& pending = partial[m.Stream];
    pending.append((const char*)m.Data, size);
    for (;;) {
      std::size_t end = pending.find('\n');
      if (end == std::string::npos) {
        break;
      }
      std::string text = pending.substr(0, end);
      if (!text.empty() && text.back() == '\r') {
        text.pop_back();
      }
      pending.erase(0, end + 1);
      lines.push_back({m.Stream, std::move(text)});
    }
    // a line that never ends must not grow forever
    if (pending.size() > 64 * 1024) {
      lines.push_back({m.Stream, std::move(pending)});
      pending.clear();
    }
    ++revision;
  }

  void start_watch(auto&& queue, cid_t channel, sid_t peer) {
    stop_watch(queue);
    {
      std::lock_guard<std::mutex> lock(view_mutex);
      lines.clear();
      idx.clear();
      indexed = 0;
      partial[1].clear();
      partial[2].clear();
      ++revision;
    }
    watch_peer = peer;
    watch_channel = channel;
    state = state_e::asking;
    last_request = clock::now() - heartbeat; // the next tick sends the request
    watching = true;
  }

  void stop_watch(auto&& queue) {
    if (watching.exchange(false)) {
      queue([channel = watch_channel, peer = watch_peer]() -> fan::event::task_t {
        co_await send_request(channel, peer, false);
      });
    }
    state = state_e::idle;
  }

  void clear_lines() {
    std::lock_guard<std::mutex> lock(view_mutex);
    lines.clear();
    idx.clear();
    indexed = 0;
    ++revision;
  }

  // calls f(count, at) with at(i) giving the i-th visible line. Only what changed since the last call is
  // indexed, so a million lines cost nothing per frame. The lock is held while f runs
  template <typename F>
  void visit(bool out, bool err, F&& f) {
    std::lock_guard<std::mutex> lock(view_mutex);
    if (out != idx_out || err != idx_err) {
      idx_out = out;
      idx_err = err;
      idx.clear();
      indexed = 0;
    }
    for (; indexed < lines.size(); ++indexed) {
      if ((lines[indexed].stream == 1 && out) || (lines[indexed].stream == 2 && err)) {
        idx.push_back((std::uint32_t)indexed);
      }
    }
    f(idx.size(), [this](std::size_t i) -> const line_t& { return lines[idx[i]]; });
  }

  std::uint64_t current_revision() {
    std::lock_guard<std::mutex> lock(view_mutex);
    return revision;
  }

  // kept for callers that want a copy of the tail
  // the last `count` lines of the chosen streams, oldest first
  std::vector<line_t> snapshot(std::size_t count, bool out, bool err, std::uint64_t* seen_revision = nullptr) {
    std::vector<line_t> result;
    std::lock_guard<std::mutex> lock(view_mutex);
    for (auto it = lines.rbegin(); it != lines.rend() && result.size() < count; ++it) {
      if ((it->stream == 1 && out) || (it->stream == 2 && err)) {
        result.push_back(*it);
      }
    }
    std::reverse(result.begin(), result.end());
    if (seen_revision) {
      *seen_revision = revision;
    }
    return result;
  }

  std::string all_text(bool out, bool err) {
    std::string text;
    std::lock_guard<std::mutex> lock(view_mutex);
    for (const auto& line : lines) {
      if ((line.stream == 1 && out) || (line.stream == 2 && err)) {
        text += line.text;
        text += '\n';
      }
    }
    return text;
  }

  // ---- both ----------------------------------------------------------------------------------

  // GUI thread, once a frame. queue(f) hands a coroutine to the backend thread
  void tick(auto&& queue) {
    auto now = clock::now();

    std::vector<C2S::Channel_Log_Data_t> out;
    std::vector<std::pair<cid_t, sid_t>> told_stopped;
    {
      std::lock_guard<std::mutex> lock(watchers_mutex);
      if (!allow.load()) {
        for (const auto& w : watchers) {
          told_stopped.push_back({w.channel, w.peer});
        }
        watchers.clear();
      }
      for (auto it = watchers.begin(); it != watchers.end();) {
        if (now - it->seen > reader_timeout) {
          it = watchers.erase(it);
        }
        else {
          ++it;
        }
      }
      for (auto& w : watchers) {
        auto chunks = log_capture().read_since(w.cursor, 32 * 1024);
        for (const auto& chunk : chunks) {
          for (std::size_t offset = 0; offset < chunk.data.size(); offset += sizeof(out[0].Data)) {
            C2S::Channel_Log_Data_t d;
            std::memset(&d, 0, sizeof(d));
            d.ChannelID = w.channel;
            d.Peer = w.peer;
            d.Seq = chunk.seq;
            d.Stream = chunk.stream;
            d.Size = (std::uint16_t)std::min(sizeof(d.Data), chunk.data.size() - offset);
            std::memcpy(d.Data, chunk.data.data() + offset, d.Size);
            out.push_back(d);
          }
          w.cursor = chunk.seq;
        }
      }
    }
    if (!out.empty()) {
      queue([out]() -> fan::event::task_t {
        try {
          for (auto packet : out) {
            co_await ecps_backend.tcp_write(C2S::Channel_Log_Data, &packet, sizeof(packet));
          }
        }
        catch (...) {}
      });
    }
    for (const auto& [channel, peer] : told_stopped) {
      queue([channel, peer]() -> fan::event::task_t {
        co_await send_status(channel, peer, 2);
      });
    }

    if (watching.load() && now - last_request >= heartbeat) {
      last_request = now;
      queue([channel = watch_channel, peer = watch_peer]() -> fan::event::task_t {
        co_await send_request(channel, peer, true);
      });
    }
  }

private:
  struct watcher_t {
    sid_t peer;
    cid_t channel;
    std::uint64_t cursor;
    clock::time_point seen;
  };

  void remove_watcher(sid_t peer) {
    for (auto it = watchers.begin(); it != watchers.end();) {
      it = it->peer.i == peer.i ? watchers.erase(it) : it + 1;
    }
  }

  static fan::event::task_t send_request(cid_t channel, sid_t peer, bool enable) {
    C2S::Channel_Log_Request_t rest;
    std::memset(&rest, 0, sizeof(rest));
    rest.ChannelID = channel;
    rest.Peer = peer;
    rest.Enable = enable;
    try {
      co_await ecps_backend.tcp_write(C2S::Channel_Log_Request, &rest, sizeof(rest));
    }
    catch (...) {}
  }

  static fan::event::task_t send_status(cid_t channel, sid_t peer, std::uint8_t code) {
    C2S::Channel_Log_Status_t rest;
    std::memset(&rest, 0, sizeof(rest));
    rest.ChannelID = channel;
    rest.Peer = peer;
    rest.State = code;
    try {
      co_await ecps_backend.tcp_write(C2S::Channel_Log_Status, &rest, sizeof(rest));
    }
    catch (...) {}
  }

  std::mutex watchers_mutex;
  std::vector<watcher_t> watchers;
};

#pragma once

// Puts the shared screen's frames back together from their 1 KB UDP packets and hands them on in order.
//
// A lost packet is asked for again (the server keeps the last frames and sends them again) and the frame waits for it
// for about two round trips. Frames after it are held meanwhile, they refer to it. Only a frame that still isn't whole
// then is dropped, and the frame after a dropped one is marked, so the viewer waits for a keyframe instead of decoding
// it into blocks.
//
// What can be lost and how it is noticed (packets come in order, so a hole below a newer packet is a loss):
//   a packet in the middle   a newer packet of the same frame came
//   the last packets         a newer frame started, or the frame has been quiet for a while
//   the first packet         it holds the packet count and the keyframe flag: asked for like any other
//   a whole frame            a newer frame started: its first packet is asked for, which gives the count
//
// No sockets or clocks here: on_packet and poll get the time, requests and frames go out through the callbacks.
// Not thread safe, call it from one thread.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

struct frame_assembler_t {
  static constexpr std::uint16_t sequence_mask = 0xfff;   // frames are numbered in 12 bits
  static constexpr std::size_t packet_bytes = 0x400;
  static constexpr std::size_t max_packets = 0x1000;      // 4 MB frames
  // frames held at once: enough for a resend at a high ping (24 frames at 60 fps is 400 ms)
  static constexpr std::size_t slot_count = 24;
  // a frame this far ahead of the one being waited for means the stream restarted (another sharer, a reconnect)
  static constexpr std::uint16_t restart_distance = 64;
  static constexpr std::size_t max_request_indices = 400; // what one request packet carries

  struct packet_t {
    std::uint16_t sequence = 0;
    std::uint16_t index = 0;
    bool head = false;            // index 0 carries the packet count and the keyframe flag
    std::uint16_t possible = 0;   // packets in the frame, from the head
    bool keyframe = false;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
  };

  struct frame_t {
    std::uint16_t sequence;
    const std::uint8_t* data;
    std::size_t size;
    bool keyframe;
    bool after_loss;   // a frame before it was dropped: it refers to something missing
    bool recovered;    // whole only because lost packets came again
  };

  struct stats_t {
    std::uint64_t packets = 0;          // different packets that arrived
    std::uint64_t duplicates = 0;       // arrived twice (a resend of one that had come after all)
    std::uint64_t late = 0;             // for a frame already handed on or dropped
    std::uint64_t frames = 0;           // handed on
    std::uint64_t recovered = 0;        // of those, whole thanks to resends
    std::uint64_t dropped = 0;          // given up on
    std::uint64_t dropped_head = 0;     // of those, without their first packet
    std::uint64_t dropped_packets = 0;  // packets missing from the dropped ones
    std::uint64_t requests = 0;         // resend requests sent
    std::uint64_t requested_packets = 0;
    std::uint64_t restarts = 0;
  };

  std::function<void(const frame_t&)> on_frame;
  std::function<void(std::uint16_t sequence, const std::vector<std::uint16_t>& missing)> on_request;
  stats_t stats;

  // round trip to the server in ms: how long a resend takes. 0 (not known yet) counts as 50 ms
  void set_rtt_ms(double rtt) { rtt_ns = rtt > 0 ? (std::uint64_t)(rtt * 1e6) : 50'000'000ull; }

  // a different stream: whatever was waiting is thrown away and the next packet starts over
  void reset() {
    for (auto& slot : slots) {
      slot.clear();
    }
    started = false;
    loss_pending = true;
  }

  void on_packet(const packet_t& p, std::uint64_t now) {
    std::uint16_t sequence = p.sequence & sequence_mask;
    if (!started) {
      started = true;
      next = sequence;
      newest = sequence;
    }
    std::uint16_t ahead = distance(next, sequence);
    if (ahead > sequence_mask / 2) {
      // behind what was handed on: a resend that came too late, or a duplicate
      ++stats.late;
      return;
    }
    if (ahead >= restart_distance) {
      ++stats.restarts;
      reset();
      started = true;
      next = sequence;
      newest = sequence;
      ahead = 0;
    }
    // no room for it: the oldest frames are given up on until there is
    while (ahead >= slot_count) {
      drop_next();
      ahead = distance(next, sequence);
    }
    if (distance(newest, sequence) <= sequence_mask / 2) {
      newest = sequence;
    }
    if (p.index >= max_packets || (p.size != packet_bytes && p.size > packet_bytes)) {
      return;
    }

    slot_t& slot = slot_for(sequence, now);
    if (slot.got[p.index]) {
      ++stats.duplicates;
    }
    else {
      std::size_t end = (std::size_t)p.index * packet_bytes + p.size;
      if (slot.data.size() < (std::size_t)(p.index + 1) * packet_bytes) {
        slot.data.resize((std::size_t)(p.index + 1) * packet_bytes);
      }
      std::memcpy(slot.data.data() + (std::size_t)p.index * packet_bytes, p.data, p.size);
      slot.got[p.index] = 1;
      ++slot.received;
      ++stats.packets;
      if (slot.requests) {
        slot.resent_arrived = true;
      }
      if (p.size != packet_bytes) {
        // the short packet is the last one
        slot.count = p.index + 1;
        slot.bytes = end;
      }
      else if (slot.count > 0 && p.index == slot.count - 1) {
        slot.bytes = end;
      }
    }
    if (p.head) {
      slot.keyframe = p.keyframe;
      if (p.possible > 0 && p.possible <= max_packets) {
        slot.count = p.possible;
        // a last packet of exactly 1 KB that came before the head
        if (slot.got[slot.count - 1] && slot.bytes == 0) {
          slot.bytes = (std::size_t)slot.count * packet_bytes;
        }
      }
    }
    slot.highest = std::max(slot.highest, p.index);
    slot.last_packet_at = now;
    deliver();
    poll(now);
  }

  // call often (every few ms): asks for lost packets and gives up on frames that waited too long
  void poll(std::uint64_t now) {
    if (!started) {
      return;
    }
    std::uint16_t span = distance(next, newest);
    // everything that came was handed on: next is one past the newest, nothing waits
    if (span > sequence_mask / 2) {
      return;
    }
    for (std::uint16_t i = 0; i <= span && i < slot_count; ++i) {
      std::uint16_t sequence = (next + i) & sequence_mask;
      slot_t& slot = slot_for(sequence, now);
      bool newer_exists = i < span;
      missing.clear();
      collect_missing(slot, newer_exists, now);
      if (missing.empty()) {
        slot.stalled_since = 0;
        continue;
      }
      if (slot.stalled_since == 0) {
        slot.stalled_since = now;
      }
      // in order they come within a fraction of a ms of each other; a short pause lets a reordered one land first
      bool due = now - slot.stalled_since >= hole_delay_ns &&
        (slot.requests == 0 || now - slot.requested_at >= retry_ns());
      if (due && slot.requests < max_requests && on_request) {
        if (missing.size() > max_request_indices) {
          missing.resize(max_request_indices);
        }
        on_request(sequence, missing);
        ++slot.requests;
        slot.requested_at = now;
        ++stats.requests;
        stats.requested_packets += missing.size();
      }
    }
    // the frame everything waits on: given up on once its resends had their time
    while (started) {
      slot_t& slot = slots[next % slot_count];
      bool waiting = slot.used && slot.sequence == next && slot.stalled_since != 0;
      if (!waiting || now - slot.stalled_since < give_up_ns()) {
        break;
      }
      drop_next();
    }
  }

  // how long the oldest frame has been waiting for packets, 0 when nothing waits
  std::uint64_t waiting_ns(std::uint64_t now) const {
    const slot_t& slot = slots[next % slot_count];
    return started && slot.used && slot.sequence == next && slot.stalled_since ? now - slot.stalled_since : 0;
  }

private:
  struct slot_t {
    bool used = false;
    std::uint16_t sequence = 0;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> got = std::vector<std::uint8_t>(max_packets);
    std::uint32_t received = 0;
    std::int32_t count = -1;     // packets in the frame, -1 until the head or the short last packet says
    std::size_t bytes = 0;       // the frame's size, once its last packet is here
    bool keyframe = false;
    std::uint16_t highest = 0;
    std::uint64_t first_at = 0;
    std::uint64_t last_packet_at = 0;
    std::uint64_t stalled_since = 0;   // when a loss was first seen, 0 = none
    std::uint64_t requested_at = 0;
    int requests = 0;
    bool resent_arrived = false;

    void clear() {
      if (used) {
        std::fill(got.begin(), got.end(), 0);
      }
      used = false;
      received = 0;
      count = -1;
      bytes = 0;
      keyframe = false;
      highest = 0;
      first_at = last_packet_at = stalled_since = requested_at = 0;
      requests = 0;
      resent_arrived = false;
    }
    bool complete() const { return count > 0 && received == (std::uint32_t)count && bytes != 0; }
  };

  static constexpr std::uint64_t hole_delay_ns = 2'000'000ull;
  // a frame whose packets stopped coming, with its count known and no newer frame yet: its tail is late or lost
  static constexpr std::uint64_t quiet_ns = 12'000'000ull;
  static constexpr int max_requests = 3;

  std::array<slot_t, slot_count> slots;
  bool started = false;
  bool loss_pending = true;   // the first frame after a start counts as after a loss: it needs a keyframe
  std::uint16_t next = 0;     // the frame handed on next
  std::uint16_t newest = 0;   // the newest frame a packet came for
  std::uint64_t rtt_ns = 50'000'000ull;
  std::vector<std::uint16_t> missing;

  static std::uint16_t distance(std::uint16_t from, std::uint16_t to) { return (to - from) & sequence_mask; }

  // a resend comes back in about a round trip: ask again after that, give up after about two and a half
  std::uint64_t retry_ns() const { return std::max<std::uint64_t>(rtt_ns + rtt_ns / 4, 8'000'000ull); }
  std::uint64_t give_up_ns() const {
    return std::clamp<std::uint64_t>(rtt_ns * 5 / 2 + 15'000'000ull, 40'000'000ull, 300'000'000ull);
  }

  slot_t& slot_for(std::uint16_t sequence, std::uint64_t now) {
    slot_t& slot = slots[sequence % slot_count];
    if (!slot.used || slot.sequence != sequence) {
      slot.clear();
      slot.used = true;
      slot.sequence = sequence;
      slot.first_at = now;
      slot.last_packet_at = now;
    }
    return slot;
  }

  void collect_missing(const slot_t& slot, bool newer_exists, std::uint64_t now) {
    if (slot.complete()) {
      return;
    }
    if (slot.received == 0) {
      // nothing of it came: its first packet says how big it is
      if (newer_exists) {
        missing.push_back(0);
      }
      return;
    }
    bool tail_lost = newer_exists || now - slot.last_packet_at >= quiet_ns;
    std::uint32_t end = slot.highest;
    if (slot.count > 0 && tail_lost) {
      end = (std::uint32_t)slot.count - 1;
    }
    for (std::uint32_t i = 0; i <= end && i < max_packets; ++i) {
      if (!slot.got[i] && (i < slot.highest || tail_lost)) {
        missing.push_back((std::uint16_t)i);
      }
    }
    // the count isn't known (no head, no short packet) and the frame is over: the head gives it
    if (missing.empty() && slot.count < 0 && tail_lost && !slot.got[0]) {
      missing.push_back(0);
    }
  }

  void deliver() {
    while (started) {
      slot_t& slot = slots[next % slot_count];
      if (!slot.used || slot.sequence != next || !slot.complete()) {
        return;
      }
      ++stats.frames;
      bool recovered = slot.resent_arrived;
      if (recovered) {
        ++stats.recovered;
      }
      if (on_frame) {
        on_frame(frame_t{next, slot.data.data(), slot.bytes, slot.keyframe, loss_pending, recovered});
      }
      loss_pending = false;
      slot.clear();
      next = (next + 1) & sequence_mask;
    }
  }

  void drop_next() {
    slot_t& slot = slots[next % slot_count];
    ++stats.dropped;
    if (slot.used && slot.sequence == next) {
      if (!slot.got[0]) {
        ++stats.dropped_head;
      }
      std::uint32_t expected = slot.count > 0 ? (std::uint32_t)slot.count : (std::uint32_t)slot.highest + 1;
      stats.dropped_packets += expected > slot.received ? expected - slot.received : 0;
    }
    else {
      ++stats.dropped_head;
    }
    slot.clear();
    loss_pending = true;
    next = (next + 1) & sequence_mask;
    deliver();
  }
};

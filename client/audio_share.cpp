// Audio share implementation, out of line.
#include "globals.h"

void audio_share_t::update(bool share, bool play, f32_t volume, const capture_options_t& options) {
    bool want_capture = share && ecps_backend.is_streaming_to_any_channel();
    bool source_changed = options.device_id != capture_options.device_id || options.exclude_discord != capture_options.exclude_discord;
    if (capture.is_running() && (source_changed || discord_changed(options))) {
      capture.stop();
      capture_failed = false;
    }
    capture_options = options;
    capture.set_gain(options.gain);
    if (want_capture && !capture.is_running() && !capture_failed) {
      capture_failed = !encoder.open() || !capture.start([this](const f32_t* pcm, std::uint32_t) { encode(pcm); }, options);
      exclude_failed = options.exclude_discord && !capture_failed && !capture.get_error().empty();
      if (capture_failed || exclude_failed) {
        fan::print("audio sharing:", capture.get_error());
      }
    }
    else if (!want_capture && capture.is_running()) {
      capture.stop();
    }
    capture_failed = capture_failed && share;

    bool want_play = play && ecps_backend.is_viewing_any_channel();
    play_enabled.store(want_play, std::memory_order_relaxed);
    if (want_play && !player.is_running() && !player_failed) {
      player_failed = !player.start();
      if (player_failed) {
        fan::print("audio playback failed to start");
      }
    }
    else if (!want_play && player.is_running()) {
      player.stop();
      receiving = false;
    }
    player_failed = player_failed && play;
    player.set_volume(volume);
  }

bool audio_share_t::discord_changed(const capture_options_t& options) {
    if (!options.exclude_discord || exclude_failed || !fan::audio::loopback_capture_t::can_exclude_processes()) {
      return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_discord_check < std::chrono::seconds(3)) {
      return false;
    }
    last_discord_check = now;
    std::uint32_t pid = fan::audio::loopback_capture_t::find_discord_pid();
    return pid != 0 && pid != capture.get_excluded_pid();
  }

void audio_share_t::encode(const f32_t* pcm) {
    std::vector<std::uint8_t> packet(sizeof(header_t) + fan::audio::stream_max_packet);
    ((header_t*)packet.data())->Sequence = send_sequence++;
    std::int32_t size = encoder.encode(pcm, packet.data() + sizeof(header_t), fan::audio::stream_max_packet);
    if (size <= 0) {
      return;
    }
    packet.resize(sizeof(header_t) + size);
    std::lock_guard<std::mutex> lock(outgoing_mutex);
    if (outgoing.size() < max_outgoing) {
      outgoing.push_back(std::move(packet));
    }
  }

fan::event::task_t audio_share_t::send_pending() {
    std::vector<std::vector<std::uint8_t>> packets;
    {
      std::lock_guard<std::mutex> lock(outgoing_mutex);
      packets.swap(outgoing);
    }
    for (const auto& packet : packets) {
      co_await ecps_backend.write_audio(packet);
    }
  }

void audio_share_t::on_packet(const std::uint8_t* data, std::uintptr_t size) {
    if (!play_enabled.load(std::memory_order_relaxed) || (!decoder.state && !decoder.open())) {
      return;
    }
    std::uint16_t sequence = ((const header_t*)data)->Sequence;
    std::int16_t gap = (std::int16_t)(sequence - expected_sequence);
    if (receiving && gap < 0 && gap > -max_late) {
      return;
    }
    std::array<f32_t, fan::audio::stream_frame_size * fan::audio::stream_channels> pcm;
    for (std::int16_t i = 0; receiving && i < std::min<std::int16_t>(gap, max_concealed); ++i) {
      if (decoder.conceal(pcm.data()) > 0) {
        player.push(pcm.data(), fan::audio::stream_frame_size);
      }
    }
    receiving = true;
    expected_sequence = sequence + 1;
    std::int32_t frames = decoder.decode(data + sizeof(header_t), (std::int32_t)(size - sizeof(header_t)), pcm.data());
    if (frames > 0) {
      player.push(pcm.data(), frames);
    }
  }

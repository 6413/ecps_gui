// Voice chat implementation, out of line.
#include "gui_std.h" // precompiled, first
#define ECPS_CORE_ONLY
#include "app_state.h"

void voice_chat_t::join(channel_id_t channel_id) {
    wanted_channel.store(channel_id.i);
    capture_failed = false;
  }

void voice_chat_t::leave() {
    wanted_channel.store(-1);
  }

bool voice_chat_t::is_wanted(channel_id_t channel_id) const {
    return wanted_channel.load() == (int)channel_id.i;
  }

bool voice_chat_t::is_active() const {
    return active_channel.load() != -1;
  }

void voice_chat_t::update(const settings_t& settings) {
    int wanted = wanted_channel.load();
    if (wanted != -1 && !ecps_backend.is_channel_joined(channel_id_t((uint16_t)wanted))) {
      leave();
    }
    bool want_capture = is_active() && !muted.load();
    if (settings.device != device) {
      capture_failed = false;
    }
    if (capture.is_running() && (!want_capture || settings.device != device)) {
      capture.stop();
    }
    device = settings.device;
    capture.set_gain(settings.gain);
    threshold.store(settings.threshold, std::memory_order_relaxed);
    if (want_capture && !capture.is_running() && !capture_failed) {
      capture_failed = !encoder.open(voice_bitrate, true) ||
        !capture.start([this](const f32_t* pcm, std::uint32_t) { encode(pcm); }, device);
      if (capture_failed) {
        fan::print("voice chat:", capture.get_error().empty() ? std::string("the encoder failed to start") : capture.get_error());
      }
    }

    if (!capture.is_running()) {
      level.store(0.f, std::memory_order_relaxed);
    }
    echo_cancel.store(settings.echo_cancel, std::memory_order_relaxed);
    noise_suppress.store(settings.noise_suppress, std::memory_order_relaxed);
    normalize.store(settings.normalize, std::memory_order_relaxed);

    bool listening = is_active() && !deafened.load();
    if (mixer_opening.valid()) {
      if (mixer_opening.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        mixer_open = mixer_opening.get();
        mixer_failed = !mixer_open;
        if (mixer_failed) {
          fan::print("voice chat: playback failed to start");
        }
      }
    }
    else if (mixer_open && (!listening || settings.output_device != output_device)) {
      mixer.stop();
      mixer_open = false;
    }
    else if (listening && !mixer_open && (!mixer_failed || settings.output_device != output_device)) {
      output_device = settings.output_device;
      mixer.played = [this](const f32_t* pcm, std::uint32_t frame_count) {
        if (echo_cancel.load(std::memory_order_relaxed)) {
          echo.push_reference(pcm, frame_count);
        }
      };
      mixer_opening = std::async(std::launch::async, [this] { return mixer.start(output_device); });
    }
    mixer_failed = mixer_failed && listening;
    mixer.set_volume(settings.volume);

    std::lock_guard<std::mutex> lock(speakers_mutex);
    auto members = listening ? ecps_backend.voice_members_of(channel_id_t((uint16_t)active_channel.load())) : std::vector<std::uint32_t>();
    std::erase_if(speakers, [&](const auto& speaker) {
      bool gone = std::find(members.begin(), members.end(), speaker.first) == members.end();
      if (gone) {
        mixer.remove(speaker.first);
      }
      return gone;
    });
    for (auto it = speaker_volumes.begin(); it != speaker_volumes.end();) {
      if (std::find(members.begin(), members.end(), it->first) == members.end()) {
        it = speaker_volumes.erase(it);
      }
      else {
        ++it;
      }
    }
  }

bool voice_chat_t::has_work() const {
    return active_channel.load() != -1 || wanted_channel.load() != -1 ||
      std::chrono::steady_clock::now() - ecps_backend.voice_probed_at >= std::chrono::seconds(2);
  }

void voice_chat_t::encode(const f32_t* input) {
    std::array<f32_t, fan::audio::stream_frame_size * fan::audio::stream_channels> pcm;
    std::copy(input, input + pcm.size(), pcm.begin());
    bool cancel = echo_cancel.load(std::memory_order_relaxed);
    if (cancel && !echo_running) {
      echo.reset();
    }
    echo_running = cancel;
    if (cancel) {
      echo.process(pcm.data(), fan::audio::stream_frame_size);
    }
    bool suppress = noise_suppress.load(std::memory_order_relaxed);
    if (suppress && !noise_running) {
      noise.reset();
    }
    noise_running = suppress;
    if (suppress) {
      noise.process(pcm.data(), fan::audio::stream_frame_size);
    }
    f32_t peak = 0.f;
    for (f32_t sample : pcm) {
      peak = std::max(peak, std::abs(sample));
    }
    level.store(peak, std::memory_order_relaxed);
    std::int64_t now = now_ms();
    if (peak >= threshold.load(std::memory_order_relaxed)) {
      loud_at_ms.store(now, std::memory_order_relaxed);
    }
    if (now - loud_at_ms.load(std::memory_order_relaxed) > hold_ms) {
      return;
    }
    std::vector<std::uint8_t> packet(sizeof(header_t) + fan::audio::stream_max_packet);
    ((header_t*)packet.data())->Sequence = send_sequence++;
    std::int32_t size = encoder.encode(pcm.data(), packet.data() + sizeof(header_t), fan::audio::stream_max_packet);
    if (size <= 0) {
      return;
    }
    packet.resize(sizeof(header_t) + size);
    std::lock_guard<std::mutex> lock(outgoing_mutex);
    if (outgoing.size() < max_outgoing) {
      outgoing.push_back(std::move(packet));
    }
  }

fan::event::task_t voice_chat_t::sync() {
    int wanted = wanted_channel.load();
    int active = active_channel.load();
    try {
      if (active != -1 && !ecps_backend.voice_known(channel_id_t((uint16_t)active))) {
        active_channel.store(active = -1);
      }
      if (active != -1 && active != wanted) {
        active_channel.store(-1);
        co_await ecps_backend.send_voice_join(channel_id_t((uint16_t)active), false);
        announced_muted.store(-1);
        active = -1;
      }
      if (wanted != -1 && active == -1 && ecps_backend.voice_known(channel_id_t((uint16_t)wanted))) {
        announced_muted.store((int)muted.load());
        co_await ecps_backend.send_voice_join(channel_id_t((uint16_t)wanted), true, muted.load());
        active_channel.store(active = wanted);
      }
      // the others see who muted themselves: tell the server when it changes
      if (active != -1 && announced_muted.load() != (int)muted.load()) {
        announced_muted.store((int)muted.load());
        co_await ecps_backend.send_voice_join(channel_id_t((uint16_t)active), true, muted.load());
      }
      co_await ecps_backend.probe_voice();
    }
    catch (...) {}

    std::vector<std::vector<std::uint8_t>> packets;
    {
      std::lock_guard<std::mutex> lock(outgoing_mutex);
      packets.swap(outgoing);
    }
    for (const auto& packet : packets) {
      if (active != -1) {
        co_await ecps_backend.write_voice(channel_id_t((uint16_t)active), packet.data(), packet.size());
      }
    }
  }

void voice_chat_t::on_packet(channel_id_t channel_id, std::uint32_t session, const std::uint8_t* data, std::uintptr_t size) {
    if (active_channel.load() != (int)channel_id.i || deafened.load()) {
      return;
    }
    std::lock_guard<std::mutex> lock(speakers_mutex);
    f32_t gain = 1.f;
    if (auto it = speaker_volumes.find(session); it != speaker_volumes.end()) {
      gain = it->second;
    }
    auto& speaker = speakers[session];
    if (!speaker) {
      speaker = std::make_unique<speaker_t>();
      speaker->decoder.open();
    }
    if (!speaker->decoder.state) {
      return;
    }
    std::uint16_t sequence = ((const header_t*)data)->Sequence;
    std::int16_t gap = (std::int16_t)(sequence - speaker->expected_sequence);
    if (speaker->receiving && gap < 0 && gap > -max_late) {
      return;
    }
    std::array<f32_t, fan::audio::stream_frame_size * fan::audio::stream_channels> pcm;
    bool normalizing = normalize.load(std::memory_order_relaxed);
    if (!normalizing) {
      speaker->norm_gain = 1.f;
      speaker->norm_level = 0.f;
    }
    for (std::int16_t i = 0; speaker->receiving && i < std::min<std::int16_t>(gap, max_concealed); ++i) {
      if (speaker->decoder.conceal(pcm.data()) > 0) {
        if (normalizing) {
          // a gap filled in: the gain the voice has now, without learning from it
          for (f32_t& sample : pcm) {
            sample = std::clamp(sample * speaker->norm_gain, -1.f, 1.f);
          }
        }
        if (gain != 1.f) {
          for (f32_t& sample : pcm) {
            sample *= gain;
          }
        }
        mixer.push(session, pcm.data(), fan::audio::stream_frame_size);
      }
    }
    speaker->receiving = true;
    speaker->expected_sequence = sequence + 1;
    std::int32_t frames = speaker->decoder.decode(data + sizeof(header_t), (std::int32_t)(size - sizeof(header_t)), pcm.data());
    if (frames > 0) {
      speaker->heard_at_ms = now_ms();
      f32_t peak = 0.f;
      for (std::int32_t i = 0; i < frames * (std::int32_t)fan::audio::stream_channels; ++i) {
        peak = std::max(peak, std::abs(pcm[i]));
      }
      speaker->level = peak;
      // the evening out first, then your own volume for this person on top of it
      if (normalizing) {
        speaker->normalize(pcm.data(), frames * (std::int32_t)fan::audio::stream_channels);
      }
      if (gain != 1.f) {
        for (std::int32_t i = 0; i < frames * (std::int32_t)fan::audio::stream_channels; ++i) {
          pcm[i] *= gain;
        }
      }
      mixer.push(session, pcm.data(), frames);
    }
  }

void voice_chat_t::set_user_volume(std::uint32_t session, f32_t volume) {
    std::lock_guard<std::mutex> lock(speakers_mutex);
    if (volume >= 0.999f && volume <= 1.001f) {
      speaker_volumes.erase(session);
    }
    else {
      speaker_volumes[session] = std::clamp(volume, 0.f, 2.f);
    }
  }

bool voice_chat_t::is_talking(std::uint32_t session) {
    std::int64_t now = now_ms();
    if (session == (std::uint32_t)ecps_backend.session_id.i) {
      return capture.is_running() && now - loud_at_ms.load(std::memory_order_relaxed) <= hold_ms;
    }
    std::lock_guard<std::mutex> lock(speakers_mutex);
    auto it = speakers.find(session);
    return it != speakers.end() && now - it->second->heard_at_ms <= hold_ms;
  }

f32_t voice_chat_t::talk_level(std::uint32_t session) {
    std::int64_t now = now_ms();
    f32_t peak = 0.f;
    if (session == (std::uint32_t)ecps_backend.session_id.i) {
      if (capture.is_running() && now - loud_at_ms.load(std::memory_order_relaxed) <= hold_ms) {
        peak = level.load(std::memory_order_relaxed);
      }
    }
    else {
      std::lock_guard<std::mutex> lock(speakers_mutex);
      auto it = speakers.find(session);
      if (it != speakers.end() && now - it->second->heard_at_ms <= 150) {
        peak = it->second->level;
      }
    }
    if (peak <= 0.f) {
      return 0.f;
    }
    return std::clamp((20.f * std::log10(peak) + 50.f) / 50.f, 0.f, 1.f);
  }

// Short, soft notification sounds: a chat message from someone else, someone joining the channel, someone leaving.
// Made here from sine notes (soft attack, slow fade, a touch of the octave above) instead of shipped as files.
// The network thread only marks a sound as wanted (request), the gui thread plays it (tick), so the audio
// system is used from one thread. Several of one kind within a short time play once

struct notify_sounds_t {
  enum kind_e : std::uint32_t { message, join, leave, kind_count };

  // any thread
  void request(kind_e kind) {
    pending.fetch_or(1u << kind, std::memory_order_relaxed);
  }

  // gui thread, every frame. volume 0 is off, 1 the normal level
  void tick(f32_t volume) {
    std::uint32_t wanted = pending.exchange(0, std::memory_order_relaxed);
    if (!wanted || volume <= 0.f) {
      return;
    }
    if (volume != built_volume) {
      build(volume);
    }
    auto now = std::chrono::steady_clock::now();
    for (std::uint32_t k = 0; k < kind_count; ++k) {
      if (!(wanted & (1u << k)) || now - last_played[k] < std::chrono::milliseconds(400)) {
        continue;
      }
      last_played[k] = now;
      try {
        sounds[k].play_once();
      }
      catch (...) {}
    }
  }

private:
  static constexpr std::uint32_t sample_rate = 48000;

  struct note_t {
    f32_t frequency;
    f32_t start;    // seconds
    f32_t loudness;
  };

  // stereo float32
  static std::vector<f32_t> render(std::initializer_list<note_t> notes, f32_t volume) {
    constexpr f32_t attack = 0.006f;  // fast enough to sound like a tap, slow enough not to click
    constexpr f32_t fade = 0.11f;     // time constant of the fade out
    constexpr f32_t length = 0.45f;   // each note, by then it's quiet
    f32_t end = 0.f;
    for (auto& n : notes) {
      end = std::max(end, n.start + length);
    }
    std::size_t frames = (std::size_t)(end * sample_rate);
    std::vector<f32_t> pcm(frames * 2, 0.f);
    for (auto& n : notes) {
      std::size_t first = (std::size_t)(n.start * sample_rate);
      for (std::size_t i = first; i < frames; ++i) {
        f32_t t = (f32_t)(i - first) / sample_rate;
        if (t > length) {
          break;
        }
        f32_t envelope = std::min(t / attack, 1.f) * std::exp(-t / fade);
        // the last 20 ms go to zero, no click at the end
        envelope *= std::min((length - t) / 0.02f, 1.f);
        f32_t phase = 2.f * 3.14159265f * n.frequency * t;
        f32_t s = (std::sin(phase) + 0.12f * std::sin(2.f * phase)) * envelope * n.loudness * volume;
        pcm[i * 2] += s;
        pcm[i * 2 + 1] += s;
      }
    }
    for (auto& s : pcm) {
      s = std::clamp(s, -1.f, 1.f);
    }
    return pcm;
  }

  void build(f32_t volume) {
    // G5 then C6, quieter than the others: it's the one heard most
    pcm[message] = render({{783.99f, 0.f, 0.14f}, {1046.5f, 0.085f, 0.12f}}, volume);
    // C5 E5 G5 up for a join, the same down for a leave
    pcm[join] = render({{523.25f, 0.f, 0.15f}, {659.25f, 0.07f, 0.15f}, {783.99f, 0.14f, 0.14f}}, volume);
    pcm[leave] = render({{783.99f, 0.f, 0.14f}, {659.25f, 0.07f, 0.15f}, {523.25f, 0.14f, 0.15f}}, volume);
    for (std::uint32_t k = 0; k < kind_count; ++k) {
      sounds[k] = fan::audio::sound_t(pcm[k].data(), pcm[k].size() / 2, 2);
    }
    built_volume = volume;
  }

  std::atomic<std::uint32_t> pending{0};
  f32_t built_volume = -1.f;
  std::vector<f32_t> pcm[kind_count];
  fan::audio::sound_t sounds[kind_count];
  std::chrono::steady_clock::time_point last_played[kind_count]{};
};

inline notify_sounds_t notify_sounds;

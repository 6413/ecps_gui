#pragma once

// Small polyphonic software synth for live MIDI. No platform code and no samples: every note is a few
// decaying harmonics, so it renders the same on Windows and Linux. Meant to be pumped from one thread
// (render) while any thread feeds it (midi) - everything shared is behind one mutex.
// Output is interleaved stereo float at sample_rate.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace fan::audio {
  struct midi_synth_t {
    static constexpr std::uint32_t sample_rate = 48000;
    static constexpr std::uint32_t channels = 2;
    static constexpr std::size_t max_voices = 32;

    // recorded note, interleaved stereo int16 at sample_rate. A note that has one plays it instead of the tone.
    struct sample_t {
      std::vector<std::int16_t> pcm;
    };

    void set_sample(std::uint8_t note, std::shared_ptr<const sample_t> sample) {
      std::lock_guard<std::mutex> lock(mutex);
      if (note < samples.size()) {
        samples[note] = std::move(sample);
      }
    }

    // raw channel message, status 0x80..0xEF (data bytes already stripped of running status)
    void midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
      std::uint8_t type = status & 0xF0;
      std::uint8_t channel = status & 0x0F;
      std::lock_guard<std::mutex> lock(mutex);
      // which keys are down, for drawing a keyboard; read from any thread without the lock
      if (data1 < down.size() && channel != 9) {
        if (type == 0x90 && data2 != 0) {
          down[data1] = true;
        }
        else if (type == 0x80 || type == 0x90) {
          down[data1] = false;
        }
      }
      if (type == 0xB0 && (data1 == 120 || data1 == 123)) {
        clear_down();
      }
      if (channel == 9 && (type == 0x80 || type == 0x90)) {
        // channel 10 is drums in General MIDI, a pitched tone would just be wrong
        if (type == 0x90 && data2 != 0) {
          start_voice(channel, data1, data2, true);
        }
        return;
      }
      switch (type) {
      case 0x90:
        if (data2 == 0) {
          release_note(channel, data1);
        }
        else {
          start_voice(channel, data1, data2, false);
        }
        break;
      case 0x80:
        release_note(channel, data1);
        break;
      case 0xB0:
        if (data1 == 64) { // sustain pedal
          sustain[channel] = data2 >= 64;
          if (!sustain[channel]) {
            for (auto& v : voices) {
              if (v.active && v.channel == channel && v.pedal_held) {
                v.pedal_held = false;
                v.stage = stage_t::release;
              }
            }
          }
        }
        else if (data1 == 120 || data1 == 123) { // all sound off / all notes off
          all_notes_off_locked(data1 == 120);
        }
        else if (data1 == 121) { // reset controllers
          sustain[channel] = false;
        }
        break;
      case 0xE0:
        bend[channel] = ((data2 << 7 | data1) - 8192) / 8192.f;
        break;
      default:
        break;
      }
    }

    // a lost connection or a dropped note-off must not leave a note ringing
    bool is_down(std::uint8_t note) const {
      return note < down.size() && down[note].load(std::memory_order_relaxed);
    }

    void all_notes_off() {
      std::lock_guard<std::mutex> lock(mutex);
      clear_down();
      all_notes_off_locked(false);
      sustain.fill(false);
    }

    // adds into out (stereo, frame_count frames), caller zeroes it first when it wants a clean buffer
    void render(float* out, std::uint32_t frame_count) {
      std::lock_guard<std::mutex> lock(mutex);
      for (auto& v : voices) {
        if (!v.active) {
          continue;
        }
        if (v.sample) {
          const std::int16_t* data = v.sample->pcm.data();
          std::size_t frames = v.sample->pcm.size() / 2;
          for (std::uint32_t i = 0; i < frame_count; ++i) {
            if (v.position >= frames) {
              v.active = false;
              break;
            }
            float gain = v.envelope * v.velocity * (1.f / 32768.f);
            out[i * 2] += data[v.position * 2] * gain;
            out[i * 2 + 1] += data[v.position * 2 + 1] * gain;
            ++v.position;
            advance(v);
            if (!v.active) {
              break;
            }
          }
          continue;
        }
        float freq = 440.f * std::pow(2.f, (v.note - 69 + bend[v.channel] * 2.f) / 12.f);
        float step = freq / sample_rate;
        for (std::uint32_t i = 0; i < frame_count; ++i) {
          float sample = oscillator(v.phase) * v.envelope * v.velocity * 0.2f;
          out[i * 2] += sample * v.pan_l;
          out[i * 2 + 1] += sample * v.pan_r;
          v.phase += step;
          v.phase -= std::floor(v.phase);
          advance(v);
          if (!v.active) {
            break;
          }
        }
      }
    }

    std::size_t active_voices() {
      std::lock_guard<std::mutex> lock(mutex);
      std::size_t count = 0;
      for (auto& v : voices) {
        count += v.active;
      }
      return count;
    }

  private:
    enum class stage_t : std::uint8_t { attack, decay, sustain_hold, release };

    struct voice_t {
      bool active = false;
      bool pedal_held = false;
      bool drum = false;
      stage_t stage = stage_t::attack;
      std::uint8_t channel = 0;
      std::uint8_t note = 0;
      float velocity = 0;
      float envelope = 0;
      float phase = 0;
      float pan_l = 0.7f;
      float pan_r = 0.7f;
      std::uint64_t age = 0;
      std::shared_ptr<const sample_t> sample;
      std::size_t position = 0;
    };

    static float oscillator(float phase) {
      constexpr float tau = 6.28318530718f;
      float x = phase * tau;
      return std::sin(x) + 0.5f * std::sin(2 * x) + 0.25f * std::sin(3 * x) + 0.12f * std::sin(4 * x);
    }

    // exponential-ish piano envelope, per sample
    static void advance(voice_t& v) {
      constexpr float attack_rate = 1.f / (0.004f * sample_rate);
      switch (v.stage) {
      case stage_t::attack:
        v.envelope += attack_rate;
        if (v.envelope >= 1.f) {
          v.envelope = 1.f;
          v.stage = stage_t::decay;
        }
        break;
      case stage_t::decay: {
        // drum hits die fast, held notes decay slowly toward silence like a struck string
        static const float slow = std::exp(-1.f / (1.8f * sample_rate));
        static const float fast = std::exp(-1.f / (0.12f * sample_rate));
        v.envelope *= v.drum ? fast : slow;
        if (v.envelope < 0.0005f) {
          v.active = false;
        }
        break;
      }
      case stage_t::sustain_hold:
        break;
      case stage_t::release: {
        static const float rate = std::exp(-1.f / (0.15f * sample_rate));
        v.envelope *= rate;
        if (v.envelope < 0.0005f) {
          v.active = false;
        }
        break;
      }
      }
    }

    void start_voice(std::uint8_t channel, std::uint8_t note, std::uint8_t velocity, bool drum) {
      voice_t* slot = nullptr;
      for (auto& v : voices) {
        if (!v.active) {
          slot = &v;
          break;
        }
      }
      if (!slot) { // steal the oldest
        slot = &*std::min_element(voices.begin(), voices.end(), [](const voice_t& a, const voice_t& b) {
          return a.age < b.age;
        });
      }
      voice_t v;
      v.active = true;
      v.drum = drum;
      v.channel = channel;
      v.note = note;
      v.velocity = std::pow(velocity / 127.f, 1.5f);
      v.age = ++counter;
      // spread low notes left and high notes right a little
      float pan = std::clamp((note - 60) / 48.f, -1.f, 1.f) * 0.35f;
      v.pan_l = 0.7f * (1.f - pan);
      v.pan_r = 0.7f * (1.f + pan);
      if (drum) {
        v.note = 36 + (note % 24); // some pitch, drums have no sample bank here
      }
      v.pedal_held = false;
      if (!drum && note < samples.size() && samples[note]) {
        v.sample = samples[note];
        v.envelope = 1.f; // the recording has its own attack and decay
        v.stage = stage_t::sustain_hold;
        v.velocity = 0.25f + 0.75f * v.velocity;
      }
      *slot = std::move(v);
    }

    void release_note(std::uint8_t channel, std::uint8_t note) {
      for (auto& v : voices) {
        if (v.active && v.channel == channel && v.note == note && v.stage != stage_t::release && !v.drum) {
          if (sustain[channel]) {
            v.pedal_held = true;
          }
          else {
            v.stage = stage_t::release;
          }
        }
      }
    }

    void all_notes_off_locked(bool immediately) {
      for (auto& v : voices) {
        if (!v.active) {
          continue;
        }
        if (immediately) {
          v.active = false;
        }
        else {
          v.pedal_held = false;
          v.stage = stage_t::release;
        }
      }
    }

    void clear_down() {
      for (auto& key : down) {
        key = false;
      }
    }

    std::mutex mutex;
    std::array<std::atomic<bool>, 128> down{};
    std::array<voice_t, max_voices> voices;
    std::array<std::shared_ptr<const sample_t>, 128> samples;
    std::array<bool, 16> sustain{};
    std::array<float, 16> bend{};
    std::uint64_t counter = 0;
  };
}

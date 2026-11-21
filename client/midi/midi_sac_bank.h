#pragma once

// Loads fan's .sac piano recordings ("steinway keys/1.sac".."88.sac", A0..C8) into a midi_synth_t.
// A .sac is a small header, one size byte per Opus segment (0xff means the packet continues in the next
// byte) and then the packets, 20 ms stereo at 48 kHz. Needs fan::audio::opus_decoder_t, so include it
// where `import fan;` is visible.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "midi_synth.h"

namespace fan::audio {
  struct midi_sac_bank_t {
    static constexpr std::uint8_t first_note = 21;
    static constexpr std::size_t note_count = 88;
    // enough for a held piano note, keeps all 88 around 100 MB
    static constexpr std::size_t max_frames = 6 * midi_synth_t::sample_rate;

#pragma pack(push, 1)
    struct header_t {
      std::uint8_t sign;
      std::uint16_t checksum;
      std::uint8_t channels;
      std::uint16_t begin_cut;
      std::uint16_t end_cut;
      std::uint32_t segments;
    };
#pragma pack(pop)

    static std::shared_ptr<midi_synth_t::sample_t> load(const std::filesystem::path& path, opus_decoder_t& decoder) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return nullptr;
      }
      header_t head;
      if (!file.read((char*)&head, sizeof(head)) || head.sign != 0xff || head.channels != midi_synth_t::channels || head.segments == 0 || head.segments > 100000) {
        return nullptr;
      }
      std::vector<std::uint8_t> sizes(head.segments);
      if (!file.read((char*)sizes.data(), sizes.size())) {
        return nullptr;
      }
      auto sample = std::make_shared<midi_synth_t::sample_t>();
      std::vector<float> pcm(stream_frame_size * stream_channels);
      std::vector<std::uint8_t> packet;
      std::size_t skip = head.begin_cut;
      std::size_t carry = 0;
      for (std::uint8_t size : sizes) {
        carry += size;
        if (size == 0xff) {
          continue;
        }
        packet.resize(carry);
        carry = 0;
        if (!file.read((char*)packet.data(), packet.size())) {
          break;
        }
        std::int32_t frames = decoder.decode(packet.data(), (std::int32_t)packet.size(), pcm.data());
        if (frames <= 0) {
          continue;
        }
        std::size_t from = std::min<std::size_t>(skip, frames);
        skip -= from;
        for (std::size_t i = from * stream_channels; i < (std::size_t)frames * stream_channels; ++i) {
          sample->pcm.push_back((std::int16_t)std::clamp(pcm[i] * 32767.f, -32768.f, 32767.f));
        }
        if (sample->pcm.size() >= max_frames * 2) {
          break;
        }
      }
      if (sample->pcm.size() / 2 > head.end_cut) {
        sample->pcm.resize(sample->pcm.size() - (std::size_t)head.end_cut * 2);
      }
      if (sample->pcm.size() / 2 > max_frames) {
        sample->pcm.resize(max_frames * 2);
      }
      // the last 50 ms fade out so the cut at max_frames doesn't click
      std::size_t fade = std::min<std::size_t>(sample->pcm.size() / 2, midi_synth_t::sample_rate / 20);
      for (std::size_t i = 0; i < fade; ++i) {
        float gain = (float)(fade - i) / fade;
        std::size_t at = sample->pcm.size() - (fade - i) * 2;
        sample->pcm[at] = (std::int16_t)(sample->pcm[at] * gain);
        sample->pcm[at + 1] = (std::int16_t)(sample->pcm[at + 1] * gain);
      }
      return sample->pcm.empty() ? nullptr : sample;
    }

    // blocks for a few seconds, run it off the audio and GUI threads. Returns how many keys loaded.
    static std::size_t load_all(const std::filesystem::path& directory, midi_synth_t& synth) {
      opus_decoder_t decoder;
      if (!decoder.open()) {
        return 0;
      }
      std::size_t loaded = 0;
      for (std::size_t i = 0; i < note_count; ++i) {
        auto sample = load(directory / (std::to_string(i + 1) + ".sac"), decoder);
        if (sample) {
          synth.set_sample((std::uint8_t)(first_note + i), std::move(sample));
          ++loaded;
        }
      }
      return loaded;
    }

    // ECPS_PIANO_DIR, then next to the executable (where the engine keeps audio/), then the working directory
    static std::filesystem::path find_directory() {
      std::vector<std::filesystem::path> roots;
      std::error_code ec;
      if (const char* env = std::getenv("ECPS_PIANO_DIR")) {
        roots.push_back(env);
      }
#if defined(_WIN32)
      wchar_t exe[1024];
      DWORD length = GetModuleFileNameW(nullptr, exe, 1024);
      if (length > 0 && length < 1024) {
        roots.push_back(std::filesystem::path(exe).parent_path() / "audio" / "steinway keys");
      }
#else
      auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
      if (!ec) {
        roots.push_back(exe.parent_path() / "audio" / "steinway keys");
      }
#endif
      roots.push_back(std::filesystem::current_path(ec) / "audio" / "steinway keys");
      for (auto& root : roots) {
        if (std::filesystem::exists(root / "1.sac", ec)) {
          return root;
        }
      }
      return {};
    }

#if defined(_WIN32)
    // The Windows release leaves the recordings out (6 MB of a 12 MB download): the first time MIDI plays they are
    // fetched from the newest GitHub release's ecps_piano.7z and unpacked next to the exe, with the curl.exe and
    // tar.exe Windows has. Blocks for the download, run it off the audio and GUI threads. True when they are there
    static bool download() {
      wchar_t exe[1024];
      DWORD length = GetModuleFileNameW(nullptr, exe, 1024);
      if (length == 0 || length >= 1024) {
        return false;
      }
      std::filesystem::path dir = std::filesystem::path(exe).parent_path();
      std::filesystem::path archive = dir / "ecps_piano.7z.part";
      wchar_t system[MAX_PATH];
      UINT n = GetSystemDirectoryW(system, MAX_PATH);
      std::wstring tools(system, n);
      auto run = [](const std::wstring& program, std::wstring arguments) {
        std::wstring command = L"\"" + program + L"\" " + arguments;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
          return false;
        }
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        return code == 0;
      };
      std::error_code ec;
      bool ok = run(tools + L"\\curl.exe", L"-fsSL --max-time 120 -o \"" + archive.wstring() +
        L"\" https://github.com/6413/ecps_gui/releases/latest/download/ecps_piano.7z") &&
        // the archive holds audio/steinway keys/*.sac
        run(tools + L"\\tar.exe", L"-xf \"" + archive.wstring() + L"\" -C \"" + dir.wstring() + L"\"");
      std::filesystem::remove(archive, ec);
      return ok && std::filesystem::exists(dir / "audio" / "steinway keys" / "1.sac", ec);
    }
#endif
  };
}

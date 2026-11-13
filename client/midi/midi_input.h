#pragma once

// Live MIDI capture. Windows uses WinMM, every connected input device is opened and each short channel
// message is handed to the callback (WinMM's own thread, keep it quick). Other platforms compile to a
// stub that reports no devices, the Linux backend (ALSA sequencer) goes in the #else branch.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <mmsystem.h>
  #pragma comment(lib, "winmm.lib")
#endif

namespace fan::audio {
  struct midi_input_t {
    // status, data1, data2 of a channel voice message (0x80..0xEF)
    using callback_t = std::function<void(std::uint8_t, std::uint8_t, std::uint8_t)>;

    midi_input_t() = default;
    midi_input_t(const midi_input_t&) = delete;
    midi_input_t& operator=(const midi_input_t&) = delete;
    ~midi_input_t() { close(); }

#if defined(_WIN32)
    static std::vector<std::string> list_devices() {
      std::vector<std::string> names;
      UINT count = midiInGetNumDevs();
      for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSA caps{};
        if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
          names.push_back(caps.szPname);
        }
      }
      return names;
    }

    // opens every input device and returns how many started
    std::size_t open(callback_t cb) {
      close();
      {
        std::lock_guard<std::mutex> lock(mutex);
        callback = std::move(cb);
      }
      UINT count = midiInGetNumDevs();
      for (UINT i = 0; i < count; ++i) {
        HMIDIIN handle = nullptr;
        if (midiInOpen(&handle, i, (DWORD_PTR)&proc, (DWORD_PTR)this, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
          continue;
        }
        if (midiInStart(handle) != MMSYSERR_NOERROR) {
          midiInClose(handle);
          continue;
        }
        devices.push_back(handle);
      }
      return devices.size();
    }

    void close() {
      for (HMIDIIN handle : devices) {
        midiInStop(handle);
        midiInReset(handle);
        midiInClose(handle);
      }
      devices.clear();
      std::lock_guard<std::mutex> lock(mutex);
      callback = nullptr;
    }

    bool is_open() const { return !devices.empty(); }

  private:
    static void CALLBACK proc(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
      if (message != MIM_DATA) {
        return;
      }
      auto* self = (midi_input_t*)instance;
      std::uint8_t status = (std::uint8_t)(param1 & 0xFF);
      if (status < 0x80 || status >= 0xF0) { // system messages (clock, active sensing...) aren't played
        return;
      }
      callback_t cb;
      {
        std::lock_guard<std::mutex> lock(self->mutex);
        cb = self->callback;
      }
      if (cb) {
        cb(status, (std::uint8_t)((param1 >> 8) & 0x7F), (std::uint8_t)((param1 >> 16) & 0x7F));
      }
    }

    std::vector<HMIDIIN> devices;
#else
    static std::vector<std::string> list_devices() { return {}; }
    std::size_t open(callback_t) { return 0; }
    void close() {}
    bool is_open() const { return false; }

  private:
#endif
    std::mutex mutex;
    callback_t callback;
  };
}

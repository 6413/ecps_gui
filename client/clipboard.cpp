// Clipboard helpers for the chat.
#include "gui_std.h" // precompiled, first
#define ECPS_CORE_ONLY
#include "app_state.h"


#if defined(_WIN32)
// the files Explorer put on the clipboard (Ctrl+C on files), for pasting them into the chat
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
std::vector<std::string> clipboard_file_paths() {
  std::vector<std::string> paths;
  if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(nullptr)) {
    return paths;
  }
  if (HANDLE data = GetClipboardData(CF_HDROP)) {
    if (auto drop = (HDROP)GlobalLock(data)) {
      UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring path(length, L'\0');
        DragQueryFileW(drop, i, path.data(), length + 1);
        paths.push_back(std::filesystem::path(path).string());
      }
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return paths;
}

// an image on the clipboard (a screenshot, an image copied in a browser or an editor) saved as a .png for the chat,
// empty if there is none. A PNG copy (browsers and the Snipping Tool put one there) is kept as it is. Any other image
// Windows gives as a bitmap: GetDIBits turns whatever it is (palette, 16, 24 or 32 bit, bitfields, bottom-up) into
// 32 bit pixels, and stb writes them as a PNG
std::string clipboard_image_file() {
  static UINT png_format = RegisterClipboardFormatW(L"PNG");
  bool has_png = png_format && IsClipboardFormatAvailable(png_format);
  bool has_bitmap = IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB) ||
    IsClipboardFormatAvailable(CF_DIBV5);
  if (!has_png && !has_bitmap) {
    return {};
  }
  std::error_code ec;
  auto folder = std::filesystem::temp_directory_path(ec) / "ecps";
  std::filesystem::create_directories(folder, ec);
  auto path = folder / ("image-" + std::to_string((long long)std::time(nullptr)) + ".png");
  if (!OpenClipboard(nullptr)) {
    return {};
  }
  bool written = false;
  if (has_png) {
    if (HANDLE data = GetClipboardData(png_format)) {
      if (auto* memory = (const std::uint8_t*)GlobalLock(data)) {
        std::size_t size = GlobalSize(data);
        static const std::uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (size > 8 && std::memcmp(memory, signature, 8) == 0) {
          std::ofstream out(path, std::ios::binary);
          out.write((const char*)memory, (std::streamsize)size);
          written = (bool)out;
        }
        GlobalUnlock(data);
      }
    }
  }
  if (!written && has_bitmap) {
    // Windows makes a CF_BITMAP from a DIB on the clipboard by itself
    if (auto bitmap = (HBITMAP)GetClipboardData(CF_BITMAP)) {
      BITMAP description{};
      if (GetObjectW(bitmap, sizeof(description), &description) && description.bmWidth > 0 && description.bmHeight > 0) {
        int width = description.bmWidth;
        int height = description.bmHeight;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; // top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint8_t> pixels((std::size_t)width * height * 4);
        HDC screen = GetDC(nullptr);
        int rows = GetDIBits(screen, bitmap, 0, (UINT)height, pixels.data(), &info, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);
        if (rows == height) {
          // BGRA to RGBA. A bitmap without alpha leaves it 0, which would be fully transparent
          bool any_alpha = false;
          for (std::size_t i = 3; i < pixels.size(); i += 4) {
            if (pixels[i]) {
              any_alpha = true;
              break;
            }
          }
          for (std::size_t i = 0; i < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
            if (!any_alpha) {
              pixels[i + 3] = 255;
            }
          }
          fan::stb::write(path.string(), pixels.data(), fan::vec2i(width, height), 4);
          written = std::filesystem::file_size(path, ec) > 0 && !ec;
        }
      }
    }
  }
  CloseClipboard();
  return written ? path.string() : std::string();
}

bool clipboard_has_image() {
  static UINT png_format = RegisterClipboardFormatW(L"PNG");
  return (png_format && IsClipboardFormatAvailable(png_format)) || IsClipboardFormatAvailable(CF_BITMAP) ||
    IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5);
}
#endif


#if !defined(_WIN32)
// an image on the clipboard on Linux: Wayland's wl-paste, or xclip on X11. The window library only gives text, so
// the programs are asked for a PNG. Empty if there is none, or neither program is installed
std::string clipboard_image_file() {
  std::error_code ec;
  auto folder = std::filesystem::temp_directory_path(ec) / "ecps";
  std::filesystem::create_directories(folder, ec);
  auto path = folder / ("image-" + std::to_string((long long)std::time(nullptr)) + ".png");
  const std::string out = "\"" + path.string() + "\"";
  const char* commands[] = {
    "wl-paste --no-newline --type image/png",
    "xclip -selection clipboard -t image/png -o",
  };
  for (const char* command : commands) {
    // only when the clipboard really has an image: the programs say a wrong type with a failure and no output
    std::string full = std::string(command) + " > " + out + " 2>/dev/null";
    if (std::system(full.c_str()) != 0) {
      continue;
    }
    std::ifstream in(path, std::ios::binary);
    std::uint8_t head[8] = {};
    in.read((char*)head, 8);
    static const std::uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (in.gcount() == 8 && std::memcmp(head, signature, 8) == 0) {
      return path.string();
    }
  }
  std::filesystem::remove(path, ec);
  return {};
}

// asking for the picture takes a clipboard program here, so there is no quick check: the read is always tried in the
// background and simply comes back empty when the clipboard has no image
bool clipboard_has_image() {
  return false;
}
#endif


// One picture is read at a time, in a thread of its own: a clipboard bitmap's conversion and PNG write take long
// enough to freeze the render thread (and with it the stream on screen) when done in a frame.
namespace {
  std::mutex clipboard_image_mutex;
  std::vector<std::string> clipboard_image_finished;
  bool clipboard_image_reading = false;
}

void clipboard_image_begin() {
  {
    std::lock_guard<std::mutex> lock(clipboard_image_mutex);
    if (clipboard_image_reading) {
      return;
    }
    clipboard_image_reading = true;
  }
  std::thread([] {
    std::string image = ecps_expected_sync([] { return clipboard_image_file(); }).value_or(std::string());
    std::lock_guard<std::mutex> lock(clipboard_image_mutex);
    if (!image.empty()) {
      clipboard_image_finished.push_back(std::move(image));
    }
    clipboard_image_reading = false;
  }).detach();
}

std::vector<std::string> clipboard_image_take() {
  std::lock_guard<std::mutex> lock(clipboard_image_mutex);
  std::vector<std::string> images;
  images.swap(clipboard_image_finished);
  return images;
}

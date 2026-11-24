// The backend's methods that are not message handlers, the frame decode, the adaptive rates
#include "gui_std.h" // precompiled, first
#include "app_state.h"
#include "ecps_threads.h"


void ecps_backend_t::update_host_mouse_coordinate(Protocol_ChannelID_t channel_id, const fan::vec2ui& pos) {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (!rt) return;

  for (auto& channel : channel_info) {
    if (channel.channel_id.i == channel_id.i && channel.is_viewing) {
      rt->host_mouse_pos = pos;
      break;
    }
  }
}

// ---- the page a phone on the same network sends pictures from ------------------------------------------------
static const char* web_upload_page() {
  return R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Send a picture to ECPS</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; font: 16px/1.45 system-ui, sans-serif; background: #140a08; color: #f2e8e4; }
  main { max-width: 520px; margin: 0 auto; padding: 24px 18px 48px; }
  h1 { font-size: 1.45rem; margin: 8px 0 4px; }
  p { color: #c9b6ae; margin: 8px 0; }
  .card { background: #201310; border: 1px solid #4a2c24; border-radius: 14px; padding: 16px; margin-top: 18px; }
  input[type=file] { position: absolute; width: 1px; height: 1px; opacity: 0; }
  label.pick { display: block; text-align: center; padding: 44px 12px; border: 2px dashed #7a4a3c; border-radius: 12px; color: #e8d4cc; font-weight: 600; cursor: pointer; }
  img { display: none; width: 100%; height: auto; border-radius: 10px; margin-top: 14px; }
  img.shown { display: block; }
  button { width: 100%; margin-top: 14px; padding: 13px; font: inherit; font-weight: 700; color: #1c0f0b; background: #ff8a5a; border: 0; border-radius: 10px; }
  button:disabled { opacity: .45; }
  #status { min-height: 1.4em; margin-top: 12px; color: #ffb795; font-weight: 600; }
  small { color: #9c8278; display: block; margin-top: 16px; }
</style></head>
<body><main>
  <h1>Send a picture</h1>
  <p>It goes to the channel open in ECPS on the computer.</p>
  <div class="card">
    <input id="file" type="file" accept="image/*" capture="environment">
    <label class="pick" for="file">Take a photo or choose one</label>
    <img id="preview" alt="">
    <button id="send" disabled>Send to the channel</button>
    <div id="status"></div>
  </div>
  <small>Both devices must be on the same Wi-Fi. The link works only while Phone upload is on.</small>
</main>
<script>
const input = document.getElementById('file');
const send = document.getElementById('send');
const status = document.getElementById('status');
const preview = document.getElementById('preview');
let chosen = null;
input.addEventListener('change', () => {
  chosen = input.files[0] || null;
  send.disabled = !chosen;
  status.textContent = '';
  if (chosen) {
    preview.src = URL.createObjectURL(chosen);
    preview.classList.add('shown');
  }
  else {
    preview.removeAttribute('src');
    preview.classList.remove('shown');
  }
});
send.addEventListener('click', async () => {
  if (!chosen) return;
  send.disabled = true;
  status.textContent = 'Sending...';
  try {
    const response = await fetch(location.pathname.replace(/\/$/, '') + '/upload', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'X-File-Name': encodeURIComponent(chosen.name || 'picture') },
      body: chosen
    });
    if (!response.ok) {
      throw new Error(await response.text());
    }
    status.textContent = 'Sent. Take another?';
    input.value = '';
    chosen = null;
    preview.removeAttribute('src');
    preview.classList.remove('shown');
  }
  catch (e) {
    status.textContent = 'Failed: ' + e.message;
    send.disabled = false;
  }
});
</script></body></html>)PAGE";
}

void ecps_backend_t::start_web_upload(std::uint16_t port, const std::string& token) {
  web_upload_token = token;
  web_upload_enabled.store(true);
  if (web_upload_task.valid()) {
    return;
  }
  web_upload_server = {};
  std::string base = "/" + token;
  web_upload_server.get(base, [](const fan::network::http::request_t&, fan::network::http::response_t& res) -> fan::event::task_t {
    res.header("Cache-Control", "no-store");
    res.html(web_upload_page());
    co_return;
  });
  web_upload_server.post(base + "/upload", [this](const fan::network::http::request_t& req, fan::network::http::response_t& res) -> fan::event::task_t {
    if (!web_upload_enabled.load()) {
      res.status(403).text("Phone upload is off");
      co_return;
    }
    if (req.body.size() < 16 || req.body.size() > 25 * 1024 * 1024) {
      res.status(413).text("Picture is too large (25 MB at most)");
      co_return;
    }
    const auto* data = (const std::uint8_t*)req.body.data();
    const std::size_t size = req.body.size();
    const char* extension = nullptr;
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
      extension = "png";
    }
    else if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
      extension = "jpg";
    }
    else if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
      extension = "gif";
    }
    else if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
      data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
      extension = "webp";
    }
    else if (size >= 2 && data[0] == 'B' && data[1] == 'M') {
      extension = "bmp";
    }
    if (!extension) {
      res.status(415).text("That is not an image");
      co_return;
    }
    std::uint16_t channel = web_upload_channel.load();
    if (channel == (std::uint16_t)-1) {
      res.status(409).text("Join a channel in ECPS on the computer first");
      co_return;
    }
    Protocol_ChannelID_t channel_id;
    channel_id.i = channel;
    if (!is_channel_joined(channel_id)) {
      res.status(409).text("The channel was left; join one in ECPS first");
      co_return;
    }
    std::error_code ec;
    auto folder = std::filesystem::temp_directory_path(ec) / "ecps" / "phone_uploads";
    std::filesystem::create_directories(folder, ec);
    if (ec) {
      res.internal_error("Could not save the picture");
      co_return;
    }
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    std::string name = "phone-" + std::to_string(now) + "." + extension;
    auto path = folder / name;
    {
      std::ofstream out(path, std::ios::binary);
      out.write((const char*)data, (std::streamsize)size);
      if (!out) {
        res.internal_error("Could not save the picture");
        co_return;
      }
    }
    {
      std::lock_guard<std::mutex> lock(web_upload_mutex);
      web_upload_files.emplace_back(channel, path.string());
    }
    res.header("Cache-Control", "no-store");
    res.text("sent");
    co_return;
  });
  try {
    web_upload_task = web_upload_server.listen(fan::network::listen_address_t{ "0.0.0.0", port });
  }
  catch (...) {
    web_upload_enabled.store(false);
  }
}

void ecps_backend_t::stop_web_upload() {
  web_upload_enabled.store(false);
}

std::vector<std::pair<std::uint16_t, std::string>> ecps_backend_t::take_web_uploads() {
  std::lock_guard<std::mutex> lock(web_upload_mutex);
  auto files = std::move(web_upload_files);
  web_upload_files.clear();
  return files;
}

void ecps_backend_t::share_t::CalculateNetworkFlowBucket() {
  uintptr_t MaxBufferSize = (sizeof(ScreenShare_StreamHeader_Head_t) + 0x400) * 8;

  m_NetworkFlow.BucketSize = dynamic_config_t::get_adaptive_bucket_size();
  m_NetworkFlow.Bucket = m_NetworkFlow.BucketSize;

#if ecps_debug_prints >= 1
  uint32_t fps = dynamic_config_t::get_target_framerate();
  ecps_print_throttled("Adaptive bucket: {}fps -> {} bits ({} MB)",
    fps, m_NetworkFlow.BucketSize, m_NetworkFlow.BucketSize / 8 / 1024 / 1024);
#endif
}

void ecps_backend_t::view_t::RequestKeyframe() {
  // whatever broke the picture, the frames after it refer to what is missing: hold them back until the keyframe
  ecps_backend.view_need_keyframe.store(true, std::memory_order_relaxed);
  // keyframes are big, asking for one per lost frame makes the congestion worse
  static auto last_request = std::chrono::steady_clock::time_point{};
  auto now = std::chrono::steady_clock::now();
  if (now - last_request < std::chrono::milliseconds(250)) {
    return;
  }
  last_request = now;

  if (auto* rt = get_render_thread()) {
    rt->ecps_gui.request_idr_reset();
  }
}

static bool is_displayable(const fan::graphics::screen_decode_t::decode_data_t& frame) {
  using image_format_e = fan::graphics::image_format_e;
  // still on the GPU (zero copy): no planes here, only its size to check
  if (frame.gpu_slot >= 0) {
    return frame.image_size.x > 0 && frame.image_size.x <= 7680 && frame.image_size.y > 0 && frame.image_size.y <= 4320;
  }
  size_t planes = frame.pixel_format == image_format_e::yuv420p ? 3 : frame.pixel_format == image_format_e::nv12 ? 2 : 1;
  for (size_t i = 0; i < planes; ++i) {
    if (frame.data[i].empty()) {
      return false;
    }
  }
  return frame.image_size.x > 0 && frame.image_size.x <= 7680 &&
    frame.image_size.y > 0 && frame.image_size.y <= 4320 &&
    frame.stride[0].x >= frame.image_size.x;
}

void decode_frame(render_thread_t* rt, std::vector<uint8_t>& frame_data) {
  // the viewer waited for a keyframe that never came: the decoder may be stuck (AV1 hardware decoders can be)
  if (ecps_backend.view_decoder_reopen.exchange(false)) {
    std::lock_guard<std::mutex> lock(rt->screen_decoder.mutex);
    rt->screen_decoder.update_flags |= codec_update_e::codec;
  }
  // frames in a row that gave no picture. A decoder fed frames whose reference is missing can keep going without
  // an error and without a picture: after a few, a keyframe is asked for, the picture comes back from it
  static int without_picture = 0;
  auto decoded = ecps_expected_sync([&]() -> bool {
    auto frame = rt->screen_decoder.decode(frame_data.data(), frame_data.size(), rt->network_frame);
    // the stream changed codec: only the new codec's keyframe can start it
    if (std::exchange(rt->screen_decoder.decoder_.codec_switched_, false)) {
      fan::print("video: asking for a keyframe in the new codec");
      ecps_backend.view.RequestKeyframe();
    }
    if (frame.type == 1 && is_displayable(frame)) {
      without_picture = 0;
      rt->latest_frame.publish(frame);
      rt->screen_decoder.recycle(frame);
      ecps_backend.view_frames_decoded.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    if (++without_picture >= 5) {
      without_picture = 0;
      // said once every 5 s at most, a stream that stays broken would fill the log
      static auto told_at = std::chrono::steady_clock::time_point {};
      if (std::chrono::steady_clock::now() - told_at > std::chrono::seconds(5)) {
        told_at = std::chrono::steady_clock::now();
        fan::print("video: 5 frames gave no picture, asking for a keyframe");
      }
      ecps_backend.view.RequestKeyframe();
    }
    return false;
  });
  if (decoded && *decoded) {
    return;
  }
  if (!decoded) {
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Decode exception: {}", decoded.error());
  #endif
  }
  ecps_backend.view_decode_failed.fetch_add(1, std::memory_order_relaxed);
  ecps_backend.view.m_stats.Frame_Drop++;
}

void ecps_backend_t::view_t::WriteFramePacket(const frame_assembler_t::frame_t& frame) {
  if (frame.size < 32 || frame.size > 0x1000000) {
    this->m_stats.Frame_Drop++;
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Frame dropped - invalid size: {} bytes (valid range: 32 - 16MB)", frame.size);
  #endif
    return;
  }

#if ecps_debug_prints >= 2
  printf("NETWORK: Frame %u received, %zu bytes%s\n", frame.sequence, frame.size, frame.recovered ? " (resends)" : "");
#endif

  ecps_backend.view_frames_assembled.fetch_add(1, std::memory_order_relaxed);

  // a frame before this one was dropped (its resends never came): decoding this one would show blocks until intra
  // refresh swept over them. A keyframe needs nothing before it
  if (frame.after_loss && !frame.keyframe) {
    RequestKeyframe();
  }

  // after a start or a loss only a keyframe can be decoded cleanly; the frames before it are held back (the last
  // picture stays) instead of decoding into blocks. A sharer that never marks keyframes isn't waited on forever
  if (ecps_backend.view_need_keyframe.load(std::memory_order_relaxed)) {
    uint64_t now = fan::event::now();
    if (frame.keyframe) {
      ecps_backend.view_need_keyframe.store(false, std::memory_order_relaxed);
      m_KeyFrameWaitSince = 0;
      m_KeyFrameRetried = false;
    }
    else {
      if (m_KeyFrameWaitSince == 0) {
        m_KeyFrameWaitSince = now;
      }
      if (now - m_KeyFrameWaitSince < 3000000000ull) {
        ecps_backend.view_keyframe_waits.fetch_add(1, std::memory_order_relaxed);
        RequestKeyframe();
        frame_index++;
        return;
      }
      if (!m_KeyFrameRetried) {
        // no keyframe for 3 s: a decoder stuck on this stream is opened again, once, and given another 3 s
        m_KeyFrameRetried = true;
        m_KeyFrameWaitSince = now;
        ecps_backend.view_decoder_reopen.store(true, std::memory_order_relaxed);
        ecps_backend.view_keyframe_waits.fetch_add(1, std::memory_order_relaxed);
        RequestKeyframe();
        frame_index++;
        return;
      }
      // a sharer that never marks keyframes isn't waited on forever
      ecps_backend.view_need_keyframe.store(false, std::memory_order_relaxed);
      m_KeyFrameWaitSince = 0;
      m_KeyFrameRetried = false;
    }
  }

  if (!decode_queue.push(std::vector<uint8_t>(frame.data, frame.data + frame.size))) {
    this->m_stats.Frame_Drop++;
    ecps_backend.view_queue_full.fetch_add(1, std::memory_order_relaxed);
    RequestKeyframe();
  }

  frame_index++;
}
uint32_t dynamic_config_t::get_target_framerate() {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (rt && rt->screen_encoder.config_.frame_rate > 0) {
    return rt->screen_encoder.config_.frame_rate;
  }
  return 60;
}

uint64_t dynamic_config_t::get_adaptive_bitrate() {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (rt && rt->ecps_gui.lossless) {
    // lossless: the pacer must not clamp it to the adaptive rate
    return 1000000000;
  }
  if (rt && rt->ecps_gui.stream_settings.bitrate_mode == 1) {
    return rt->ecps_gui.stream_settings.bitrate_mbps * 1000000;
  }

  // 1080p at 60 fps had 12 Mbit/s: games with foliage and shade turned into squares. A little more at each rate
  uint64_t bitrate = pick_by_fps(30000000, 25000000, 20000000, 18000000, 12000000);
  // the table is for 1080p. More pixels need more bits, but less than in proportion: 1440p gets 1.33x, 4k 2x
  if (rt) {
    f64_t pixels = (f64_t)rt->screen_encoder.config_.width * rt->screen_encoder.config_.height;
    bitrate = (uint64_t)(bitrate * std::clamp(std::sqrt(pixels / (1920.0 * 1080.0)), 1.0, 3.0));
  }
  return bitrate;
}

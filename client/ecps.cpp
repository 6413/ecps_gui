#include "gui_std.h" // precompiled, first
#if defined(_WIN32)
#include <stdlib.h>
#endif
#include "app_state.h"
#include "ecps_threads.h"

int main() {
#if defined(_WIN32)
  // config.json, the settings, known_hosts, fonts and chats are found next to the exe. Started from the Start menu
  // search or a shortcut, Windows can give another working folder and the app would read and write somewhere else
  {
    wchar_t* exe = nullptr;
    if (_get_wpgmptr(&exe) == 0 && exe) {
      std::error_code ec;
      std::filesystem::current_path(std::filesystem::path(exe).parent_path(), ec);
    }
  }
#endif
  // from here on everything printed can be read by another client that is allowed to
  log_capture().start();
  fan::print(std::string("ecps client v") + ecps_version);
  ecps_backend.audio_cb = [](const uint8_t* data, uintptr_t size) {
    audio_share.on_packet(data, size);
  };
  ecps_backend.voice_cb = [](ecps_backend_t::Protocol_ChannelID_t channel_id, std::uint32_t session, const uint8_t* data, uintptr_t size) {
    voice_chat.on_packet(channel_id, session, data, size);
  };
  ecps_backend.login_fail_cb = [](fan::exception_t e) {
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (rt) {// might be able to be removed
      rt->screen_decoder.graphics_queue_callback([e] {
        fan::printcl("failed to connect to server:"_str + e.reason + ", retrying...");
      });
    }
  };
  // the assembler runs on the network thread (the udp receive and the 1 ms network task)
  ecps_backend.view.assembler.on_frame = [](const frame_assembler_t::frame_t& frame) {
    ecps_backend.view.WriteFramePacket(frame);
  };
  ecps_backend.view.assembler.on_request = [](uint16_t sequence, const std::vector<uint16_t>& missing) {
    ecps_backend.view.SendRecoveryRequest(sequence, missing);
  };
  ecps_backend.view.recovery_callback = [](std::vector<uint8_t> request_data) {
    auto* rt = get_render_thread();
    if (rt) {
      rt->ecps_gui.backend_queue([request_data = std::move(request_data)]() -> fan::event::task_t {
          ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_RecoveryRequest_t rest;
          rest.ChannelID = ecps_backend_t::Protocol_ChannelID_t(0);
          rest.ChannelSessionID = ecps_backend_t::Protocol_ChannelSessionID_t(0);

          for (const auto& channel : ecps_backend.channel_info) {
            if (channel.is_viewing) {
              rest.ChannelID = channel.channel_id;
              rest.ChannelSessionID = channel.session_id;
              break;
            }
          }

          (void)co_await ecps_expected(ecps_backend.udp_write(
            0,
            ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_RecoveryRequest,
            rest,
            request_data.data(),
            request_data.size()
          ));
      });
    }
  };

  ecps_backend.share.resend_packet_callback = [](const std::vector<uint8_t>& packet_data) {
    auto* rt = get_render_thread();
    if (rt) {
      rt->ecps_gui.backend_queue([packet_data]() -> fan::event::task_t {
          auto* channel = ecps_backend.streaming_channel();
          if (!channel) {
            co_return;
          }
          ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t rest;
          rest.ChannelID = channel->channel_id;
          rest.ChannelSessionID = channel->session_id;

          (void)co_await ecps_expected(ecps_backend.udp_write(
            0,
            ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData,
            rest,
            packet_data.data(),
            packet_data.size()
          ));
      });
    }
  };

  std::promise<void> render_thread_promise;
  std::future<void> render_thread_future = render_thread_promise.get_future();

  // the engine builds its gui fonts on the render thread, so load the emojis before it starts
  fan::graphics::gui::load_emojis();

  fan::event::thread_create([&render_thread_promise] { run_render_thread(render_thread_promise); });

  render_thread_future.wait();


  // counted before the thread exists, so the render thread can't miss a worker that is about to start
  worker_threads_active.fetch_add(1);
  fan::event::thread_create([] { run_encode_thread(); });

  worker_threads_active.fetch_add(1);
  fan::event::thread_create([] { run_decode_thread(); });

  // runs until the program stops, sleeping when there is nothing queued (not an idle handle, see idle_task)
  auto queue_task = idle_task();

  auto network_task = fan::event::every(1, []() -> fan::event::runv_t<bool> { return network_tick(); });
  fan::event::loop();

  auto* rt_final = render_thread_ptr.load(std::memory_order_acquire);
  if (rt_final) {
    rt_final->should_stop.store(true);
  }
  // static destructors run in exit, nothing may still be using the globals by then
  stop_worker_threads();
  auto render_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (render_thread_ready.load() && std::chrono::steady_clock::now() < render_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::exit(0);
}

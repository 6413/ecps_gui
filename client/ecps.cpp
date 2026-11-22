#include "app_state.h"

static fan::vec2i capture_origin(render_thread_t* rt) {
#if defined(_WIN32)
  return rt->screen_encoder.mdscr.desktop_origin;
#else
  return fan::vec2i(0, 0);
#endif
}

// the host's own setting decides, the server can't turn control on by itself
static bool host_allows_input(ecps_backend_t& backend, auto channel_id) {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  bool allowed = rt && rt->ecps_gui.stream_settings.input_control != 0 && !rt->ecps_gui.stream_settings.share_camera &&
    std::any_of(backend.channel_info.begin(), backend.channel_info.end(), [&](const auto& channel) {
      return channel.is_streaming && channel.channel_id.i == channel_id.i;
    });
  if (!allowed) {
    host_input.close();
    return false;
  }
  static bool reported = false;
  if (!host_input.is_open() && !host_input.open()) {
    if (!reported) {
      fan::print("input control: no way to inject input here, on wayland /dev/uinput must be writable");
    }
    reported = true;
    return false;
  }
  reported = false;
  auto& geometry = rt->screen_encoder.mdscr.Geometry;
  fan::vec2i origin = capture_origin(rt);
  host_input.set_desktop(origin.x, origin.y, geometry.Resolution.x, geometry.Resolution.y);
  return true;
}

// viewers send stream pixels, the stream can be smaller than the desktop
static bool stream_to_desktop(const fan::vec2si& position, std::int32_t& x, std::int32_t& y) {
  auto* rt = render_thread_ptr.load(std::memory_order_acquire);
  if (!rt) {
    return false;
  }
  auto& config = rt->screen_encoder.config_;
  std::int64_t desktop_width = rt->screen_encoder.mdscr.Geometry.Resolution.x;
  std::int64_t desktop_height = rt->screen_encoder.mdscr.Geometry.Resolution.y;
  if (position.x < 0 || position.y < 0 || !desktop_width || !desktop_height || !config.width || !config.height) {
    return false;
  }
  fan::vec2i origin = capture_origin(rt);
  x = origin.x + (std::int32_t)std::min<std::int64_t>((std::int64_t)position.x * desktop_width / config.width, desktop_width - 1);
  y = origin.y + (std::int32_t)std::min<std::int64_t>((std::int64_t)position.y * desktop_height / config.height, desktop_height - 1);
  return true;
}

// every handler reads its own message first. A failed read means the connection is going down,
// there is nothing in the message to act on then
#define s2c(name) __dme_get(Protocol_S2C, name) = [](ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) -> fan::event::task_t
#define s2c_read(name) \
  auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::name##_t>(); \
  if (msg.status != 0) { \
    co_return; \
  }
// messages another part of the client handles whole
#define s2c_forward(name, call) s2c(name) { \
    s2c_read(name); \
    call; \
    co_return; \
  }

static void encoder_force_keyframe() {
  if (auto* rt = get_render_thread()) {
    rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
  }
}

static bool is_midi_voice_message(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
  return status >= 0x80 && status < 0xF0 && !(data1 & 0x80) && !(data2 & 0x80);
}

static void play_midi(bool allowed, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
  if (!allowed) {
    midi_host.silence();
  }
  // the server checks too, but this is the player's own machine
  else if (is_midi_voice_message(status, data1, data2)) {
    midi_host.play(status, data1, data2);
  }
}

ecps_backend_t::ecps_backend_t() {
  s2c(KeepAlive) {
    co_return;
  };

  s2c(KeyExchange) {
    auto msg = co_await backend.tcp_client.read<Protocol_S2C_t::KeyExchange_t>();
    fan::network::crypto::x25519_key_t server_ephemeral, server_static;
    fan::network::crypto::session_keys_t keys;
    bool ok = msg.status == 0;
    if (ok) {
      std::memcpy(server_ephemeral.data(), msg->ServerEphemeral, server_ephemeral.size());
      std::memcpy(server_static.data(), msg->ServerStatic, server_static.size());
      ok = fan::network::crypto::client_handshake(backend.handshake_private, backend.handshake_public, server_ephemeral, server_static, Protocol::CryptoContext, keys)
        && fan::network::crypto::confirm_equal(keys.confirm, msg->Confirm)
        && backend.verify_server_key(server_static);
    }
    fan::network::crypto::cleanse(backend.handshake_private.data(), backend.handshake_private.size());
    if (!ok) {
      backend.mark_disconnected("server key exchange failed");
      co_return;
    }
    backend.udp_crypto.set_client(keys);
    backend.tcp_client.enable_secure(keys.tcp_c2s, keys.tcp_s2c);
    backend.tcp_secure_ready = true;
    fan::network::crypto::cleanse(&keys, sizeof(keys));
  };

  s2c(Response_Login) {
    s2c_read(Response_Login);
    backend.session_id = msg->SessionID;
    co_await backend.udp_write(0, ProtocolUDP::C2S_t::KeepAlive, {}, 0, 0);
  };

  s2c(CreateChannel_OK) {
    s2c_read(CreateChannel_OK);
    backend.finish_request(base.ID, msg->ChannelID);
    co_return;
  };

  s2c(CreateChannel_Error) {
    s2c_read(CreateChannel_Error);
    fan::print("the server refused to create the channel:", Protocol::JoinChannel_Error_Reason_String[(std::uint8_t)msg->Reason % 4]);
    backend.finish_request(base.ID, Protocol_ChannelID_t((uint16_t)-1));
    co_return;
  };

  s2c(JoinChannel_OK) {
    s2c_read(JoinChannel_OK);
    // the join this answers, by its request: an auto join without a password can be on its way next to the one with it
    ecps_backend_t::channel_info_t* answered = nullptr;
    for (auto& entry : backend.channel_info) {
      if (entry.join_request_id == base.ID && entry.channel_id.i == msg->ChannelID.i) {
        answered = &entry;
      }
    }
    if (auto* channel = answered ? answered : backend.find_channel(msg->ChannelID)) {
      channel->session_id = msg->ChannelSessionID;
      channel->join_confirmed = true;
      // the password worked, it is not asked for again
      if (!channel->password.empty()) {
        backend.remember_password(backend.channel_name_of(channel->channel_id), channel->password);
        channel->password.clear();
      }
    }
    encoder_force_keyframe();
    // the keyframe asked for with the join can reach the streamer before the server has us in the channel: once
    // more now that we are in, so the first picture doesn't wait for the next regular keyframe
    auto joined = msg->ChannelID;
    if (auto* channel = backend.find_channel(joined); channel && !channel->is_own) {
      backend.view_need_keyframe = true;
      try {
        co_await backend.send_idr_request(joined);
      }
      catch (...) {}
    }
    co_return;
  };

  s2c(JoinChannel_Error) {
    s2c_read(JoinChannel_Error);
    // the channel was added when the join was sent, it has to go again
    for (const auto& channel : backend.channel_info) {
      if (channel.join_request_id == base.ID) {
        fan::print("could not join channel", (uint64_t)channel.channel_id.i, ":", Protocol::JoinChannel_Error_Reason_String[(std::uint8_t)msg->Reason % 4]);
        backend.did_just_join = false;
        auto channel_id = channel.channel_id;
        std::string name = backend.channel_name_of(channel_id);
        // only this join goes: another one for the same channel (the password typed in) may still be on its way
        bool other_join = std::any_of(backend.channel_info.begin(), backend.channel_info.end(), [&](const auto& entry) {
          return entry.channel_id.i == channel_id.i && entry.join_request_id != base.ID;
        });
        if (other_join) {
          auto request = base.ID;
          backend.channel_info.erase(std::remove_if(backend.channel_info.begin(), backend.channel_info.end(),
            [&](const auto& entry) { return entry.join_request_id == request; }), backend.channel_info.end());
          break;
        }
        backend.forget_channel(channel_id);
        if (msg->Reason == Protocol::JoinChannel_Error_Reason_t::PasswordRequired ||
          msg->Reason == Protocol::JoinChannel_Error_Reason_t::WrongPassword) {
          bool wrong = msg->Reason == Protocol::JoinChannel_Error_Reason_t::WrongPassword;
          if (wrong) {
            backend.forget_password(name);
          }
          std::lock_guard<std::mutex> lock(backend.password_mutex);
          if (!backend.password_prompt.wanted || wrong) {
            backend.password_prompt = { true, wrong, channel_id, name };
          }
        }
        break;
      }
    }
    co_return;
  };

  s2c(Channel_ScreenShare_ViewToShare) {
    s2c_read(Channel_ScreenShare_ViewToShare);
    if (backend.is_channel_streaming(msg->ChannelID) && (msg->Flag & ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR)) {
      encoder_force_keyframe();
    }
    co_return;
  };

  s2c(ChannelList) {
    s2c_read(ChannelList);
    uint16_t channel_count = msg->ChannelCount;
    std::vector<ecps_backend_t::channel_list_info_t> channels;
    channels.reserve(channel_count);

    for (uint16_t i = 0; i < channel_count; ++i) {
      auto channel_info = co_await backend.tcp_client.read<Protocol_S2C_t::ChannelInfo_t>();
      if (channel_info.status != 0) {
        co_return;
      }
      ecps_backend_t::channel_list_info_t info;
      info.channel_id = channel_info->ChannelID;
      info.type = channel_info->Type;
      info.user_count = channel_info->UserCount;
      info.name = std::string(channel_info->Name, strnlen(channel_info->Name, 63));
      info.is_password_protected = (channel_info->IsPasswordProtected != 0);
      info.host_session_id = channel_info->HostSessionID;
      channels.push_back(info);
    }
    backend.available_channels = std::move(channels);
    backend.channel_list_received = true;
  };

  s2c(ChannelSessionList) {
    s2c_read(ChannelSessionList);
    Protocol_ChannelID_t channel_id = msg->ChannelID;
    uint16_t session_count = msg->SessionCount;
    // keep the old list until the new one is fully read, the gui draws from it between reads
    std::vector<ecps_backend_t::session_info_t> sessions;
    sessions.reserve(session_count);

    for (uint16_t i = 0; i < session_count; ++i) {
      auto session_info = co_await backend.tcp_client.read<Protocol_S2C_t::SessionInfo_t>();
      if (session_info.status != 0) {
        co_return;
      }
      ecps_backend_t::session_info_t info;
      info.session_id = session_info->SessionID;
      info.channel_session_id = session_info->ChannelSessionID;
      info.account_id = session_info->AccountID;
      info.username = std::string((const char*)session_info->Username, strnlen((const char*)session_info->Username, 31));
      info.is_host = (session_info->IsHost != 0);
      info.joined_at = session_info->JoinedAt;
      sessions.push_back(info);
    }
    // someone joined or left a channel we're in: a sound. Only against a list that already had us, so our own joining
    // (whose first list is everyone already there) is silent
    if (backend.is_channel_joined(channel_id)) {
      auto old = backend.channel_sessions.find(channel_id.i);
      if (old != backend.channel_sessions.end()) {
        auto has = [](const std::vector<ecps_backend_t::session_info_t>& list, auto id) {
          return std::any_of(list.begin(), list.end(), [&](const auto& s) { return s.session_id.i == id; });
        };
        auto me = backend.session_id.i;
        if (has(old->second, me)) {
          for (const auto& s : sessions) {
            if (s.session_id.i != me && !has(old->second, s.session_id.i)) {
              notify_sounds.request(notify_sounds_t::join);
            }
          }
          for (const auto& s : old->second) {
            if (s.session_id.i != me && !has(sessions, s.session_id.i)) {
              notify_sounds.request(notify_sounds_t::leave);
            }
          }
        }
      }
    }
    backend.channel_sessions[channel_id.i] = std::move(sessions);
    for (auto& channel : backend.available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        channel.user_count = session_count;
        break;
      }
    }
  };

  // input
  s2c(Channel_ScreenShare_View_InformationToViewSetFlag) {
    s2c_read(Channel_ScreenShare_View_InformationToViewSetFlag);
    if (auto* channel = backend.find_channel(msg->ChannelID)) {
      channel->flag = msg->Flag;
    }
    co_return;
  };

  s2c(Channel_ScreenShare_View_InformationToViewMouseCoordinate) {
    s2c_read(Channel_ScreenShare_View_InformationToViewMouseCoordinate);
    backend.update_host_mouse_coordinate(msg->ChannelID, msg->pos);
    co_return;
  };

  s2c(Channel_Voice_Members) {
    s2c_read(Channel_Voice_Members);
    if (!backend.is_channel_joined(msg->ChannelID)) {
      co_return;
    }
    std::vector<std::uint32_t> members;
    for (std::uint8_t i = 0; i < msg->Count && i < 16; ++i) {
      members.push_back((std::uint32_t)msg->Members[i].i);
    }
    std::lock_guard<std::mutex> lock(backend.voice_mutex);
    backend.voice_members[(std::uint32_t)msg->ChannelID.i] = std::move(members);
    co_return;
  };

  s2c(Channel_Voice_Muted) {
    s2c_read(Channel_Voice_Muted);
    if (!backend.is_channel_joined(msg->ChannelID)) {
      co_return;
    }
    std::vector<std::uint32_t> muted;
    for (std::uint8_t i = 0; i < msg->Count && i < 16; ++i) {
      muted.push_back((std::uint32_t)msg->Members[i].i);
    }
    std::lock_guard<std::mutex> lock(backend.voice_mutex);
    backend.voice_muted[(std::uint32_t)msg->ChannelID.i] = std::move(muted);
    co_return;
  };

  s2c(Channel_Chat_Message) {
    s2c_read(Channel_Chat_Message);
    if (!backend.is_channel_joined(msg->ChannelID)) {
      co_return;
    }
    ecps_backend_t::chat_message_t message;
    message.id = msg->Id;
    message.from = (std::uint32_t)msg->From.i;
    message.name = std::string(msg->Name, strnlen(msg->Name, sizeof(msg->Name)));
    message.time = msg->Time;
    message.text = std::string(msg->Text, std::min<std::size_t>(msg->Length, sizeof(msg->Text)));
    backend.on_chat_message(msg->ChannelID, std::move(message));
    co_return;
  };

  s2c(Channel_Chat_History_End) {
    s2c_read(Channel_Chat_History_End);
    if (backend.is_channel_joined(msg->ChannelID)) {
      backend.on_chat_known(msg->ChannelID);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_View_Streamers) {
    s2c_read(Channel_ScreenShare_View_Streamers);
    std::vector<std::uint32_t> streamers;
    for (std::uint8_t i = 0; i < msg->Count && i < 8; ++i) {
      streamers.push_back((std::uint32_t)msg->Streamers[i].i);
    }
    std::lock_guard<std::mutex> lock(backend.streamers_mutex);
    backend.channel_streamers[(std::uint32_t)msg->ChannelID.i] = std::move(streamers);
    co_return;
  };

  s2c(SetUsername_Result) {
    s2c_read(SetUsername_Result);
    std::string name((const char*)msg->Name, strnlen((const char*)msg->Name, sizeof(msg->Name)));
    {
      std::lock_guard<std::mutex> lock(backend.username_mutex);
      backend.username_result_name = name;
      if (!name.empty()) {
        backend.my_username = name;
      }
    }
    backend.username_result.store(msg->Result);
    backend.username_result_seq.fetch_add(1);

    // the first one comes unasked right after login and shows this is a newer server: now the saved name can be
    // asked for, and the other newer messages may be sent
    if (!backend.server_new.exchange(true) && !backend.desired_username.empty() && backend.desired_username != name) {
      co_await backend.send_set_username(backend.desired_username);
    }
  };

  s2c(Channel_ScreenShare_View_InformationToViewCursor) {
    s2c_read(Channel_ScreenShare_View_InformationToViewCursor);
    auto* rt = get_render_thread();
    if (rt && backend.is_channel_viewing(msg->ChannelID)) {
      rt->host_cursor.x.store(msg->X / 65535.f);
      rt->host_cursor.y.store(msg->Y / 65535.f);
      rt->host_cursor.visible.store(msg->Visible != 0);
      rt->host_cursor.updated_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMouseCoordinate) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMouseCoordinate);
    std::int32_t x, y;
    if (host_allows_input(backend, msg->ChannelID) && stream_to_desktop(msg->pos, x, y)) {
      host_input.move(x, y);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMouseMotion) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMouseMotion);
    if (host_allows_input(backend, msg->ChannelID)) {
      host_input.move_relative(msg->Motion.x, msg->Motion.y);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMouseButton) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMouseButton);
    if (host_allows_input(backend, msg->ChannelID)) {
      std::int32_t x, y;
      if (stream_to_desktop(msg->pos, x, y)) {
        host_input.move(x, y);
      }
      host_input.button(msg->key, msg->state);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostKeyboard) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostKeyboard);
    if (host_allows_input(backend, msg->ChannelID) && !host_input.key(msg->Scancode, msg->State)) {
      ecps_print_throttled("input control: can't press scancode {:#x}", msg->Scancode);
    }
    co_return;
  };

  s2c(Channel_ScreenShare_Share_ApplyToHostMidi) {
    s2c_read(Channel_ScreenShare_Share_ApplyToHostMidi);
    auto* rt = get_render_thread();
    // the host's own setting decides, and only for a channel it hosts. No stream needed
    play_midi(rt && rt->ecps_gui.allow_midi && backend.is_hosting_channel(msg->ChannelID), msg->Status, msg->Data1, msg->Data2);
    co_return;
  };

  s2c(Channel_ScreenShare_View_InformationToViewMidi) {
    s2c_read(Channel_ScreenShare_View_InformationToViewMidi);
    auto* rt = get_render_thread();
    // being in the channel is enough, the host doesn't have to be streaming
    play_midi(rt && rt->ecps_gui.play_host_midi && backend.is_channel_joined(msg->ChannelID) &&
      !backend.is_hosting_channel(msg->ChannelID), msg->Status, msg->Data1, msg->Data2);
    co_return;
  };

  s2c(Channel_Log_Request) {
    s2c_read(Channel_Log_Request);
    co_await remote_logs.on_request(msg.data);
  };
  s2c_forward(Channel_Log_Status, remote_logs.on_status(msg.data));
  s2c_forward(Channel_Log_Data, remote_logs.on_data(msg.data));

  // file transfer
  s2c(Channel_File_Offer) {
    s2c_read(Channel_File_Offer);
    co_await backend.on_file_offer(msg.data);
  };
  s2c(Channel_File_Chunk) {
    s2c_read(Channel_File_Chunk);
    co_await backend.on_file_chunk(msg.data);
  };
  s2c_forward(Channel_File_Response, backend.on_file_response(msg.data));
  s2c_forward(Channel_File_Ack, backend.on_file_ack(msg.data));
  s2c_forward(Channel_File_Cancel, backend.on_file_cancel(msg.data));

  // a channel closed (its host left) or we were put out of it
  s2c_forward(KickedFromChannel, backend.forget_channel(msg->ChannelID));
}
#undef s2c
#undef s2c_read
#undef s2c_forward

fan::event::task_t ecps_backend_t::default_s2c_cb(ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) {
  co_await backend.tcp_client.read(backend.Protocol_S2C.NA(base.Command)->m_DSS);
  co_return;
}

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

static void decode_frame(render_thread_t* rt, std::vector<uint8_t>& frame_data) {
  try {
    auto frame = rt->screen_decoder.decode(frame_data.data(), frame_data.size(), rt->network_frame);
    if (frame.type == 1 && is_displayable(frame)) {
      rt->latest_frame.publish(frame);
      rt->screen_decoder.recycle(frame);
      ecps_backend.view_frames_decoded.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  catch (const std::exception& e) {
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Decode exception: {}", e.what());
  #endif
  }
  ecps_backend.view_decode_failed.fetch_add(1, std::memory_order_relaxed);
  ecps_backend.view.m_stats.Frame_Drop++;
}

void ecps_backend_t::view_t::WriteFramePacket(const frame_assembler_t::frame_t& frame) {
  if (frame.size < 32 || frame.size > 0x500000) {
    this->m_stats.Frame_Drop++;
  #if ecps_debug_prints >= 1
    ecps_print_throttled("NETWORK: Frame dropped - invalid size: {} bytes (valid range: 32 - 5MB)", frame.size);
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
      ecps_backend.view_need_keyframe.store(false, std::memory_order_relaxed);
      m_KeyFrameWaitSince = 0;
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
  if (rt && rt->ecps_gui.stream_settings.bitrate_mode == 1) {
    return rt->ecps_gui.stream_settings.bitrate_mbps * 1000000;
  }

  uint64_t bitrate = pick_by_fps(25000000, 20000000, 15000000, 12000000, 8000000);
  // the table is for 1080p. More pixels need more bits, but less than in proportion: 1440p gets 1.33x, 4k 2x
  if (rt) {
    f64_t pixels = (f64_t)rt->screen_encoder.config_.width * rt->screen_encoder.config_.height;
    bitrate = (uint64_t)(bitrate * std::clamp(std::sqrt(pixels / (1920.0 * 1080.0)), 1.0, 3.0));
  }
  return bitrate;
}

int main() {
  // from here on everything printed can be read by another client that is allowed to
  log_capture().start();
  fan::print(std::string("ecps client v") + ecps_version);
  ecps_backend.reconnected_cb = [](ecps_backend_t::Protocol_ChannelID_t channel_id) {
    if (auto* rt = render_thread_ptr.load(std::memory_order_acquire)) {
      rt->ecps_gui.selected_channel_id = channel_id;
    }
  };
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
        try {
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

          co_await ecps_backend.udp_write(
            0,
            ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_RecoveryRequest,
            rest,
            request_data.data(),
            request_data.size()
          );
        }
        catch (...) {}
      });
    }
  };

  ecps_backend.share.resend_packet_callback = [](const std::vector<uint8_t>& packet_data) {
    auto* rt = get_render_thread();
    if (rt) {
      rt->ecps_gui.backend_queue([packet_data]() -> fan::event::task_t {
        try {
          auto* channel = ecps_backend.streaming_channel();
          if (!channel) {
            co_return;
          }
          ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t rest;
          rest.ChannelID = channel->channel_id;
          rest.ChannelSessionID = channel->session_id;

          co_await ecps_backend.udp_write(
            0,
            ecps_backend_t::ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData,
            rest,
            packet_data.data(),
            packet_data.size()
          );
        }
        catch (...) {}
      });
    }
  };

  std::promise<void> render_thread_promise;
  std::future<void> render_thread_future = render_thread_promise.get_future();

  // the engine builds its gui fonts on the render thread, so load the emojis before it starts
  fan::graphics::gui::load_emojis();

  fan::event::thread_create([&render_thread_promise] {
    render_thread_t render_thread_instance;

    render_thread_ptr.store(&render_thread_instance, std::memory_order_release);
    render_thread_ready.store(true, std::memory_order_release);
    render_thread_promise.set_value();

    while (!render_thread_instance.screen_decoder.is_initialized() &&
      !render_thread_instance.should_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (!render_thread_instance.engine.should_close() && !render_thread_instance.should_stop.load()) {
      {
        std::lock_guard<std::mutex> lock(render_thread_instance.screen_decoder.mutex);
        for (auto& i : render_thread_instance.screen_decoder.graphics_queue) {
          i();
        }
        render_thread_instance.screen_decoder.graphics_queue.clear();
      }

      // uploaded outside the lock, the decoder doesn't wait for the gpu
      static fan::graphics::screen_decode_t::decode_data_t shown_frame;
      if (render_thread_instance.latest_frame.take(shown_frame)) {
        static fan::time::timer display_fps_timer(1e+9, true);
        static int display_frame_count = 0;

        display_frame_count++;
        if (display_fps_timer.elapsed() >= 1e+9) {
          render_thread_instance.displayed_fps = static_cast<float>(display_frame_count) * 1e+9 / display_fps_timer.elapsed();
          display_frame_count = 0;
          display_fps_timer.restart();
        }

        if (ecps_backend.did_just_join && !ecps_backend.is_current_user_host_of_channel(ecps_backend.channel_info.back().channel_id)) {
          render_thread_instance.ecps_gui.window_handler.main_tab = 1;
          ecps_backend.did_just_join = false;
        }

        std::array<void*, 4> planes;
        for (size_t i = 0; i < planes.size(); ++i) {
          planes[i] = shown_frame.data[i].empty() ? nullptr : shown_frame.data[i].data();
        }
        try {
          stage_scope_t upload_scope {stage_times.upload};
          if (shown_frame.gpu_slot >= 0) {
            // decoded on the GPU and still there: copied inside the GPU into the picture, rows tightly packed
            if (render_thread_instance.screen_decoder.show_gpu_frame(shown_frame, render_thread_instance.network_frame)) {
              render_thread_instance.network_frame.set_tc_size(fan::vec2(1, 1));
            }
          }
          else {
            render_thread_instance.network_frame.set_tc_size(fan::vec2((f32_t)shown_frame.image_size.x / shown_frame.stride[0].x, 1));
            render_thread_instance.network_frame.reload(
              shown_frame.pixel_format,
              planes.data(),
              fan::vec2ui(shown_frame.stride[0].x, shown_frame.image_size.y)
            );
          }
        }
        catch (const std::exception& e) {
        #if ecps_debug_prints >= 1
          ecps_print_throttled("RENDER: frame upload failed: {}", e.what());
        #endif
        }
      }

      render_thread_instance.render([] {});
      notify_sounds.tick(render_thread_instance.ecps_gui.stream_settings.notify_volume);
      {
        static auto second_start = std::chrono::steady_clock::now();
        static auto last_frame = second_start;
        static int frames = 0;
        static f32_t slowest_ms = 0.f;
        auto now = std::chrono::steady_clock::now();
        slowest_ms = std::max(slowest_ms, std::chrono::duration<f32_t, std::milli>(now - last_frame).count());
        last_frame = now;
        ++frames;
        f32_t seconds = std::chrono::duration<f32_t>(now - second_start).count();
        if (seconds >= 1.f) {
          draw_rate.fps.store(frames / seconds, std::memory_order_relaxed);
          draw_rate.slowest_ms.store(slowest_ms, std::memory_order_relaxed);
          frames = 0;
          slowest_ms = 0.f;
          second_start = now;
        }
      }

      // In the background (another program in front, a game for example) the window draws only as often as the
      // stream needs, with room to spare: every decoded frame still shows, and the rest of the monitor's refresh
      // rate (165 draws a second on a 165 Hz screen) is GPU time left to the game. The Target Framerate setting caps
      // it in front too: the engine's own frame timer isn't used, this thread draws the frames
      {
        static auto last_draw = std::chrono::steady_clock::now();
        f64_t wanted = 0.0;
        if (!render_thread_instance.engine.window.is_focused()) {
          f32_t stream_fps = render_thread_instance.ecps_gui.debug_sample.decoded_fps;
          wanted = std::max(30.0, stream_fps * 1.25 + 5.0);
        }
        if (int target = render_thread_instance.engine.target_fps; target > 0) {
          wanted = wanted > 0.0 ? std::min(wanted, (f64_t)target) : (f64_t)target;
        }
        if (wanted > 0.0) {
          auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<f64_t>(1.0 / wanted));
          auto since = std::chrono::steady_clock::now() - last_draw;
          if (since < interval) {
            std::this_thread::sleep_for(interval - since);
          }
        }
        last_draw = std::chrono::steady_clock::now();
      }

      std::this_thread::yield();
    }

    // the workers hold a pointer to render_thread_instance, it may only go away after they did
    stop_worker_threads();
    render_thread_ptr.store(nullptr, std::memory_order_release);
    render_thread_ready.store(false, std::memory_order_release);
  });

  render_thread_future.wait();


  // counted before the thread exists, so the render thread can't miss a worker that is about to start
  worker_threads_active.fetch_add(1);
  fan::event::thread_create([] {
    worker_thread_scope_t worker_scope;
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    while (!rt || !rt->screen_encoder.encoder_.is_initialized()) {
      if (app_stopping.load()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rt = render_thread_ptr.load(std::memory_order_acquire);
    }

    lower_thread_priority();

    uint64_t frame_id = 0;
    bool first_frame = true;
    auto last_idr_time = std::chrono::steady_clock::now();
    auto last_backlog_idr = last_idr_time;
    auto& flow = ecps_backend.share.m_NetworkFlow;

    // one line every few seconds while sharing, says where frames stop: capture, encode or the network
    struct share_stats_t {
      uint64_t read_ok = 0, read_failed = 0, encode_failed = 0, frames = 0, bytes = 0;
      std::chrono::steady_clock::time_point printed = std::chrono::steady_clock::now();
    } stats;
    auto report_stats = [&] {
      auto stamp = std::chrono::steady_clock::now();
      if (stamp - stats.printed < std::chrono::seconds(3)) {
        return;
      }
      stats.printed = stamp;
      fan::print("share: captured", stats.read_ok, "capture_failed", stats.read_failed,
        "encode_failed", stats.encode_failed, "encoded", stats.frames, "(", stats.bytes, "bytes ) udp_packets",
        ecps_backend.stream_udp_packets.load(), "udp_bytes", ecps_backend.stream_udp_bytes.load(),
        "queued", flow.FrameList.Usage());
    };

    bool was_streaming = false;
    while (!app_stopping.load()) {
      rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt || rt->should_stop.load()) break;

      if (ecps_backend.is_streaming_to_any_channel()) {
        was_streaming = true;
      #if ecps_debug_prints >= 1
        report_stats();
      #endif
        if (first_frame) {
          rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
          first_frame = false;
          last_idr_time = std::chrono::steady_clock::now();
        }

        auto now = std::chrono::steady_clock::now();
        auto time_since_idr = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_idr_time);
        // a keyframe now and then repairs what a lost frame broke for a viewer that didn't ask. With intra refresh
        // the picture is renewed all the time anyway, and an extra keyframe would only be the burst it avoids
        if (time_since_idr > std::chrono::milliseconds(5000) && !rt->screen_encoder.encoder_.uses_intra_refresh()) {
          rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
          last_idr_time = now;
        }

      #if !defined(_WIN32)
        // "Change..." while sharing: the next read opens the share dialog again
        if (rt->ecps_gui.capture_reselect.exchange(false)) {
          close_capture(rt->screen_encoder);
        }
      #endif
        // waits for the next frame of the desktop, but a failed read returns at once
        if (!rt->screen_encoder.screen_read()) {
        #if !defined(_WIN32)
          if (rt->screen_encoder.take_capture_open_failed()) {
            rt->ecps_gui.capture_armed = false;
          }
        #endif
          ++stats.read_failed;
          rt->screen_encoder.sleep_thread();
          continue;
        }
        ++stats.read_ok;
        stage_times.capture.add(rt->screen_encoder.capture_time_ns);
      #if defined(_WIN32)
        if (rt->ecps_gui.capture_preview_due()) {
          auto& encoder = rt->screen_encoder;
          const std::uint8_t* frame = encoder.screen_buffer ? encoder.screen_buffer : encoder.mdscr.map_for_cpu(true);
          auto& geometry = encoder.mdscr.Geometry;
          rt->ecps_gui.store_capture_preview_now(frame, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
        }
      #endif
      #if !defined(_WIN32)
        {
          auto& geometry = rt->screen_encoder.mdscr.Geometry;
          rt->ecps_gui.store_capture_preview(rt->screen_encoder.screen_buffer, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
        }
      #endif

        {
          // the encoder is set to a landscape size, a portrait screen squeezed into it comes out stretched.
          // Keep the encoder's orientation the same as the capture's, the pixel count stays what was chosen
          auto& geometry = rt->screen_encoder.mdscr.Geometry;
          auto& encoder_config = rt->screen_encoder.config_;
          if (geometry.Resolution.x > 0 && geometry.Resolution.y > 0 && encoder_config.width > 0 && encoder_config.height > 0) {
            bool capture_portrait = geometry.Resolution.y > geometry.Resolution.x;
            bool encoder_portrait = encoder_config.height > encoder_config.width;
            if (capture_portrait != encoder_portrait) {
              rt->screen_encoder.set_user_resolution(encoder_config.height, encoder_config.width);
              rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
            }
          }
        }

        bool encoded;
        {
          stage_scope_t encode_scope {stage_times.encode};
          encoded = rt->screen_encoder.encode_write();
        }
        if (!encoded) {
          ++stats.encode_failed;
          rt->screen_encoder.sleep_thread();
          continue;
        }

      #if ecps_debug_prints >= 1
        {
          // one-off dump of the captured screen, to tell capture problems from encode/render ones
          static int capture_dump_frame = 0;
          auto& geometry = rt->screen_encoder.mdscr.Geometry;
          if (++capture_dump_frame == 60 && rt->screen_encoder.screen_buffer && geometry.Resolution.x && geometry.Resolution.y) {
            int dump_width = geometry.LineSize ? (int)geometry.LineSize / 4 : (int)geometry.Resolution.x;
            fan::image::write("ecps_capture.png", rt->screen_encoder.screen_buffer,
              fan::vec2i(dump_width, (int)geometry.Resolution.y), 4);
            fan::print("DEBUG: wrote ecps_capture.png (" + std::to_string(geometry.Resolution.x) + "x" +
              std::to_string(geometry.Resolution.y) + ", line size " + std::to_string(geometry.LineSize) + ")");
          }
        }
      #endif

        uint8_t* encoded_data = nullptr;
        size_t encoded_size = rt->screen_encoder.encode_read(&encoded_data);

        if (encoded_size > 0 && encoded_data) {
          ++stats.frames;
          ecps_backend.share_frames_encoded.fetch_add(1, std::memory_order_relaxed);
          stats.bytes += encoded_size;
          if (rt->ecps_gui.show_own_stream &&
            !decode_queue.push(std::vector<uint8_t>(encoded_data, encoded_data + encoded_size))) {
            rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
          }

          // encode_write already cleared the force_keyframe flag, the encoded frame is the source of truth
          const bool is_keyframe = rt->screen_encoder.current_packet_.is_keyframe;

          // the frames after this one depend on it, losing it silently would corrupt the picture
          std::unique_lock<std::timed_mutex> frame_list_lock(ecps_backend.share.frame_list_mutex, std::chrono::milliseconds(20));
          if (frame_list_lock.owns_lock()) {
            // a backlog is latency on the viewer side, a keyframe lets the queue be dropped safely
            auto backlog_now = std::chrono::steady_clock::now();
            // a few frames waiting is a burst the sender catches up on, dropping them all for a keyframe is worse:
            // only a queue of about a sixth of a second is let go
            std::size_t backlog_limit = std::max<std::size_t>(dynamic_config_t::get_adaptive_queue_size(),
              dynamic_config_t::get_target_framerate() / 6);
            if (!is_keyframe && flow.FrameList.Usage() > backlog_limit &&
              backlog_now - last_backlog_idr > std::chrono::seconds(1)) {
              rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
              last_backlog_idr = backlog_now;
              send_debug.backlog_idrs.fetch_add(1, std::memory_order_relaxed);
            }

            if (is_keyframe) {
              // a partly sent frame is dropped here, its sequence must not be reused
              auto head = flow.FrameList.GetNodeFirst();
              if (head != flow.FrameList.dst && flow.FrameList[head].SentOffset) {
                ++ecps_backend.share.frame_index;
              }
              send_debug.flushed.fetch_add(flow.FrameList.Usage(), std::memory_order_relaxed);
              while (flow.FrameList.Usage() > 0) {
                flow.FrameList.unlrec(flow.FrameList.GetNodeFirst());
              }
            }

            auto f = &flow.FrameList[flow.FrameList.NewNodeLast()];
            // current_packet_ is only read above, its buffer goes to the queue without a copy
            f->vec = std::move(rt->screen_encoder.current_packet_.data);
            f->SentOffset = 0;
            f->keyframe = is_keyframe;
            send_debug.queued.fetch_add(1, std::memory_order_relaxed);
            send_debug.queued_bytes_total.fetch_add(f->vec.size(), std::memory_order_relaxed);
            send_debug_t::raise(send_debug.max_queue, flow.FrameList.Usage());
          }
          else {
            rt->screen_encoder.encode_write_flags |= fan::graphics::codec_update_e::force_keyframe;
            send_debug.lock_misses.fetch_add(1, std::memory_order_relaxed);
          }
        }

        rt->screen_encoder.sleep_thread();
      }
      else {
        if (was_streaming) {
          // stopped: let go of the capture, so the next Start asks which screen or window to share again.
          // A source chosen with "Choose..." is kept, it is what the next share uses
          was_streaming = false;
        #if !defined(_WIN32)
          if (!rt->ecps_gui.capture_armed.load() || rt->screen_encoder.is_camera())
        #endif
          close_capture(rt->screen_encoder);
        }
        first_frame = true;
      #if defined(_WIN32)
        if (rt->ecps_gui.capture_preview_wanted.load()) {
          // not sharing, but the preview shows: frames are captured (with the HDR tone mapping) only for it, not
          // encoded, so it shows what would go out
          auto& encoder = rt->screen_encoder;
          if (encoder.screen_read() && rt->ecps_gui.capture_preview_due()) {
            const std::uint8_t* frame = encoder.screen_buffer ? encoder.screen_buffer : encoder.mdscr.map_for_cpu(true);
            auto& geometry = encoder.mdscr.Geometry;
            rt->ecps_gui.store_capture_preview_now(frame, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(std::max(5, 500 / std::clamp(rt->ecps_gui.preview_fps, 1, 60))));
          continue;
        }
      #endif
      #if !defined(_WIN32)
        if (rt->ecps_gui.capture_reselect.exchange(false)) {
          close_capture(rt->screen_encoder);
        }
        if (rt->ecps_gui.capture_armed.load() && !rt->screen_encoder.is_camera()) {
          // chosen but not shared yet: the first read opens the share dialog, the frames after it only feed the preview
          if (rt->screen_encoder.screen_read()) {
            auto& geometry = rt->screen_encoder.mdscr.Geometry;
            rt->ecps_gui.store_capture_preview(rt->screen_encoder.screen_buffer, geometry.Resolution.x, geometry.Resolution.y, geometry.LineSize);
          }
          else if (rt->screen_encoder.take_capture_open_failed()) {
            // the portal dialog was cancelled: stay cancelled instead of asking again every 100 ms
            rt->ecps_gui.capture_armed = false;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
      #endif
        rt->screen_encoder.poll_source();
        if (rt->screen_encoder.is_camera()) {
          close_capture(rt->screen_encoder);
        }
        const int fps = std::max<int>(rt->screen_encoder.config_.frame_rate, 1);
        const auto sleep_time = std::chrono::microseconds(1000000 / fps);
        std::this_thread::sleep_for(sleep_time);
      }
    }
  });

  worker_threads_active.fetch_add(1);
  fan::event::thread_create([] {
    worker_thread_scope_t worker_scope;
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    while (!rt || !rt->screen_decoder.is_initialized()) {
      if (app_stopping.load()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      rt = render_thread_ptr.load(std::memory_order_acquire);
    }

    lower_thread_priority();

    std::vector<uint8_t> frame;
    auto view_report_time = std::chrono::steady_clock::now();
    uint64_t view_reported_packets = 0;
    while (!app_stopping.load()) {
      rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt || rt->should_stop.load()) break;

    #if ecps_debug_prints >= 1
      // while something is being received, one line every few seconds says how far the frames get
      if (auto stamp = std::chrono::steady_clock::now(); stamp - view_report_time >= std::chrono::seconds(3)) {
        view_report_time = stamp;
        uint64_t packets = ecps_backend.view_udp_packets.load();
        if (packets != view_reported_packets) {
          view_reported_packets = packets;
          fan::print("view: udp_packets", packets, "udp_bytes", ecps_backend.view_udp_bytes.load(),
            "frames_assembled", ecps_backend.view_frames_assembled.load(),
            "decoded", ecps_backend.view_frames_decoded.load(),
            "decode_failed", ecps_backend.view_decode_failed.load());
        }
      }
    #endif

      if (decode_queue.pop(frame)) {
        stage_scope_t decode_scope {stage_times.decode};
        decode_frame(rt, frame);
      }
    }
  });

  fan::event::task_idle([]() -> fan::event::task_t {
    try {
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (!rt || rt->should_stop.load()) {
        co_return;
      }

      constexpr size_t MAX_BATCH_SIZE = 4;
      std::vector<std::function<fan::event::task_t()>> local_tasks;

      {
        // backend_queue() pushes under task_queue_mutex, from the gui and the network thread
        std::lock_guard<std::mutex> task_lock(rt->ecps_gui.task_queue_mutex);
        if (!rt->ecps_gui.task_queue.empty()) {
          size_t batch_size = std::min(rt->ecps_gui.task_queue.size(), MAX_BATCH_SIZE);
          auto first = rt->ecps_gui.task_queue.begin();
          local_tasks.assign(std::make_move_iterator(first), std::make_move_iterator(first + batch_size));
          rt->ecps_gui.task_queue.erase(first, first + batch_size);
        }
      }

      for (const auto& f : local_tasks) {
        co_await f();
      }

      if (local_tasks.empty()) {
        co_await fan::co_sleep(1);
      }
    }
    catch (...) {
      fan::print("error");
    }
  });

  auto network_task = fan::event::every(1, []() -> fan::event::runv_t<bool> {
    // lost video packets are asked for and given up on with time, not only when the next packet comes
    ecps_backend.view.assembler.set_rtt_ms(ecps_backend.ping_ms);
    ecps_backend.view.assembler.poll(fan::event::now());
    ecps_backend.view.SyncStats();
    co_await audio_share.send_pending();
    if (voice_chat.has_work()) {
      co_await voice_chat.sync();
    }
    auto* rt = render_thread_ptr.load(std::memory_order_acquire);
    if (rt) {
      if (rt->ecps_gui.window_handler.auto_refresh) {
        static fan::time::timer refresh_timer(rt->ecps_gui.window_handler.refresh_interval * 2e+9, true);
        if (refresh_timer.finished()) {
          // the user list of the open channel needs refreshing too, otherwise
          // people who join after it was opened never show up
          auto selected_channel_id = rt->ecps_gui.selected_channel_id;
          rt->ecps_gui.backend_queue([selected_channel_id]() -> fan::event::task_t {
            try {
              co_await ecps_backend.request_channel_list();
              if (selected_channel_id.i != (uint16_t)-1) {
                co_await ecps_backend.request_channel_session_list(selected_channel_id);
              }
            }
            catch (...) {}
          });
          refresh_timer.restart();
        }
      }
    }

    if (ecps_backend.channel_info.empty()) {
      co_return 0;
    }

    static uint64_t skip_counter = 0;
    skip_counter++;

    if (skip_counter % 5 != 0) {
      auto flnr_check = ecps_backend.share.m_NetworkFlow.FrameList.GetNodeFirst();
      if (flnr_check == ecps_backend.share.m_NetworkFlow.FrameList.dst) {
        co_return 0;
      }
    }

    std::unique_lock<std::timed_mutex> frame_list_lock(ecps_backend.share.frame_list_mutex, std::try_to_lock);
    if (!frame_list_lock.owns_lock()) {
      co_return 0;
    }

    uint64_t ctime = fan::event::now();
    uint64_t DeltaTime = ctime - ecps_backend.share.m_NetworkFlow.TimerLastCallAt;

    if (DeltaTime < 100000) {
      frame_list_lock.unlock();
      co_return 0;
    }

    ecps_backend.share.m_NetworkFlow.TimerLastCallAt = ctime;

    f32_t bucket_multiplier = dynamic_config_t::get_adaptive_bucket_multiplier();

    if (!rt) {
      co_return 0;
    }

    // The pacing smooths bursts, it must not hold the stream below what the encoder makes: the encoder can go over its
    // target (high frame rates, keyframes), and a pace below that only fills the queue until frames are thrown away.
    // So the rate is the target or what came out of the encoder over the last half second, whichever is higher
    static uint64_t rate_window_start = ctime;
    static uint64_t rate_window_bytes = send_debug.queued_bytes_total.load(std::memory_order_relaxed);
    static double encoder_bps = 0.0;
    if (ctime - rate_window_start >= 500000000ull) {
      uint64_t total = send_debug.queued_bytes_total.load(std::memory_order_relaxed);
      encoder_bps = (total - rate_window_bytes) * 8.0 / ((ctime - rate_window_start) / 1e9);
      rate_window_bytes = total;
      rate_window_start = ctime;
    }
    double pace_bps = std::max<double>((double)rt->screen_encoder.config_.bitrate, encoder_bps);

    double seconds_elapsed = static_cast<double>(DeltaTime) / 1000000000.0;
    double bits_to_add = seconds_elapsed * pace_bps * bucket_multiplier;

    auto& flow = ecps_backend.share.m_NetworkFlow;
    // A keyframe goes out on credit (the bucket below zero), and every frame after it waited until the debt was paid
    // at the stream's own rate: up to a quarter second of held frames, a visible hitch after each keyframe. The link
    // is faster than the stream's average, so the debt is paid back at three times the pace
    if (flow.Bucket < 0) {
      bits_to_add *= 3.0;
    }
    // room for a quarter second at the pace, so a keyframe's debt is paid back in time
    flow.BucketSize = std::max<uint64_t>(dynamic_config_t::get_adaptive_bucket_size(), (uint64_t)(pace_bps / 4));
    flow.Bucket = std::min(flow.Bucket + static_cast<int64_t>(bits_to_add), static_cast<int64_t>(flow.BucketSize));

    if (flow.FrameList.Usage() > 0) {
      send_debug_t::raise(send_debug.max_gap_us, DeltaTime / 1000);
    }

    // several frames can go in one tick, a late tick must not lower the frame rate. The limit is what the bitrate
    // budget holds, not a fixed packet count: a fixed count capped the upload at count x ticks per second
    const size_t max_chunks = std::clamp<size_t>(
      (size_t)std::max<int64_t>(flow.Bucket, 0) / (0x400 * 8) + 1, dynamic_config_t::get_adaptive_chunk_count(), 512);
    size_t chunks_left = max_chunks;
    auto pass_start = std::chrono::steady_clock::now();
    std::uint64_t pass_chunks = 0;
    bool bucket_stop = false;

    while (chunks_left > 0) {
      auto flnr = flow.FrameList.GetNodeFirst();
      if (flnr == flow.FrameList.dst) {
        break;
      }

      auto f = &flow.FrameList[flnr];

      uint8_t Flag = f->keyframe ? ecps_backend_t::ProtocolChannel::ScreenShare::StreamHeadFlag::KeyFrame : 0;
      uint16_t Possible = (f->vec.size() / 0x400) + !!(f->vec.size() % 0x400);
      uint16_t sent_offset = f->SentOffset;
      bool is_likely_keyframe = f->keyframe || f->vec.size() > 30000;
      int64_t min_bucket = is_likely_keyframe ? -static_cast<int64_t>(flow.BucketSize) : 0;
      if (is_likely_keyframe) {
        chunks_left += max_chunks;
      }

      bool blocked = false;
      for (; sent_offset < Possible && chunks_left > 0; sent_offset++, chunks_left--) {
        uintptr_t DataSize = std::min<uintptr_t>(f->vec.size() - sent_offset * 0x400, 0x400);
        if (flow.Bucket - static_cast<int64_t>(DataSize * 8) < min_bucket) {
          bucket_stop = true;
          blocked = true;
          break;
        }
        if (co_await ecps_backend.write_stream(sent_offset, Possible, Flag, &f->vec[sent_offset * 0x400], DataSize)) {
          blocked = true;
          break;
        }
        flow.Bucket -= static_cast<int64_t>(DataSize * 8);
        ++pass_chunks;
      }

      f->SentOffset = sent_offset;

      if (blocked || sent_offset < Possible) {
        break;
      }
      flow.FrameList.unlrec(flnr);
      ++ecps_backend.share.frame_index;
      send_debug.sent.fetch_add(1, std::memory_order_relaxed);
    }

    if (pass_chunks || bucket_stop) {
      auto pass_us = (std::uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pass_start).count();
      send_debug.passes.fetch_add(1, std::memory_order_relaxed);
      send_debug.chunks.fetch_add(pass_chunks, std::memory_order_relaxed);
      send_debug.send_us.fetch_add(pass_us, std::memory_order_relaxed);
      send_debug_t::raise(send_debug.max_send_us, pass_us);
      if (bucket_stop) {
        send_debug.bucket_stops.fetch_add(1, std::memory_order_relaxed);
      }
      else if (chunks_left == 0 && flow.FrameList.Usage() > 0) {
        send_debug.cap_stops.fetch_add(1, std::memory_order_relaxed);
      }
    }

    co_return 0;
  });
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
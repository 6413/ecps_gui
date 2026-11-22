// What the client does with each message from the server: ecps_backend_t's constructor fills the table of
// handlers. Split out of ecps.cpp so an edit to one part recompiles less
#include "gui_std.h" // precompiled, first
#include "app_state.h"
#include "ecps_threads.h"


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
      // you got in: the same sound the others hear for you
      notify_sounds.request(notify_sounds_t::join);
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

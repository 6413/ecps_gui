struct ecps_backend_t {

#define __ecps_client
#include "../prot.h"

  ecps_backend_t();
  ~ecps_backend_t() {
    should_stop_tcp_read.store(true);
    task_supervisor = {};
    task_restore = {};
    task_keep_alive = {};
    task_tcp_read = {};
    task_udp_listen = {};
  }

  struct channel_create_awaiter {
    ecps_backend_t& backend;
    uint32_t request_id;

    channel_create_awaiter(ecps_backend_t& b, uint32_t id) : backend(b), request_id(id) {}

    bool await_ready() const noexcept {
      auto it = backend.pending_requests.find(request_id);
      return it != backend.pending_requests.end() && it->second.completed;
    }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      auto it = backend.pending_requests.find(request_id);
      if (it != backend.pending_requests.end()) {
        it->second.continuation = h;
      }
    }
    uint16_t await_resume() {
      auto it = backend.pending_requests.find(request_id);
      if (it != backend.pending_requests.end()) {
        uint16_t channel_id = it->second.channel_id;
        backend.pending_requests.erase(it);
        return channel_id;
      }
      throw std::runtime_error("Channel creation request not found");
    }
  };
  std::atomic<bool> should_stop_tcp_read{ false };

  fan::event::task_t tcp_read() {
    while (!should_stop_tcp_read.load()) {
      fan::network::typed_message_t<tcp::ProtocolBasePacket_t> msg;
      try {
        msg = co_await tcp_client.read<tcp::ProtocolBasePacket_t>();
      }
      catch (const std::exception& e) {
        mark_disconnected(e.what());
        co_return;
      }
      catch (const fan::exception_t& e) {
        mark_disconnected(e.reason);
        co_return;
      }
      if (should_stop_tcp_read.load()) {
        co_return;
      }
      if (msg.status != 0 || msg->Command >= Protocol_S2C.size()) {
        mark_disconnected(msg.status != 0 ? "connection lost" : "invalid command");
        co_return;
      }
      last_rx = fan::event::now();
      try {
        co_await(*Protocol_S2C.NA(msg->Command))(*this, msg.data);
      }
      catch (const std::exception& e) {
        fan::print("Message processing error: " + std::string(e.what()));
      }
    }
  }
  fan::event::runv_t<uint32_t> tcp_write(int command, void* data, uint32_t len) {
    while (command != Protocol_C2S_t::KeyExchange && !tcp_secure_ready) {
      if (!connected) {
        fan::throw_error("not connected");
      }
      co_await fan::co_sleep(5);
    }
    static int id = 0;
    tcp::ProtocolBasePacket_t bp;
    bp.ID = id++;
    bp.Command = command;
    // header and body in one write: several coroutines send concurrently (auto-refresh,
    // gui actions, keep-alive), and two separate writes can interleave on the stream,
    // which desyncs the server's parser for the rest of the connection
    fan::network::buffer_t packet(sizeof(bp) + len);
    std::memcpy(packet.data(), &bp, sizeof(bp));
    if (len) {
      std::memcpy(packet.data() + sizeof(bp), data, len);
    }
    co_await tcp_client.write_raw(std::move(packet));
    co_return bp.ID;
  }
  template <typename T>
  fan::event::task_t udp_write(
    uint32_t ID,
    const T& Command,
    T CommandData,
    const void* data,
    uintptr_t size
  ) {
    uint8_t buffer[2048];
    auto BasePacket = (ProtocolUDP::BasePacket_t*)buffer;
    BasePacket->SessionID = session_id;
    BasePacket->ID = ID;
    BasePacket->Nonce = udp_crypto.send_nonce++;
    BasePacket->Command = Command;
    auto CommandDataPacket = (T*)&BasePacket[1];
    *CommandDataPacket = CommandData;
    auto RestPacket = (uint8_t*)CommandDataPacket + T::dss;
    __builtin_memcpy(RestPacket, data, size);
    uint16_t TotalSize = udp_crypto.seal(buffer, sizeof(ProtocolUDP::BasePacket_t), sizeof(ProtocolUDP::BasePacket_t) + T::dss + size, BasePacket->Nonce);
    if (!TotalSize) {
      co_return;
    }
    co_await udp_client.send(fan::network::buffer_t((uint8_t*)buffer, (uint8_t*)buffer + TotalSize), ip, port);
    //IO_ssize_t r = NET_sendto(&g_pile->UDP.udp, buffer, TotalSize, &g_pile->UDP.Address);
  }

  struct recovery_request_t {
    uint16_t sequence;
    uint16_t missing_count;
  };

  struct view_t {
    uint64_t frame_index = 0;
    uint16_t m_Sequence;
    uint16_t m_Possible;
    uint16_t m_ModuloSize;
    uint64_t frame_start_time = 0;
    uint64_t last_valid_packet_time = 0;

    std::vector<uint8_t> m_DataCheck = std::vector<uint8_t>(0x201);
    std::vector<uint8_t> m_data = std::vector<uint8_t>(0x400400);
    std::vector<uint16_t> m_MissingPackets;
    uint32_t m_ReceivedCount = 0;
    uint16_t m_HighestCurrent = 0;
    uint64_t m_GapSince = 0;
    uint64_t m_LastRecoveryAt = 0;
    uint8_t m_RecoveryRetries = 0;

    static constexpr uint64_t recovery_delay_ns = 5000000;
    std::function<void(std::vector<uint8_t>)> recovery_callback;

    struct {
      uint64_t Frame_Total;
      uint64_t Frame_Drop;
      uint64_t Packet_Total;
      uint64_t Packet_HeadDrop;
      uint64_t Packet_BodyDrop;
      uint64_t Frame_Corrected;
    } m_stats{};

    uint64_t get_adaptive_timeout() {
      f32_t ping_ms = 0;
      auto* rt = render_thread_ptr.load(std::memory_order_acquire);
      if (rt) {
        ping_ms = OFFSETLESS(this, ecps_backend_t, view)->ping_ms;
      }

      // Base timeout + (RTT * safety multiplier) + recovery time
      uint64_t base_timeout = 50000000ULL; // 50ms base
      uint64_t rtt_timeout = (uint64_t)(ping_ms * 2.0f * 1000000.0f); // 2x RTT in nanoseconds
      uint64_t recovery_time = 30000000ULL; // 30ms for recovery processing

      uint64_t total_timeout = base_timeout + rtt_timeout + recovery_time;

      // big frames (keyframes) are paced out by the sender's bandwidth bucket and can
      // take a few hundred ms to arrive; give each 1 KB packet 1.5 ms (~5.5 Mbit/s floor)
      if (m_Possible != (uint16_t)-1) {
        total_timeout += (uint64_t)m_Possible * 1500000ULL;
      }

      uint64_t min_timeout = 80000000ULL;
      uint64_t max_timeout = 1000000000ULL;

      return std::clamp(total_timeout, min_timeout, max_timeout);
    }

    void CheckAndSendRecoveryRequest() {
      uint64_t current_time = fan::event::now();

      if (last_valid_packet_time > 0 &&
        current_time - last_valid_packet_time > 5000000000ULL) {
      #if ecps_debug_prints >= 1
        fan::print("UDP: forcing sequence reset due to timeout");
      #endif
        m_Sequence = 0;
        last_valid_packet_time = current_time;
        RequestKeyframe();
        return;
      }

      if (frame_start_time > 0) {
        uint64_t frame_age = current_time - frame_start_time;
        uint64_t adaptive_timeout = get_adaptive_timeout();

        if (frame_age > adaptive_timeout) {
          m_stats.Frame_Total++;
          m_stats.Frame_Drop++;
          SetNewSequence((m_Sequence + 1) % 0x1000);
          RequestKeyframe();
          return;
        }
      }

      // packets arrive in order, so only a hole below the newest packet can be a loss;
      // anything above it is still on its way and asking for it just doubles the traffic
      if (m_Possible == (uint16_t)-1 || m_ReceivedCount > m_HighestCurrent) {
        m_GapSince = 0;
        m_RecoveryRetries = 0;
        return;
      }
      if (m_GapSince == 0) {
        m_GapSince = current_time;
      }
      if (current_time - m_GapSince < recovery_delay_ns || m_RecoveryRetries >= 3 ||
        current_time - m_LastRecoveryAt < recovery_delay_ns) {
        return;
      }

      UpdateMissingPackets(m_HighestCurrent);
      SendRecoveryRequest();
      m_LastRecoveryAt = current_time;
      m_RecoveryRetries++;
    }

    void FixFrameOnComplete();

    void RequestKeyframe();

    void UpdateMissingPackets(uint16_t Limit = (uint16_t)-1) {
      m_MissingPackets.clear();
      if (m_Possible == (uint16_t)-1) {
        return;
      }

      Limit = std::min(Limit, m_Possible);
      for (uint16_t i = 0; i < Limit; i++) {
        if (!GetDataCheck(i)) {
          m_MissingPackets.push_back(i);
        }
      }
    }

    void SendRecoveryRequest() {
      if (m_MissingPackets.empty()) {
        return;
      }

      size_t request_size = sizeof(recovery_request_t) + m_MissingPackets.size() * sizeof(uint16_t);
      std::vector<uint8_t> request_data(request_size);

      recovery_request_t* req = reinterpret_cast<recovery_request_t*>(request_data.data());
      req->sequence = m_Sequence;
      req->missing_count = static_cast<uint16_t>(m_MissingPackets.size());

      uint16_t* missing_indices = reinterpret_cast<uint16_t*>(req + 1);
      for (size_t i = 0; i < m_MissingPackets.size(); ++i) {
        missing_indices[i] = m_MissingPackets[i];
      }

      if (recovery_callback) {
        recovery_callback(std::move(request_data));
      }

    }

    void SetNewSequence(uint16_t sequence) {
      m_Sequence = sequence;
      m_Possible = (uint16_t)-1;
      m_RecoveryRetries = 0;
      m_MissingPackets.clear();
      __builtin_memset(m_DataCheck.data(), 0, m_DataCheck.size());
      // timed from the frame's first packet, see the udp receive path
      frame_start_time = 0;
      m_ReceivedCount = 0;
      m_HighestCurrent = 0;
      m_GapSince = 0;
    }


    void SetDataCheck(uint16_t Index) {
      uint8_t* byte = &this->m_DataCheck[Index / 8];
      *byte |= 1 << Index % 8;
    }

    bool GetDataCheck(uint16_t Index) {
      uint8_t* byte = &this->m_DataCheck[Index / 8];
      return (*byte & (1 << Index % 8)) >> Index % 8;
    }

    void WriteFramePacket();
  }view;

  struct share_t {
    uint64_t frame_index = 0;

    // the last frames as they were sent, one flat buffer per frame so storing a packet allocates nothing
    struct sent_frame_t {
      uint16_t sequence = (uint16_t)-1;
      std::vector<uint8_t> data;
      std::vector<std::pair<uint32_t, uint16_t>> packets; // offset, size
    };
    std::array<sent_frame_t, 32> m_SentFrames;
    std::function<void(const std::vector<uint8_t>&)> resend_packet_callback;

    void StorePacketForRecovery(uint16_t sequence, uint16_t current,
      const void* data, size_t size) {
      auto& frame = m_SentFrames[sequence % m_SentFrames.size()];
      if (frame.sequence != sequence) {
        frame.sequence = sequence;
        frame.data.clear();
        frame.packets.clear();
      }
      if (frame.packets.size() <= current) {
        frame.packets.resize(current + 1);
      }
      frame.packets[current] = {(uint32_t)frame.data.size(), (uint16_t)size};
      frame.data.insert(frame.data.end(), (const uint8_t*)data, (const uint8_t*)data + size);
    }

    void ProcessRecoveryRequest(uint16_t sequence, const uint16_t* missing_indices,
      uint16_t missing_count) {
      auto& frame = m_SentFrames[sequence % m_SentFrames.size()];
      if (frame.sequence != sequence || !resend_packet_callback) {
        return;
      }
      for (uint16_t i = 0; i < missing_count; ++i) {
        uint16_t packet_index = missing_indices[i];
        if (packet_index < frame.packets.size() && frame.packets[packet_index].second) {
          auto begin = frame.data.begin() + frame.packets[packet_index].first;
          resend_packet_callback(std::vector<uint8_t>(begin, begin + frame.packets[packet_index].second));
        }
      }
    }



    struct m_NetworkFlow_t {
      struct FrameListNodeData_t {
#if set_VerboseProtocol_HoldStreamTimes == 1
        ScreenShare_StreamHeader_Head_t::_VerboseTime_t _VerboseTime;
#endif
        std::vector<uint8_t> vec;
        uintptr_t SentOffset;
        bool keyframe = false;
      };
#include <fan/memory/fan_bll_preset.h>
#define BLL_set_prefix FrameList
#define BLL_set_Language 1
#define BLL_set_Usage 1
#define BLL_set_AreWeInsideStruct 1
#define BLL_set_NodeDataType FrameListNodeData_t
#define BLL_set_CPP_CopyAtPointerChange 1
#include <BLL/BLL.h>
      FrameList_t FrameList;
      int64_t Bucket; /* in bit, negative = debt */
      uint64_t BucketSize; /* in bit */
      uint64_t TimerLastCallAt;
      uint64_t TimerCallCount;
    }m_NetworkFlow;
    std::timed_mutex frame_list_mutex;
    void CalculateNetworkFlowBucket();
  }share;

  // what the share thread reports, to tell a stalled capture or encoder from packets that never arrive
  std::atomic<uint64_t> stream_udp_packets{0};
  std::atomic<uint64_t> stream_udp_bytes{0};
  std::atomic<uint64_t> share_frames_encoded{0};
  // the same on the viewing side: packets that arrived, frames put back together, frames decoded
  std::atomic<uint64_t> view_udp_packets{0};
  std::atomic<uint64_t> view_udp_bytes{0};
  std::atomic<uint64_t> view_frames_assembled{0};
  std::atomic<uint64_t> view_frames_decoded{0};
  std::atomic<uint64_t> view_decode_failed{0};

  fan::event::runv_t<bool> write_stream(
    uint16_t Current,
    uint16_t Possible,
    uint8_t Flag,
    void* Data,
    uintptr_t DataSize
  ) {
    uint8_t buffer[sizeof(ScreenShare_StreamHeader_Head_t) + 0x400];

    auto Body = (ScreenShare_StreamHeader_Body_t*)buffer;
    Body->SetSequence(share.frame_index);
    Body->SetCurrent(Current);

    void* DataWillBeAt;
    if (Current == 0) {
      auto Head = (ScreenShare_StreamHeader_Head_t*)buffer;
      Head->SetPossible(Possible);
      Head->SetFlag(Flag);
      DataWillBeAt = (void*)&Head[1];
    }
    else {
      DataWillBeAt = (void*)&Body[1];
    }

    uintptr_t BufferSize = (uintptr_t)DataWillBeAt - (uintptr_t)buffer + DataSize;
    __builtin_memcpy(DataWillBeAt, Data, DataSize);

    auto* channel = streaming_channel();
    if (!channel) {
      co_return 1;
    }
    share.StorePacketForRecovery(share.frame_index & 0xfff, Current, buffer, BufferSize);

    ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t rest;
    rest.ChannelID = channel->channel_id;
    rest.ChannelSessionID = channel->session_id;

  #if ecps_debug_prints >= 1
    {
      // once per channel, to compare with what the server says about who hosts it
      static uint32_t reported_channel = (uint32_t)-1;
      if (reported_channel != (uint32_t)rest.ChannelID.i) {
        reported_channel = (uint32_t)rest.ChannelID.i;
        fan::print("share: sending video as session", (uint64_t)session_id.i, "in channel", (uint64_t)rest.ChannelID.i,
          "channel session", (uint64_t)rest.ChannelSessionID.i);
      }
    }
  #endif

    co_await udp_write(0, ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData, rest, buffer, BufferSize);
    stream_udp_packets.fetch_add(1, std::memory_order_relaxed);
    stream_udp_bytes.fetch_add(BufferSize, std::memory_order_relaxed);
#if ecps_debug_prints >= 4
    static std::atomic<uint64_t> total_udp_packets_sent{ 0 };
    printf("UDP_SEND: seq=%u, pkt=%u/%u, size=%zu, total_sent=%llu\n",
      share.frame_index, Current, Possible, BufferSize,
      total_udp_packets_sent.fetch_add(1));
#endif
    co_return 0;
  }

  fan::event::task_t write_audio(const std::vector<uint8_t>& packet) {
    if (auto* channel = streaming_channel()) {
      ProtocolUDP::C2S_t::Channel_ScreenShare_Host_AudioData_t rest;
      rest.ChannelID = channel->channel_id;
      rest.ChannelSessionID = channel->session_id;
      co_await udp_write(0, ProtocolUDP::C2S_t::Channel_ScreenShare_Host_AudioData, rest, packet.data(), packet.size());
    }
  }

  fan::event::runv_t<bool> connect(const std::string& ip, uint16_t port) {
    this->ip = ip;
    this->port = port;

    connected = false;
    tcp_secure_ready = false;
    restore_own_channel = false;
    restore_streaming = false;
    should_stop_tcp_read.store(true);
    task_tcp_read = {};
    task_udp_listen = {};

    pending_requests.clear();
    channel_info.clear();
    available_channels.clear();
    channel_sessions.clear();
    channel_list_received = false;
    session_id = {};
    udp_crypto = {};

    if (!task_supervisor.valid()) {
      task_supervisor = supervise();
    }
    tcp_client.reset();
    try {
      co_await tcp_client.connect(ip, port);
    }
    catch (...) { co_return false; }

    should_stop_tcp_read.store(false);

    // udp read
    task_udp_listen = udp_client.listen(
      fan::network::listen_address_t{ this->ip, this->port },
      [this, ip, port](const fan::network::udp_t& udp, const fan::network::udp_datagram_t& datagram) -> fan::event::task_t {

        uint8_t packet[2048];
        auto size = datagram.data.size();
        if (size < sizeof(udp::BasePacket_t) || size > sizeof(packet)) {
          co_return;
        }
        __builtin_memcpy(packet, datagram.data.data(), size);

        udp::BasePacket_t bp = *(udp::BasePacket_t*)packet;
        if (bp.SessionID != session_id) {
          co_return;
        }
        size = udp_crypto.open(packet, sizeof(bp), size, bp.Nonce);
        if (!size) {
          co_return;
        }

        if (bp.Command == ProtocolUDP::S2C_t::KeepAlive) {
          if (sizeof(bp) != size) {
            fan::print("size is not same as sizeof expected arrival");
          }
        #if ecps_debug_prints >= 2
          fan::print("udp keep alive came");
        #endif
          if (keepalive_sent_time > 0) {
            uint64_t now = fan::event::now();
            ping_ms = (now - keepalive_sent_time) / 1000000.0;
            keepalive_sent_time = 0;
          }
        }
        else if (bp.Command == ProtocolUDP::S2C_t::Channel_ScreenShare_View_StreamData) {
          uintptr_t relative_size = size - sizeof(bp);
          auto command_data = (ProtocolUDP::S2C_t::Channel_ScreenShare_View_StreamData_t*)&(((udp::BasePacket_t*)packet)[1]);
          if (relative_size < sizeof(*command_data)) {
            co_return;
          }
          relative_size -= sizeof(*command_data);
          set_channel_viewing(command_data->ChannelID, true);
          view_udp_packets.fetch_add(1, std::memory_order_relaxed);
          view_udp_bytes.fetch_add(relative_size, std::memory_order_relaxed);

          if (relative_size < sizeof(ScreenShare_StreamHeader_Body_t)) {
            co_return;
          }

          auto stream_data = (uint8_t*)&command_data[1];
          auto body = (ScreenShare_StreamHeader_Body_t*)stream_data;
          uint16_t sequence = body->GetSequence();

        #if ecps_debug_prints >= 3
          printf("UDP: received seq=%u, current_seq=%u, diff=%d\n",
            sequence, view.m_Sequence, (int16_t)(sequence - view.m_Sequence));
        #endif

          // another streamer is being watched now, its sequence has nothing to do with the old one's
          if (view_resync.exchange(false) && sequence != view.m_Sequence) {
            if (view.m_Possible != (uint16_t)-1) {
              view.FixFrameOnComplete();
            }
            view.SetNewSequence(sequence);
          }

          bool is_sequence_valid = true;
          if (sequence != view.m_Sequence) {
            int16_t diff = (int16_t)(sequence - view.m_Sequence);

            if (abs(diff) > 0x800) {
            #if ecps_debug_prints >= 1
              printf("UDP: large sequence jump detected %u->%u, accepting\n", view.m_Sequence, sequence);
            #endif
              if (view.m_Possible != (uint16_t)-1) {
                view.FixFrameOnComplete();
              }
              view.SetNewSequence(sequence);
            }
            else {
              const int16_t sequence_window = 0x1000;
              bool is_past = diff < -sequence_window || (diff < 0 && diff > -sequence_window);

              if (is_past) {
              #if ecps_debug_prints >= 2
                printf("UDP: dropping past sequence %u (current %u)\n", sequence, view.m_Sequence);
              #endif
                co_return;
              }

              if (view.m_Possible != (uint16_t)-1) {
                view.FixFrameOnComplete();
              }
              view.SetNewSequence(sequence);
            }
          }

          uint8_t* packet_data;
          uint16_t current = body->GetCurrent();

          if (current == 0) {
            if (relative_size < sizeof(ScreenShare_StreamHeader_Head_t)) {
              co_return;
            }
            if (view.m_Possible != (uint16_t)-1) {
              co_return;
            }
            auto head = (ScreenShare_StreamHeader_Head_t*)stream_data;
            view.m_Possible = head->GetPossible();
            packet_data = &stream_data[sizeof(ScreenShare_StreamHeader_Head_t)];
          }
          else {
            packet_data = &stream_data[sizeof(ScreenShare_StreamHeader_Body_t)];
          }

          uint16_t packet_size = relative_size - ((uintptr_t)packet_data - (uintptr_t)stream_data);
          if (packet_size != 0x400) {
            view.m_ModuloSize = packet_size;
          }

          if (current < view.m_data.size() / 0x400) {
            __builtin_memcpy(&view.m_data[current * 0x400], packet_data, packet_size);
            if (!view.GetDataCheck(current)) {
              view.SetDataCheck(current);
              ++view.m_ReceivedCount;
              ++view.m_stats.Packet_Total;
            }
            view.m_HighestCurrent = std::max(view.m_HighestCurrent, current);

            view.last_valid_packet_time = fan::event::now();
            if (view.frame_start_time == 0) {
              view.frame_start_time = view.last_valid_packet_time;
            }

          #if ecps_debug_prints >= 3
            printf("UDP: accepted packet seq=%u, current=%u, size=%u\n", sequence, current, packet_size);
          #endif

            view.CheckAndSendRecoveryRequest();

            if ((view.m_Possible != (uint16_t)-1 && current == view.m_Possible) || packet_size != 0x400) {
              view.FixFrameOnComplete();
              view.SetNewSequence((view.m_Sequence + 1) % 0x1000);
            }
          }
          else {
          #if ecps_debug_prints >= 1
            printf("UDP: packet index %u exceeds buffer capacity\n", current);
          #endif
          }
        }
        else if (bp.Command == ProtocolUDP::S2C_t::Channel_ScreenShare_View_AudioData) {
          uintptr_t offset = sizeof(bp) + sizeof(ProtocolUDP::S2C_t::Channel_ScreenShare_View_AudioData_t);
          if (size < offset + sizeof(ScreenShare_AudioHeader_t)) {
            co_return;
          }
          auto command_data = (ProtocolUDP::S2C_t::Channel_ScreenShare_View_AudioData_t*)&packet[sizeof(bp)];
          if (audio_cb && is_channel_joined(command_data->ChannelID)) {
            audio_cb(&packet[offset], size - offset);
          }
        }
        else if (bp.Command == ProtocolUDP::S2C_t::Channel_Voice_Data) {
          uintptr_t offset = sizeof(bp) + sizeof(ProtocolUDP::S2C_t::Channel_Voice_Data_t);
          if (size <= offset + sizeof(ScreenShare_AudioHeader_t)) {
            co_return;
          }
          auto command_data = (ProtocolUDP::S2C_t::Channel_Voice_Data_t*)&packet[sizeof(bp)];
          if (voice_cb) {
            voice_cb(command_data->ChannelID, (std::uint32_t)command_data->SessionID.i, &packet[offset], size - offset);
          }
        }
        else if (bp.Command == ProtocolUDP::S2C_t::Channel_ScreenShare_RecoveryRequest) {
          uintptr_t offset = sizeof(bp) + sizeof(ProtocolUDP::S2C_t::Channel_ScreenShare_RecoveryRequest_t);
          if (size < offset + sizeof(recovery_request_t)) {
            co_return;
          }
          auto req = (recovery_request_t*)&packet[offset];
          if (size < offset + sizeof(recovery_request_t) + req->missing_count * sizeof(uint16_t)) {
            co_return;
          }
          share.ProcessRecoveryRequest(req->sequence, (const uint16_t*)(req + 1), req->missing_count);
        }
        else {
          fan::print("unprocessed udp data came");
        }
        co_return;
      }
    );

    connected = true;
    last_rx = fan::event::now();
    task_tcp_read = tcp_read();
    try {
      if (!fan::network::crypto::x25519_keygen(handshake_private, handshake_public)) {
        fan::throw_error("x25519 keygen failed");
      }
      Protocol_C2S_t::KeyExchange_t rest;
      __builtin_memcpy(rest.ClientPublic, handshake_public.data(), handshake_public.size());
      co_await tcp_write(Protocol_C2S_t::KeyExchange, &rest, sizeof(rest));
    }
    catch (...) {
      mark_disconnected("key exchange send failed");
      co_return false;
    }
    co_return true;
  }
  void mark_disconnected(const char* reason) {
    if (!connected) {
      return;
    }
    connected = false;
    tcp_secure_ready = false;
    should_stop_tcp_read.store(true);
    for (const auto& channel : channel_info) {
      if (channel.is_own) {
        restore_own_channel = true;
        restore_streaming = channel.is_streaming;
      }
    }
    session_id = {};
    udp_crypto = {};
    server_new = false;
    {
      std::lock_guard<std::mutex> lock(voice_mutex);
      voice_members.clear();
    }
    {
      std::lock_guard<std::mutex> lock(username_mutex);
      my_username.clear();
    }
    fail_active_file_transfers("connection to the server was lost");
    fan::print("disconnected from server:", reason);
  }
  fan::event::task_t supervise() {
    std::uint32_t backoff_ms = 500;
    while (true) {
      co_await fan::co_sleep(connected ? 1000 : backoff_ms);
      if (connected) {
        if (fan::event::now() - last_rx > tcp_timeout_ns) {
          mark_disconnected("timed out");
          continue;
        }
        task_keep_alive = send_keep_alive();
        continue;
      }
      bool own = restore_own_channel, streaming = restore_streaming;
      if (!co_await connect(ip, port)) {
        restore_own_channel = own;
        restore_streaming = streaming;
        backoff_ms = std::min(backoff_ms * 2, 8000u);
        continue;
      }
      backoff_ms = 500;
      fan::print("reconnected to", ip + ":" + std::to_string(port));
      task_restore = restore(own, streaming);
    }
  }
  fan::event::task_t send_keep_alive() {
    try {
      if (!tcp_secure_ready) {
        co_return;
      }
      co_await tcp_write(Protocol_C2S_t::KeepAlive, 0, 0);
      keepalive_sent_time = fan::event::now();
      co_await udp_write(0, ProtocolUDP::C2S_t::KeepAlive, {}, 0, 0);
    }
    catch (...) {
      mark_disconnected("keep alive write failed");
    }
  }
  fan::event::task_t restore(bool own, bool streaming) {
    try {
      co_await login();
      if (own) {
        auto channel_id = co_await channel_create();
        co_await channel_join(channel_id, true);
        if (streaming) {
          share.m_NetworkFlow.Bucket = 0;
          share.m_NetworkFlow.TimerLastCallAt = fan::event::now();
          share.m_NetworkFlow.TimerCallCount = 0;
          share.CalculateNetworkFlowBucket();
          set_channel_streaming(channel_id, true);
        }
        if (reconnected_cb) {
          reconnected_cb(channel_id);
        }
      }
      co_await request_channel_list();
    }
    catch (...) {}
  }
  // An older server drops a connection that sends it a message it doesn't know. This turns true when the server
  // sends the name message right after login, which only a newer one does, and every newer message waits for it
  std::atomic<bool> server_new {false};

  // the name this client wants (from the config) and what the server last answered
  std::string desired_username;
  mutable std::mutex username_mutex;
  std::string my_username;
  std::string username_result_name;
  std::atomic<std::uint8_t> username_result {0};
  std::atomic<std::uint32_t> username_result_seq {0};

  // Who shares in each channel, as the server last said (the network thread writes, the UI reads), and a flag that
  // tells the receive path the watched streamer changed: the next video packet starts a new sequence, whatever
  // its number is
  mutable std::mutex streamers_mutex;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> channel_streamers;
  std::atomic<bool> view_resync {false};

  std::vector<std::uint32_t> streamers_of(Protocol_ChannelID_t channel_id) const {
    std::lock_guard<std::mutex> lock(streamers_mutex);
    auto it = channel_streamers.find((std::uint32_t)channel_id.i);
    return it == channel_streamers.end() ? std::vector<std::uint32_t>() : it->second;
  }

  fan::event::task_t send_stream_state(Protocol_ChannelID_t channel_id, Protocol_ChannelSessionID_t channel_session_id, bool streaming) {
    if (!server_new.load()) {
      co_return;
    }
    try {
      Protocol_C2S_t::Channel_ScreenShare_Share_StreamState_t rest;
      std::memset(&rest, 0, sizeof(rest));
      rest.ChannelID = channel_id;
      rest.ChannelSessionID = channel_session_id;
      rest.Streaming = streaming ? 1 : 0;
      co_await tcp_write(Protocol_C2S_t::Channel_ScreenShare_Share_StreamState, &rest, sizeof(rest));
    }
    catch (...) {}
  }

  fan::event::task_t send_watch(Protocol_ChannelID_t channel_id, Protocol_ChannelSessionID_t channel_session_id, Protocol_SessionID_t source) {
    if (!server_new.load()) {
      co_return;
    }
    try {
      Protocol_C2S_t::Channel_ScreenShare_View_Watch_t rest;
      std::memset(&rest, 0, sizeof(rest));
      rest.ChannelID = channel_id;
      rest.ChannelSessionID = channel_session_id;
      rest.HasSource = 1;
      rest.Source = source;
      co_await tcp_write(Protocol_C2S_t::Channel_ScreenShare_View_Watch, &rest, sizeof(rest));
      // a new picture starts with a keyframe, ask the new streamer for one
      co_await send_idr_request(channel_id);
    }
    catch (...) {}
  }

  fan::event::task_t send_set_username(std::string name) {
    if (!server_new.load()) {
      co_return;
    }
    try {
      Protocol_C2S_t::SetUsername_t rest;
      std::memset(&rest, 0, sizeof(rest));
      std::strncpy(rest.Name, name.c_str(), sizeof(rest.Name) - 1);
      co_await tcp_write(Protocol_C2S_t::SetUsername, &rest, sizeof(rest));
    }
    catch (...) {}
  }

  fan::event::task_t login() {
    // while (1) {
    try {
      Protocol_C2S_t::Request_Login_t rest;
      rest.Type = Protocol::LoginType_t::Anonymous;
      co_await tcp_write(Protocol_C2S_t::Request_Login, &rest, sizeof(rest));
      // the saved name is asked for when the server shows it is a new one, see the SetUsername_Result handler
      co_return;
    }
    catch (fan::exception_t e) {
      login_fail_cb(e);
    }
    //co_await fan::co_sleep(1000);
    //co_await connect(ip, port);
 // }
  }
  bool verify_server_key(const fan::network::crypto::x25519_key_t& key) {
    using namespace fan::network::crypto;
    std::string host = ip + ":" + std::to_string(port);
    switch (check_pinned_key(known_hosts_path, host, key)) {
      case pin_result_e::matched: return true;
      case pin_result_e::pinned: fan::print("pinned server key for", host, to_hex(key.data(), key.size())); return true;
      case pin_result_e::mismatch: fan::print("server key for", host, "changed, refusing. delete its line in", known_hosts_path, "if this is expected"); return false;
      default: fan::print("failed to write", known_hosts_path); return false;
    }
  }
  fan::event::runv_t<Protocol_ChannelID_t> channel_create() {
    Protocol_C2S_t::CreateChannel_t rest;
    rest.Type = Protocol::ChannelType_ScreenShare_e;
    uint32_t request_id = co_await tcp_write(Protocol_C2S_t::CreateChannel, &rest, sizeof(rest));
    pending_requests[request_id] = pending_request_t{
      .request_id = request_id,
      .continuation = {},
      .channel_id = 0,
      .completed = false
    };
    Protocol_ChannelID_t channel_id = co_await channel_create_awaiter(*this, request_id);
    if (!is_channel_id_valid(channel_id)) {
      throw std::runtime_error("the server refused to create the channel");
    }
    // a new channel of ours (after a server restart, say) gets the password ours had again
    if (std::string password = own_saved_password(); !password.empty()) {
      Protocol_C2S_t::SetChannelPassword_t password_rest;
      std::memset(&password_rest, 0, sizeof(password_rest));
      password_rest.ChannelID = channel_id;
      std::memcpy(password_rest.Password, password.data(), std::min(password.size(), sizeof(password_rest.Password) - 1));
      co_await tcp_write(Protocol_C2S_t::SetChannelPassword, &password_rest, sizeof(password_rest));
    }
    co_return channel_id;
  }
  // the password saved for our own channel, which is named after us
  std::string own_saved_password() {
    std::string name = get_current_username();
    if (name.empty()) {
      return {};
    }
    std::lock_guard<std::mutex> lock(password_mutex);
    auto it = channel_passwords.find(password_key(name));
    return it == channel_passwords.end() ? std::string() : it->second;
  }
  // answers the coroutine waiting in channel_create
  void finish_request(uint32_t request_id, Protocol_ChannelID_t channel_id) {
    auto it = pending_requests.find(request_id);
    if (it == pending_requests.end()) {
      return;
    }
    it->second.channel_id = channel_id;
    it->second.completed = true;
    if (it->second.continuation) {
      it->second.continuation.resume();
    }
  }
  // the channel is listed as joined at once, a JoinChannel_Error from the server takes it out again
  // A password given here is tried, otherwise the one saved for this channel. A channel without one is joined as before
  fan::event::task_t channel_join(Protocol_ChannelID_t channel_id, bool is_own_channel = false, std::string password = {}) {
    channel_info_t ci;
    if (password.empty() && !is_own_channel) {
      password = saved_password(channel_id);
    }
    if (!password.empty() && !is_own_channel) {
      Protocol_C2S_t::JoinChannelPassword_t rest;
      std::memset(&rest, 0, sizeof(rest));
      rest.ChannelID = channel_id;
      std::memcpy(rest.Password, password.data(), std::min(password.size(), sizeof(rest.Password) - 1));
      ci.join_request_id = co_await tcp_write(Protocol_C2S_t::JoinChannelPassword, &rest, sizeof(rest));
      ci.password = password;
    }
    else {
      ci.join_request_id = co_await tcp_write(Protocol_C2S_t::JoinChannel, &channel_id, sizeof(channel_id));
    }
    ci.channel_id = channel_id;
    ci.joined_at.start();
    ci.is_own = is_own_channel;
    channel_info.emplace_back(ci);
    co_await send_idr_request(channel_id);
    did_just_join = !is_own_channel;
  }

  // ---- channel passwords --------------------------------------------------------------------------------------
  // A password is kept per server and channel name (channels are named after their host), so it is asked for once
  struct password_prompt_t {
    bool wanted = false;
    bool wrong = false;
    Protocol_ChannelID_t channel_id = 0;
    std::string name;
  };
  std::mutex password_mutex;
  std::unordered_map<std::string, std::string> channel_passwords;
  password_prompt_t password_prompt;
  std::function<void()> passwords_changed;

  std::string password_key(const std::string& channel_name) const {
    return ip + ":" + std::to_string(port) + "/" + channel_name;
  }
  std::string channel_name_of(Protocol_ChannelID_t channel_id) const {
    for (const auto& channel : available_channels) {
      if (channel.channel_id.i == channel_id.i) {
        return channel.name;
      }
    }
    return {};
  }
  std::string saved_password(Protocol_ChannelID_t channel_id) {
    std::string name = channel_name_of(channel_id);
    if (name.empty()) {
      return {};
    }
    std::lock_guard<std::mutex> lock(password_mutex);
    auto it = channel_passwords.find(password_key(name));
    return it == channel_passwords.end() ? std::string() : it->second;
  }
  void remember_password(const std::string& channel_name, const std::string& password) {
    if (channel_name.empty() || password.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(password_mutex);
      auto& saved = channel_passwords[password_key(channel_name)];
      if (saved == password) {
        return;
      }
      saved = password;
    }
    if (passwords_changed) {
      passwords_changed();
    }
  }
  void forget_password(const std::string& channel_name) {
    {
      std::lock_guard<std::mutex> lock(password_mutex);
      if (channel_passwords.erase(password_key(channel_name)) == 0) {
        return;
      }
    }
    if (passwords_changed) {
      passwords_changed();
    }
  }
  // only a v0.99 server knows the command, the voice probe is how that shows
  bool channel_passwords_supported(Protocol_ChannelID_t channel_id) const {
    return voice_known(channel_id);
  }
  // an empty password removes it
  fan::event::task_t channel_set_password(Protocol_ChannelID_t channel_id, std::string password) {
    if (!channel_passwords_supported(channel_id)) {
      co_return;
    }
    Protocol_C2S_t::SetChannelPassword_t rest;
    std::memset(&rest, 0, sizeof(rest));
    rest.ChannelID = channel_id;
    std::memcpy(rest.Password, password.data(), std::min(password.size(), sizeof(rest.Password) - 1));
    co_await tcp_write(Protocol_C2S_t::SetChannelPassword, &rest, sizeof(rest));
    // kept like a channel joined with it: coming back to our own channel, or making it again, needs it
    std::string name = channel_name_of(channel_id);
    if (name.empty()) {
      name = get_current_username();
    }
    if (password.empty()) {
      forget_password(name);
    }
    else {
      remember_password(name, password);
    }
    co_await request_channel_list();
  }

  // leaves a channel: the server is told, and everything kept about the channel here goes
  fan::event::task_t channel_quit(Protocol_ChannelID_t channel_id) {
    auto* channel = find_channel(channel_id);
    if (!channel) {
      co_return;
    }
    Protocol_C2S_t::QuitChannel_t rest;
    rest.ChannelID = channel_id;
    rest.ChannelSessionID = channel->session_id;
    forget_channel(channel_id);
    co_await tcp_write(Protocol_C2S_t::QuitChannel, &rest, sizeof(rest));
  }

  void forget_channel(Protocol_ChannelID_t channel_id) {
    channel_info.erase(std::remove_if(channel_info.begin(), channel_info.end(),
      [&](const auto& channel) { return channel.channel_id.i == channel_id.i; }), channel_info.end());
    channel_sessions.erase(channel_id.i);
    {
      std::lock_guard<std::mutex> lock(chat_mutex);
      chat_messages.erase((std::uint32_t)channel_id.i);
      chat_channels.erase((std::uint32_t)channel_id.i);
    }
    {
      std::lock_guard<std::mutex> lock(voice_mutex);
      voice_members.erase((std::uint32_t)channel_id.i);
    }
    std::lock_guard<std::mutex> lock(streamers_mutex);
    channel_streamers.erase((std::uint32_t)channel_id.i);
  }

  mutable std::mutex voice_mutex;
  std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> voice_members;
  std::chrono::steady_clock::time_point voice_probed_at{};
  std::function<void(Protocol_ChannelID_t channel_id, std::uint32_t session, const uint8_t* data, uintptr_t size)> voice_cb;

  bool voice_known(Protocol_ChannelID_t channel_id) const {
    std::lock_guard<std::mutex> lock(voice_mutex);
    return voice_members.contains((std::uint32_t)channel_id.i);
  }
  std::vector<std::uint32_t> voice_members_of(Protocol_ChannelID_t channel_id) const {
    std::lock_guard<std::mutex> lock(voice_mutex);
    auto it = voice_members.find((std::uint32_t)channel_id.i);
    return it == voice_members.end() ? std::vector<std::uint32_t>() : it->second;
  }
  fan::event::task_t write_voice(Protocol_ChannelID_t channel_id, const void* data, uintptr_t size) {
    if (auto* channel = find_channel(channel_id)) {
      ProtocolUDP::C2S_t::Channel_Voice_Data_t rest;
      rest.ChannelID = channel->channel_id;
      rest.ChannelSessionID = channel->session_id;
      co_await udp_write(0, ProtocolUDP::C2S_t::Channel_Voice_Data, rest, data, size);
    }
  }
  fan::event::task_t send_voice_join(Protocol_ChannelID_t channel_id, bool joined) {
    if (auto* channel = find_channel(channel_id)) {
      Protocol_C2S_t::Channel_Voice_Join_t rest;
      rest.ChannelID = channel->channel_id;
      rest.ChannelSessionID = channel->session_id;
      rest.Joined = joined;
      co_await tcp_write(Protocol_C2S_t::Channel_Voice_Join, &rest, sizeof(rest));
    }
  }
  fan::event::task_t probe_voice() {
    auto now = std::chrono::steady_clock::now();
    if (now - voice_probed_at < std::chrono::seconds(2)) {
      co_return;
    }
    voice_probed_at = now;
    std::vector<Protocol_ChannelID_t> unanswered;
    for (auto& channel : channel_info) {
      if (channel.join_confirmed && channel.voice_probes < 5 && !voice_known(channel.channel_id)) {
        ++channel.voice_probes;
        unanswered.push_back(channel.channel_id);
      }
    }
    for (auto channel_id : unanswered) {
      co_await write_voice(channel_id, nullptr, 0);
    }
    // the chat is asked for the same way: a server without it ignores the probe, one with it answers with the history
    std::vector<std::pair<Protocol_ChannelID_t, Protocol_ChannelSessionID_t>> chat_unanswered;
    for (auto& channel : channel_info) {
      if (channel.join_confirmed && channel.chat_probes < 5 && !chat_known(channel.channel_id)) {
        ++channel.chat_probes;
        chat_unanswered.emplace_back(channel.channel_id, channel.session_id);
      }
    }
    for (auto [channel_id, channel_session] : chat_unanswered) {
      ProtocolUDP::C2S_t::Channel_Chat_Probe_t rest;
      rest.ChannelID = channel_id;
      rest.ChannelSessionID = channel_session;
      co_await udp_write(0, ProtocolUDP::C2S_t::Channel_Chat_Probe, rest, nullptr, 0);
    }
  }

  // ---- chat -----------------------------------------------------------------------------------------------------
  // per channel, the messages the server sent (its history first). Written by the network thread, read by the gui
  struct chat_message_t {
    std::uint32_t id = 0;
    std::uint32_t from = 0;
    std::string name;
    std::int64_t time = 0;
    std::string text;
  };
  mutable std::mutex chat_mutex;
  std::unordered_map<std::uint32_t, std::vector<chat_message_t>> chat_messages;
  std::unordered_map<std::uint32_t, bool> chat_channels;
  // goes up with every message, the gui scrolls down when it changed
  std::atomic<std::uint64_t> chat_revision{0};

  bool chat_known(Protocol_ChannelID_t channel_id) const {
    std::lock_guard<std::mutex> lock(chat_mutex);
    return chat_channels.contains((std::uint32_t)channel_id.i);
  }
  std::vector<chat_message_t> chat_of(Protocol_ChannelID_t channel_id) const {
    std::lock_guard<std::mutex> lock(chat_mutex);
    auto it = chat_messages.find((std::uint32_t)channel_id.i);
    return it == chat_messages.end() ? std::vector<chat_message_t>() : it->second;
  }
  void on_chat_message(Protocol_ChannelID_t channel_id, chat_message_t message) {
    {
      std::lock_guard<std::mutex> lock(chat_mutex);
      chat_channels[(std::uint32_t)channel_id.i] = true;
      auto& list = chat_messages[(std::uint32_t)channel_id.i];
      // the history after a reconnect repeats what is here already
      for (const auto& have : list) {
        if (have.id == message.id) {
          return;
        }
      }
      list.push_back(std::move(message));
      std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
      if (list.size() > 500) {
        list.erase(list.begin(), list.begin() + (list.size() - 500));
      }
    }
    chat_revision.fetch_add(1);
  }
  void on_chat_known(Protocol_ChannelID_t channel_id) {
    std::lock_guard<std::mutex> lock(chat_mutex);
    chat_channels[(std::uint32_t)channel_id.i] = true;
  }
  // only into a channel whose server answered the probe, an older one would close the connection
  fan::event::task_t send_chat(Protocol_ChannelID_t channel_id, std::string text) {
    auto* channel = find_channel(channel_id);
    if (!channel || !chat_known(channel_id) || text.empty()) {
      co_return;
    }
    Protocol_C2S_t::Channel_Chat_Send_t rest;
    std::memset(&rest, 0, sizeof(rest));
    rest.ChannelID = channel->channel_id;
    rest.ChannelSessionID = channel->session_id;
    std::size_t length = std::min(text.size(), sizeof(rest.Text));
    rest.Length = (std::uint16_t)length;
    std::memcpy(rest.Text, text.data(), length);
    co_await tcp_write(Protocol_C2S_t::Channel_Chat_Send, &rest, sizeof(rest));
  }

  bool is_channel_id_valid(Protocol_ChannelID_t channel_id) const {
    return channel_id.i != (uint16_t)-1 && channel_id.i != 0;
  }
  bool is_channel_available(Protocol_ChannelID_t channel_id) const {
    return is_channel_id_valid(channel_id) && std::any_of(available_channels.begin(), available_channels.end(),
      [channel_id](const channel_list_info_t& channel) {
        return channel.channel_id.i == channel_id.i;
      });
  }
  bool is_channel_joined(Protocol_ChannelID_t channel_id) const {
    return is_channel_id_valid(channel_id) && find_channel(channel_id);
  }

  bool is_current_user_host_of_channel(Protocol_ChannelID_t channel) const {
    auto session_it = channel_sessions.find(channel);
    if (session_it == channel_sessions.end()) {
      return false;
    }

    // by session, not by name: two clients can both be called User_1
    for (const auto& session : session_it->second) {
      if (session.is_host && session.session_id.i == session_id.i) {
        return true;
      }
    }
    return false;
  }

  // channels this client hosts: it created them, or the session list names it the host
  bool is_hosting_channel(Protocol_ChannelID_t channel_id) const {
    for (const auto& channel : channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        return channel.is_own || is_current_user_host_of_channel(channel_id);
      }
    }
    return false;
  }

  bool is_hosting_any_channel() const {
    return std::any_of(channel_info.begin(), channel_info.end(), [this](const channel_info_t& channel) {
      return is_hosting_channel(channel.channel_id);
    });
  }

  fan::event::task_t task_tcp_read;

  std::string ip;
  uint16_t port;
  fan::network::tcp_t tcp_client;
  fan::network::udp_t udp_client;
  fan::event::task_t task_udp_listen;
  uint64_t keepalive_sent_time = 0;
  f32_t ping_ms = 0;

  std::function<void(fan::exception_t)> login_fail_cb;
  std::function<void(const uint8_t*, uintptr_t)> audio_cb;
  std::function<void(Protocol_ChannelID_t)> reconnected_cb;
  fan::event::task_t task_supervisor;
  fan::event::task_t task_restore;
  fan::event::task_t task_keep_alive;
  static constexpr std::uint64_t tcp_timeout_ns = 6'000'000'000;
  std::uint64_t last_rx = 0;
  bool connected = false;
  bool tcp_secure_ready = false;
  bool restore_own_channel = false;
  bool restore_streaming = false;

  fan::event::task_t request_channel_list() {
    uint8_t a;
    co_await tcp_write(Protocol_C2S_t::RequestChannelList, &a, 1);
  }

  fan::event::task_t request_channel_session_list(Protocol_ChannelID_t channel_id) {
    Protocol_C2S_t::RequestChannelSessionList_t request;
    request.ChannelID = channel_id;
    co_await tcp_write(Protocol_C2S_t::RequestChannelSessionList, &request, sizeof(request));
  }



  std::string get_current_username() const {
    {
      // what the server told us is newer than what the user lists say
      std::lock_guard<std::mutex> lock(username_mutex);
      if (!my_username.empty()) {
        return my_username;
      }
    }
    for (const auto& [channel_id, sessions] : channel_sessions) {
      for (const auto& session : sessions) {
        if (session.session_id.i == this->session_id.i) {
          return session.username;
        }
      }
    }

    if (session_id.i != 0) {
      return "Anonymous_" + std::to_string(session_id.i);
    }

    return "Not Connected";
  }

  void set_channel_streaming(Protocol_ChannelID_t channel_id, bool streaming) {
    if (auto* channel = find_channel(channel_id)) {
      channel->is_streaming = streaming;
      channel->stream_start_timer.start();
    }
  }
  void set_channel_viewing(Protocol_ChannelID_t channel_id, bool viewing) {
    if (auto* channel = find_channel(channel_id)) {
      channel->is_viewing = viewing;
    }
  }
  bool is_channel_streaming(Protocol_ChannelID_t channel_id) const {
    auto* channel = find_channel(channel_id);
    return channel && channel->is_streaming;
  }
  uint64_t get_channel_stream_time(Protocol_ChannelID_t channel_id) {
    auto* channel = find_channel(channel_id);
    return channel ? channel->stream_start_timer.elapsed() : 0;
  }
  bool is_channel_viewing(Protocol_ChannelID_t channel_id) const {
    auto* channel = find_channel(channel_id);
    return channel && channel->is_viewing;
  }
  bool is_streaming_to_any_channel() const {
    return streaming_channel() != nullptr;
  }
  bool is_viewing_any_channel() const {
    return std::any_of(channel_info.begin(), channel_info.end(),
      [](const channel_info_t& ch) { return ch.is_viewing; });
  }
  fan::event::task_t send_idr_request(Protocol_ChannelID_t channel_id) {
    Protocol_C2S_t::Channel_ScreenShare_ViewToShare_t rest;
    std::memset(&rest, 0, sizeof(rest));
    rest.ChannelID = channel_id;
    rest.Flag = ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR;
    co_await tcp_write(Protocol_C2S_t::Channel_ScreenShare_ViewToShare, &rest, sizeof(rest));
  }
  // asks the streamer of every watched channel for a keyframe
  fan::event::task_t request_idr_reset() {
    // the list can change while a write is suspended
    std::vector<Protocol_ChannelID_t> viewed;
    for (const auto& channel : channel_info) {
      if (channel.is_viewing) {
        viewed.push_back(channel.channel_id);
      }
    }
    try {
      for (auto channel_id : viewed) {
        co_await send_idr_request(channel_id);
      }
    }
    catch (...) {}
  }

  void update_host_mouse_coordinate(Protocol_ChannelID_t channel_id, const fan::vec2ui& pos);

  bool channel_list_received = false;

  struct pending_request_t {
    uint32_t request_id;
    std::coroutine_handle<> continuation;
    Protocol_ChannelID_t channel_id = 0;
    bool completed = false;
  };
  std::unordered_map<uint32_t, pending_request_t> pending_requests;

  fan::network::crypto::x25519_key_t handshake_private{};
  fan::network::crypto::x25519_key_t handshake_public{};
  fan::network::crypto::secure_channel_t udp_crypto;
  inline static const std::string known_hosts_path = "known_hosts";

  Protocol_SessionID_t session_id;

  struct channel_info_t {
    Protocol_ChannelID_t channel_id = 0;
    Protocol_ChannelSessionID_t session_id = 0;

    bool is_streaming = false;
    fan::time::timer stream_start_timer;
    bool is_viewing = false;
    bool is_own = false;
    uint32_t join_request_id = (uint32_t)-1;
    bool join_confirmed = false;
    uint8_t voice_probes = 0;
    uint8_t chat_probes = 0;
    std::string password; // sent with the join, kept when the server lets us in
    fan::time::timer joined_at;
    int flag = 0;
  };
  std::vector<channel_info_t> channel_info;

  channel_info_t* find_channel(Protocol_ChannelID_t channel_id) {
    for (auto& channel : channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        return &channel;
      }
    }
    return nullptr;
  }
  const channel_info_t* find_channel(Protocol_ChannelID_t channel_id) const {
    return const_cast<ecps_backend_t*>(this)->find_channel(channel_id);
  }
  // the channel this client streams to, there is at most one
  const channel_info_t* streaming_channel() const {
    for (const auto& channel : channel_info) {
      if (channel.is_streaming) {
        return &channel;
      }
    }
    return nullptr;
  }


  std::vector<channel_list_info_t> available_channels;
  std::unordered_map<Protocol_ChannelID_t::Type, std::vector<session_info_t>> channel_sessions;
  bool did_just_join = false; // need manual reset from gui

#include "file_transfer.h"

}ecps_backend;
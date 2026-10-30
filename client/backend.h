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
  #if use_input_control == 1
    MD_Mice_Close(&mice);
    MD_Keyboard_close(&keyboard);
  #endif
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
    uint32_t m_PacketCounter = 0;
    bool m_AckSent = false;
    uint8_t m_RecoveryRetries = 0;

    static constexpr uint32_t recovery_ack_interval = 1;
    std::function<void(std::vector<uint8_t>)> recovery_callback;

    struct {
      uint64_t Frame_Total;
      uint64_t Frame_Drop;
      uint64_t Packet_Total;
      uint64_t Packet_HeadDrop;
      uint64_t Packet_BodyDrop;
      uint64_t Frame_Corrected;
    } m_stats;

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
          SetNewSequence((m_Sequence + 1) % 0x1000);
          return;
        }
      }

      m_PacketCounter++;
      if (m_PacketCounter % 1 != 0) {
        return;
      }

      UpdateMissingPackets();
      if (m_MissingPackets.empty()) {
        m_AckSent = false;
        m_RecoveryRetries = 0;
        return;
      }

      if (!m_AckSent || m_RecoveryRetries < 3) {
        SendRecoveryRequest();
        m_RecoveryRetries++;
      }
    }

    void FixFrameOnComplete();

    void ApplyImprovedErrorConcealment() {
      for (uint16_t missing : m_MissingPackets) {
        if (missing > 0 && missing < m_Possible - 1) {
          uint8_t* dst = &m_data[missing * 0x400];
          const uint8_t* prev = &m_data[(missing - 1) * 0x400];
          const uint8_t* next = &m_data[(missing + 1) * 0x400];

          for (int i = 0; i < 0x400; i += 4) {
            dst[i] = static_cast<uint8_t>((prev[i] + next[i]) / 2);
            dst[i + 1] = static_cast<uint8_t>((prev[i + 1] + next[i + 1]) / 2);
            dst[i + 2] = static_cast<uint8_t>((prev[i + 2] + next[i + 2]) / 2);
            dst[i + 3] = static_cast<uint8_t>((prev[i + 3] + next[i + 3]) / 2);
          }
        }
        else if (missing > 0) {
          uint8_t* dst = &m_data[missing * 0x400];
          const uint8_t* src = &m_data[(missing - 1) * 0x400];
          for (int i = 0; i < 0x400; i += 4) {
            dst[i] = static_cast<uint8_t>(src[i] * 0.8f);
            dst[i + 1] = static_cast<uint8_t>(src[i + 1] * 0.8f);
            dst[i + 2] = static_cast<uint8_t>(src[i + 2] * 0.8f);
            dst[i + 3] = static_cast<uint8_t>(src[i + 3] * 0.8f);
          }
        }
        else {
          memset(&m_data[missing * 0x400], 0, 0x400);
        }
      }
    }

    void RequestKeyframe();

    void UpdateMissingPackets() {
      m_MissingPackets.clear();
      if (m_Possible == (uint16_t)-1) {
        return;
      }

      for (uint16_t i = 0; i < m_Possible; i++) {
        if (!GetDataCheck(i)) {
          m_MissingPackets.push_back(i);
        }
      }
    }

    void SendRecoveryRequest() {
      if (m_MissingPackets.empty()) {
        return;
      }

      struct recovery_request_t {
        uint16_t sequence;
        uint16_t missing_count;
      };

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

      m_AckSent = true;
    }

    void SetNewSequence(uint16_t sequence) {
      m_Sequence = sequence;
      m_Possible = (uint16_t)-1;
      m_AckSent = false;
      m_RecoveryRetries = 0;
      m_MissingPackets.clear();
      __builtin_memset(m_DataCheck.data(), 0, m_DataCheck.size());
      // timed from the frame's first packet, see the udp receive path
      frame_start_time = 0;
      m_PacketCounter = 0;
    }

    bool IsSequencePast(uint16_t PacketSequence) {
      const int16_t SEQUENCE_WINDOW = 0x1000; // half of sequence space
      int16_t diff = (int16_t)(PacketSequence - this->m_Sequence);

      // consider past if difference is negative and within reasonable range
      return diff < -SEQUENCE_WINDOW || (diff < 0 && diff > -SEQUENCE_WINDOW);
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

    std::unordered_map<uint16_t, std::vector<std::vector<uint8_t>>> m_SentPackets;
    std::function<void(const std::vector<uint8_t>&)> resend_packet_callback;

    void StorePacketForRecovery(uint16_t sequence, uint16_t current,
      const void* data, size_t size) {
      if (m_SentPackets[sequence].size() <= current) {
        m_SentPackets[sequence].resize(current + 1);
      }

      m_SentPackets[sequence][current].assign(
        static_cast<const uint8_t*>(data),
        static_cast<const uint8_t*>(data) + size
      );

      if (m_SentPackets.size() > 50) {
        std::erase_if(m_SentPackets, [sequence](const auto& p) { return ((sequence - p.first) & 0xfff) > 32; });
      }
    }

    void ProcessRecoveryRequest(uint16_t sequence, const uint16_t* missing_indices,
      uint16_t missing_count) {
      auto it = m_SentPackets.find(sequence);
      if (it == m_SentPackets.end()) {
        return;
      }

      auto& packets = it->second;

      for (uint16_t i = 0; i < missing_count; ++i) {
        uint16_t packet_index = missing_indices[i];
        if (packet_index < packets.size() && !packets[packet_index].empty()) {

          if (resend_packet_callback) {
            resend_packet_callback(packets[packet_index]);
          }
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
      uint64_t WantedInterval = 5000000;
      int64_t Bucket; /* in bit, negative = debt */
      uint64_t BucketSize; /* in bit */
      uint64_t TimerLastCallAt;
      uint64_t TimerCallCount;
    }m_NetworkFlow;
    std::timed_mutex frame_list_mutex;
    void CalculateNetworkFlowBucket();
  }share;

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
    share.StorePacketForRecovery(share.frame_index & 0xfff, Current, buffer, BufferSize);

    __builtin_memcpy(DataWillBeAt, Data, DataSize);

    ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t rest;
    Protocol_ChannelID_t streaming_channel_id;
    Protocol_ChannelSessionID_t streaming_session_id;
    bool found_streaming = false;

    for (const auto& channel : channel_info) {
      if (channel.is_streaming) {
        streaming_channel_id = channel.channel_id;
        streaming_session_id = channel.session_id;
        found_streaming = true;
        break;
      }
    }

    if (!found_streaming) {
      co_return 1;
    }

    rest.ChannelID = streaming_channel_id;
    rest.ChannelSessionID = streaming_session_id;

    co_await udp_write(0, ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData, rest, buffer, BufferSize);
#if ecps_debug_prints >= 4
    static std::atomic<uint64_t> total_udp_packets_sent{ 0 };
    printf("UDP_SEND: seq=%u, pkt=%u/%u, size=%zu, total_sent=%llu\n",
      share.frame_index, Current, Possible, BufferSize,
      total_udp_packets_sent.fetch_add(1));
#endif
    co_return 0;
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
            view.SetDataCheck(current);

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
  fan::event::task_t login() {
    // while (1) {
    try {
      Protocol_C2S_t::Request_Login_t rest;
      rest.Type = Protocol::LoginType_t::Anonymous;
      co_await tcp_write(Protocol_C2S_t::Request_Login, &rest, sizeof(rest));
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
    co_return co_await channel_create_awaiter(*this, request_id);
  }
  // add check if channel join is ok
  fan::event::task_t channel_join(Protocol_ChannelID_t channel_id, bool is_own_channel = false) {
    co_await tcp_write(Protocol_C2S_t::JoinChannel, &channel_id, sizeof(channel_id));
    channel_info_t ci;
    ci.channel_id = channel_id;
    ci.joined_at.start();
    ci.is_own = is_own_channel;
    channel_info.emplace_back(ci);
    {
      ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_ViewToShare_t rest;
      rest.ChannelID = channel_info.back().channel_id;
      rest.Flag = ecps_backend_t::ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR;
      co_await tcp_write(
        ecps_backend_t::Protocol_C2S_t::Channel_ScreenShare_ViewToShare,
        &rest,
        sizeof(rest)
      );
    }
    did_just_join = !is_own_channel;
  }

  bool is_channel_id_valid(Protocol_ChannelID_t channel_id) const {
    return channel_id.i != (uint16_t)-1 && channel_id.i != 0;
  }
  bool is_channel_available(Protocol_ChannelID_t channel_id) const {
    if (!is_channel_id_valid(channel_id)) {
      return false;
    }
    return std::any_of(available_channels.begin(), available_channels.end(),
      [channel_id](const channel_list_info_t& channel) {
        return channel.channel_id.i == channel_id.i;
      });
  }
  bool is_channel_joined(Protocol_ChannelID_t channel_id) const {
    if (!is_channel_id_valid(channel_id)) {
      return false;
    }

    return std::any_of(channel_info.begin(), channel_info.end(),
      [channel_id](const channel_info_t& joined_channel) {
        return joined_channel.channel_id.i == channel_id.i;
      });
  }

  bool cleanup_disconnected_channels() {
    size_t original_size = channel_info.size();
    channel_info.erase(
      std::remove_if(channel_info.begin(), channel_info.end(),
        [this](const auto& joined) {
          if (joined.joined_at.elapsed() < 5e+9) {
            return false;
          }

          return std::none_of(available_channels.begin(), available_channels.end(),
            [&](const auto& available) {
              return available.channel_id.i == joined.channel_id.i;
            });
        }),
      channel_info.end()
    );
    return channel_info.size() < original_size;
  }

  bool is_current_user_host_of_channel(Protocol_ChannelID_t channel) const {
    auto session_it = channel_sessions.find(channel);
    if (session_it == channel_sessions.end()) {
      return false;
    }

    const std::string current_username = get_current_username();
    for (const auto& session : session_it->second) {
      if (session.username == current_username && session.is_host) {
        return true;
      }
    }
    return false;
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


  fan::event::task_t handle_channel_list(ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) {
    auto data = reinterpret_cast<const uint8_t*>(&base) + sizeof(base);
    auto channel_list = reinterpret_cast<const Protocol_S2C_t::ChannelList_t*>(data);

    backend.available_channels.clear();
    backend.available_channels.reserve(channel_list->ChannelCount);

    auto channel_info_ptr = reinterpret_cast<const Protocol_S2C_t::ChannelInfo_t*>(data + sizeof(Protocol_S2C_t::ChannelList_t));

    for (uint16_t i = 0; i < channel_list->ChannelCount; ++i) {
      ecps_backend_t::channel_list_info_t info;
      info.channel_id = channel_info_ptr[i].ChannelID;
      info.type = channel_info_ptr[i].Type;
      info.user_count = channel_info_ptr[i].UserCount;
      info.name = std::string(channel_info_ptr[i].Name, strnlen(channel_info_ptr[i].Name, 63));
      info.is_password_protected = channel_info_ptr[i].IsPasswordProtected != 0;
      info.host_session_id = channel_info_ptr[i].HostSessionID;
      backend.available_channels.push_back(info);
    }

    backend.channel_list_received = true;
#if ecps_debug_prints >= 3
    fan::print("Received channel list with {} channels", channel_list->ChannelCount);
#endif

    co_return;
  }

  fan::event::task_t handle_channel_session_list(ecps_backend_t& backend, const tcp::ProtocolBasePacket_t& base) {
    auto data = reinterpret_cast<const uint8_t*>(&base) + sizeof(base);
    auto session_list = reinterpret_cast<const Protocol_S2C_t::ChannelSessionList_t*>(data);

    Protocol_ChannelID_t channel_id = session_list->ChannelID;

    backend.channel_sessions[channel_id].clear();
    backend.channel_sessions[channel_id].reserve(session_list->SessionCount);

    auto session_info_ptr = reinterpret_cast<const Protocol_S2C_t::SessionInfo_t*>(data + sizeof(Protocol_S2C_t::ChannelSessionList_t));

    for (uint16_t i = 0; i < session_list->SessionCount; ++i) {
      ecps_backend_t::session_info_t info;
      info.session_id = session_info_ptr[i].SessionID;
      info.channel_session_id = session_info_ptr[i].ChannelSessionID;
      info.account_id = session_info_ptr[i].AccountID;
      info.username = std::string((const char*)session_info_ptr[i].Username, strnlen((const char*)session_info_ptr[i].Username, 31));
      info.is_host = session_info_ptr[i].IsHost != 0;
      info.joined_at = session_info_ptr[i].JoinedAt;
      backend.channel_sessions[channel_id].push_back(info);
    }
#if ecps_debug_prints >= 3
    fan::print("Received session list for channel {} with {} sessions", (int)channel_id, session_list->SessionCount);
#endif

    co_return;
  }


  bool is_connected() {
    return false;
  }

  std::string get_current_username() const {
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
    if (channel_id.i == (uint16_t)-1) {
      return;
    }
    for (auto& channel : channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        channel.is_streaming = streaming;
        channel.stream_start_timer.start();
        return;
      }
    }
  }
  void set_channel_viewing(Protocol_ChannelID_t channel_id, bool viewing) {
    if (channel_id.i == (uint16_t)-1) {
      return;
    }
    for (auto& channel : channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        channel.is_viewing = viewing;
        return;
      }
    }
  }
  bool is_channel_streaming(Protocol_ChannelID_t channel_id) const {
    if (channel_id.i == (uint16_t)-1) {
      return false;
    }
    for (const auto& channel : channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        return channel.is_streaming;
      }
    }
    return false;
  }
  uint64_t get_channel_stream_time(Protocol_ChannelID_t channel_id) {
    if (channel_id.i == (uint16_t)-1) {
      return 0;
    }
    for (const auto& channel : channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        return channel.stream_start_timer.elapsed();
      }
    }
    return 0;
  }
  bool is_channel_viewing(Protocol_ChannelID_t channel_id) const {
    for (const auto& channel : channel_info) {
      if (channel.channel_id.i == channel_id.i) {
        return channel.is_viewing;
      }
    }
    return false;
  }
  bool is_streaming_to_any_channel() const {
    return std::any_of(channel_info.begin(), channel_info.end(),
      [](const channel_info_t& ch) { return ch.is_streaming; });
  }
  bool is_viewing_any_channel() const {
    return std::any_of(channel_info.begin(), channel_info.end(),
      [](const channel_info_t& ch) { return ch.is_viewing; });
  }
  fan::event::task_t request_idr_reset() {
    try {
      for (const auto& channel : channel_info) {
        if (channel.is_viewing) {
          Protocol_C2S_t::Channel_ScreenShare_ViewToShare_t rest;
          rest.ChannelID = channel.channel_id;
          rest.Flag = ProtocolChannel::ScreenShare::ChannelFlag::ResetIDR;
          co_await tcp_write(
            Protocol_C2S_t::Channel_ScreenShare_ViewToShare,
            &rest,
            sizeof(rest)
          );
        }
      }
    }
    catch (const std::exception& e) {
      fan::print("Failed to send IDR reset: " + std::string(e.what()));
    }
    catch (fan::exception_t e) {
      fan::print("Failed to send IDR reset: " + std::string(e.reason));
    }
    catch (...) {
      fan::print("Failed to send IDR reset: unknown error");
    }
    co_return;
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
    fan::time::timer joined_at;
    int flag = 0;
  };
  std::vector<channel_info_t> channel_info;


  std::vector<channel_list_info_t> available_channels;
  std::unordered_map<Protocol_ChannelID_t::Type, std::vector<session_info_t>> channel_sessions;
  bool did_just_join = false; // need manual reset from gui

#if use_input_control == 1
  MD_Mice_t mice;
  MD_Keyboard_t keyboard;
#endif

}ecps_backend;
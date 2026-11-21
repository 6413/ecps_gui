#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <set>
#include <algorithm>
#include <string>
#include <sstream>
#include <array>
#include <bitset>
#include <vector>

#include <WITCH/WITCH.h>
#include <WITCH/STR/psh.h>

#include _WITCH_PATH(MEM/MEM.h)
#include _WITCH_PATH(STR/common/common.h)
#include _WITCH_PATH(IO/IO.h)
#include _WITCH_PATH(IO/print.h)
#include _WITCH_PATH(HASH/SHA.h)
#include _WITCH_PATH(RAND/RAND.h)
#include _WITCH_PATH(VEC/VEC.h)
#include _WITCH_PATH(EV/EV.h)
#include _WITCH_PATH(NET/NET.h)
#include _WITCH_PATH(NET/TCP/TCP.h)

import fan.types.vector;
import fan.event.types;
import fan.time;
import fan.print;
import fan.network.crypto;

#include "Base.h"
#include "../client/version.h"

// WITCH's Windows NET backend only defines NET_SOL_SOCKET
#ifndef NET_SO_SNDBUF
  #define NET_SO_SNDBUF SO_SNDBUF
  #define NET_SO_RCVBUF SO_RCVBUF
#endif

// 1 prints the udp status lines below, same switch name as the client. Set it with -Decps_debug_prints=1
#ifndef ecps_debug_prints
  #define ecps_debug_prints 0
#endif

// where host video packets stop on their way through the server, printed every few seconds while any arrive
struct udp_stats_t {
  uint64_t received = 0;
  uint64_t bad_session = 0;
  uint64_t bad_crypto = 0;
  uint64_t bad_channel = 0;
  uint64_t not_host = 0;
  uint64_t stream_accepted = 0;
  uint64_t relayed = 0;
  uint64_t no_viewer_address = 0;
  std::chrono::steady_clock::time_point printed = std::chrono::steady_clock::now();
  uint64_t printed_received = 0;

  void report() {
  #if ecps_debug_prints >= 1
    auto now = std::chrono::steady_clock::now();
    if (now - printed < std::chrono::seconds(3) || received == printed_received) {
      return;
    }
    printed = now;
    printed_received = received;
    fan::print("udp: received", received, "bad_session", bad_session, "bad_crypto", bad_crypto,
      "stream_accepted", stream_accepted, "bad_channel", bad_channel, "not_host", not_host,
      "relayed_to_viewers", relayed, "viewer_without_udp_address", no_viewer_address);
  #endif
  }
}g_udp_stats;

template <typename T>
void
UDP_send(
  Protocol_SessionID_t SessionID,
  uint32_t ID,
  const T& Command,
  T CommandData,
  const void* data,
  IO_size_t size
) {
  auto Session = &g_pile->SessionList[SessionID];
  uint8_t buffer[2048];
  auto BasePacket = (ProtocolUDP::BasePacket_t*)buffer;
  BasePacket->SessionID = SessionID;
  BasePacket->ID = ID;
  BasePacket->Nonce = Session->UDP.Crypto.send_nonce++;
  BasePacket->Command = Command;
  auto CommandDataPacket = (T*)&BasePacket[1];
  *CommandDataPacket = CommandData;
  auto RestPacket = (uint8_t*)CommandDataPacket + T::dss;
  __builtin_memcpy(RestPacket, data, size);
  uint16_t TotalSize = Session->UDP.Crypto.seal(buffer, sizeof(ProtocolUDP::BasePacket_t), sizeof(ProtocolUDP::BasePacket_t) + T::dss + size, BasePacket->Nonce);
  if (!TotalSize) {
    return;
  }
  IO_ssize_t r = NET_sendto(&g_pile->udp, buffer, TotalSize, &Session->UDP.Address);
  if (r != TotalSize) {
    if (r == 0) {
      fan::print_throttled("UDP send buffer full, backing off\r\n");
    }
    else {
      fan::print_throttled("[INTERNAL ERROR] NET_sendto failed. wanted " + std::to_string(TotalSize) + " got " + std::to_string(r));
    }
  }
}

#include "channel.h"

#include "Session.h"
#include "TCPPeer.h"

#include "ScreenShare.h"
#include "FileTransfer.h"

bool TCP_Seal(NET_TCP_peer_t *peer, const void *Data, uintptr_t Size, std::vector<char> &Out){
  auto PeerData = (TCPMain_PeerData_t *)NET_TCP_GetPeerData(peer, g_pile->extid);
  auto &Crypto = g_pile->SessionList[PeerData->SessionID].TCP.Crypto;
  if(!Crypto.ready){
    return false;
  }
  Out = Crypto.seal((const char *)Data, Size);
  return true;
}

uint32_t TCPMain_state_cb(NET_TCP_peer_t* peer, TCPMain_SockData_t* SockData, TCPMain_PeerData_t* PeerData, uint32_t flag) {
  if (flag & NET_TCP_state_succ_e) {
    TCPPeer::Open(peer);
  }
  else do {
    if (!(flag & NET_TCP_state_init_e)) {
      break;
    }
    TCPPeer::Close(peer);
  } while (0);

  return 0;
}

uint32_t TCPMain_read_cb(
  NET_TCP_peer_t* peer,
  uint8_t* SockData,
  TCPMain_PeerData_t* PeerData,
  NET_TCP_QueuerReference_t QueuerReference,
  uint32_t* type,
  NET_TCP_Queue_t* Queue
) {
  uint8_t* ReadData;
  uintptr_t ReadSize;
  uint8_t _EventReadBuffer[4096];
  switch (*type) {
  case NET_TCP_QueueType_DynamicPointer: {
    ReadData = (uint8_t*)Queue->DynamicPointer.ptr;
    ReadSize = Queue->DynamicPointer.size;
    break;
  }
  case NET_TCP_QueueType_PeerEvent: {
    IO_fd_t peer_fd;
    EV_event_get_fd(&peer->event, &peer_fd);
    IO_ssize_t len = IO_read(&peer_fd, _EventReadBuffer, sizeof(_EventReadBuffer));
    if (len < 0) {
      NET_TCP_CloseHard(peer);
      return NET_TCP_EXT_PeerIsClosed_e;
    }
    ReadData = _EventReadBuffer;
    ReadSize = len;
    break;
  }
  case NET_TCP_QueueType_CloseHard: {
    return 0;
  }
  default: {
    WriteError("*type %lx\r\n", *type);
    __abort();
  }
  }
  std::vector<char> Plain;
  auto& TCPCrypto = g_pile->SessionList[PeerData->SessionID].TCP.Crypto;
  if (TCPCrypto.ready) {
    if (!TCPCrypto.open((const char*)ReadData, ReadSize, Plain)) {
      NET_TCP_CloseHard(peer);
      return NET_TCP_EXT_PeerIsClosed_e;
    }
    ReadData = (uint8_t*)Plain.data();
    ReadSize = Plain.size();
  }
  NET_TCP_UpdatePeerTimer(peer->parent->listener, peer->parent, peer);
  auto IsBodyless = [PeerData] {
    return PeerData->state == PeerState_Waitting_Data_e &&
      Protocol_C2S.NA(((ProtocolBasePacket_t*)PeerData->Buffer)->Command)->m_DSS == 0;
  };
  for (uintptr_t iSize = 0; iSize < ReadSize || IsBodyless();) {
    switch (PeerData->state) {
    case PeerState_Idle_e: {
      PeerData->iBuffer = 0;
      PeerData->Buffer = A_resize(0, sizeof(ProtocolBasePacket_t));
      PeerData->state = PeerState_Waitting_BasePacket_e;
      break;
    }
    case PeerState_Waitting_BasePacket_e: {
      uintptr_t size = ReadSize - iSize;
      uintptr_t needed_size = sizeof(ProtocolBasePacket_t) - PeerData->iBuffer;
      if (size > needed_size) {
        size = needed_size;
      }
      __builtin_memcpy(&PeerData->Buffer[PeerData->iBuffer], &ReadData[iSize], size);
      iSize += size;
      PeerData->iBuffer += size;
      if (PeerData->iBuffer == sizeof(ProtocolBasePacket_t)) {

        auto BasePacket = (ProtocolBasePacket_t*)PeerData->Buffer;
        //WriteInformation("Received packet: ID=%u, Command=%u, Max=%u\r\n", 
        //BasePacket->ID, BasePacket->Command, Protocol_C2S_t::GetMemberAmount());
        if (BasePacket->Command >= Protocol_C2S_t::GetMemberAmount()) {
          TWriteErrorClient("BasePacket->Command >= Protocol_C2S_t::GetMemberAmount()\r\n");
          TWriteErrorClient("Command: %u, Max: %u\r\n", BasePacket->Command, Protocol_C2S_t::GetMemberAmount());
          A_resize(PeerData->Buffer, 0);
          PeerData->state = PeerState_Idle_e;
          NET_TCP_CloseHard(peer);
          return NET_TCP_EXT_PeerIsClosed_e;
        }
        if (!TCPCrypto.ready && BasePacket->Command != Protocol_C2S_t::KeyExchange) {
          A_resize(PeerData->Buffer, 0);
          PeerData->state = PeerState_Idle_e;
          NET_TCP_CloseHard(peer);
          return NET_TCP_EXT_PeerIsClosed_e;
        }
        PeerData->Buffer = A_resize(PeerData->Buffer, sizeof(ProtocolBasePacket_t) + Protocol_C2S.NA(BasePacket->Command)->m_DSS);
        PeerData->state = PeerState_Waitting_Data_e;
      }
      break;
    }
    case PeerState_Waitting_Data_e: {
      auto BasePacket = (ProtocolBasePacket_t*)PeerData->Buffer;
      uintptr_t TotalSize = sizeof(ProtocolBasePacket_t) + Protocol_C2S.NA(BasePacket->Command)->m_DSS;
      uintptr_t size = ReadSize - iSize;
      uintptr_t needed_size = TotalSize - PeerData->iBuffer;
      if (size > needed_size) {
        size = needed_size;
      }
      __builtin_memcpy(&PeerData->Buffer[PeerData->iBuffer], &ReadData[iSize], size);
      iSize += size;
      PeerData->iBuffer += size;
      if (PeerData->iBuffer == TotalSize) {
        auto RestPacket = (uint8_t*)BasePacket + sizeof(ProtocolBasePacket_t);

        // lets preget things
        auto SessionID = PeerData->SessionID;
        auto Session = &g_pile->SessionList[SessionID];

        switch (BasePacket->Command) {
#include "ReadDone.h"
        default: {
          __unreachable();
        }
        }
      }
      break;
    }
    default: {
      __unreachable();
    }
    }
    continue;
  StateDone_gt:
    A_resize(PeerData->Buffer, 0);
    PeerData->state = PeerState_Idle_e;
  }
  return 0;
}

bool CompareCommand(const uint8_t* Command, uintptr_t* iCommand, uintptr_t CommandSize, const void* Str) {
  for (; *iCommand < CommandSize; (*iCommand)++) {
    if (!STR_ischar_blank(Command[*iCommand])) {
      break;
    }
  }
  uintptr_t StrSize = MEM_cstrlen(Str);
  if (StrSize != CommandSize) {
    return 0;
  }
  if (STR_ncasecmp(&Command[*iCommand], Str, StrSize)) {
    return 0;
  }
  return 1;
}

bool GetNextArgument(const uint8_t* Command, uintptr_t* iCommand, uintptr_t CommandSize) {
  for (; *iCommand < CommandSize; (*iCommand)++) {
    if (!STR_ischar_blank(Command[*iCommand])) {
      break;
    }
  }
  for (; *iCommand < CommandSize; (*iCommand)++) {
    if (STR_ischar_blank(Command[*iCommand])) {
      break;
    }
  }
  for (; *iCommand < CommandSize; (*iCommand)++) {
    if (!STR_ischar_blank(Command[*iCommand])) {
      return 1;
    }
  }
  return 0;
}

uintptr_t GetSizeOfArgument(const uint8_t* Command, uintptr_t* iCommand, uintptr_t CommandSize) {
  uintptr_t iCommandLocal = *iCommand;
  for (; iCommandLocal < CommandSize; iCommandLocal++) {
    if (STR_ischar_blank(Command[iCommandLocal])) {
      return iCommandLocal - *iCommand;
    }
  }
  return iCommandLocal - *iCommand;
}

void ProcessInput(uint8_t* Input, uintptr_t InputSize) {
  uintptr_t iCommand = 0;
  uintptr_t CommandSize = GetSizeOfArgument(Input, &iCommand, InputSize);
  if (CompareCommand(Input, &iCommand, CommandSize, "help")) {
    WriteInformation("commands:\r\n"
      "help - shows this\r\n"
    );
  }
  else {
    WriteInformation("failed to find command type help\r\n");
  }
}

bool IsInputLineDone(uint8_t** buffer, uintptr_t* size) {
  for (uintptr_t i = 0; i < *size; i++) {
    if ((*buffer)[i] == 0x7f || (*buffer)[i] == 0x08) {
      if (g_pile->InputSize) {
        g_pile->InputSize--;
      }
      continue;
    }
    if ((*buffer)[i] == '\n' || (*buffer)[i] == '\r') {
      if ((*buffer)[i] == '\r') {
        WriteInformation("\r\n");
      }
      *buffer += i + 1;
      *size -= i + 1;
      return 1;
    }
    g_pile->Input[g_pile->InputSize++] = (*buffer)[i];
  }
  return 0;
}

void evio_stdin_cb(EV_t* listener, EV_event_t* evio_stdin, uint32_t flag) {
  IO_fd_t event_fd;
  EV_event_get_fd(evio_stdin, &event_fd);

  uint8_t _buffer[0x1000];
  uint8_t* buffer = _buffer;
  IO_ssize_t size;
  size = IO_read(&event_fd, buffer, sizeof(_buffer));
  if (size <= 0) {
    // stdin closed/unreadable (no console, redirected, debugger)
  #if !defined(_WIN32)
    // a closed stdin stays readable, the event would fire again forever
    EV_event_stop(listener, evio_stdin);
  #endif
    // the WITCH EV backend can't stop a tty event on Windows (aborts), so just ignore it there
    return;
  }
  while (1) {
    if (!IsInputLineDone(&buffer, (uintptr_t*)&size)) {
      return;
    }
    ProcessInput(g_pile->Input, g_pile->InputSize);
    g_pile->InputSize = 0;
  }
}

struct recovery_request_t {
  uint16_t sequence;
  uint16_t missing_count;
};

void request_missing_from_host(Protocol_SessionID_t host_session_id, Protocol_ChannelID_t channel_id, ScreenShare_FrameTracker_t& frame) {
  uint8_t request_buffer[sizeof(recovery_request_t) + 512 * sizeof(uint16_t)];
  auto req = (recovery_request_t*)request_buffer;
  auto missing = (uint16_t*)(req + 1);
  req->sequence = frame.Sequence;
  req->missing_count = 0;
  for (uint16_t i = 0; i < frame.Expected && req->missing_count < 512; ++i) {
    if (!frame.Got[i]) {
      missing[req->missing_count++] = i;
    }
  }
  if (!req->missing_count) {
    return;
  }
  ProtocolUDP::S2C_t::Channel_ScreenShare_RecoveryRequest_t rest;
  rest.ChannelID = channel_id;
  UDP_send(host_session_id, 0, ProtocolUDP::S2C_t::Channel_ScreenShare_RecoveryRequest,
    rest, request_buffer, sizeof(recovery_request_t) + req->missing_count * sizeof(uint16_t));
}

void track_server_packet(Protocol_SessionID_t host_session_id,
  Protocol_ChannelID_t channel_id,
  uint16_t sequence,
  uint16_t current,
  uint16_t possible) {
  auto ChannelData = (Channel_ScreenShare_Data_t*)g_pile->ChannelList[channel_id].Buffer;
  uint64_t now = EV_nowi(&g_pile->listener);

  // host_session_id is whoever sent the packet, every sender has frames of its own
  auto& stream = ChannelData->Streams[(uint32_t)host_session_id.i];
  auto& frame = stream.Frames[sequence % stream.Frames.size()];
  if (frame.Sequence != sequence) {
    frame = {};
    frame.Sequence = sequence;
    frame.LastRequestAt = now;
  }
  if (current == 0) {
    frame.Expected = possible;
  }
  if (current < frame.Got.size() && !frame.Got[current]) {
    frame.Got.set(current);
    ++frame.Received;
  }

  for (auto& f : stream.Frames) {
    bool incomplete = f.Sequence != (uint16_t)-1 && f.Expected && f.Received < f.Expected;
    if (incomplete && f.Requests < 3 && now - f.LastRequestAt > 30000000ULL && f.Sequence != sequence) {
      request_missing_from_host(host_session_id, channel_id, f);
      f.LastRequestAt = now;
      ++f.Requests;
    }
  }
}

// reads and handles one datagram, false when the socket has nothing more
static bool udp_read_one() {
  uint8_t buffer[0x800];
  NET_addr_t dstaddr;
  IO_ssize_t size = NET_recvfrom(&g_pile->udp, buffer, sizeof(buffer), &dstaddr);
  if (size < 0) {
    return false;
  }
  ++g_udp_stats.received;
  g_udp_stats.report();
  if (size == sizeof(buffer)) {
    fan::print_throttled("udp packet too big, dropping\r\n");
    return true;
  }

  if ((uintptr_t)size < sizeof(ProtocolUDP::BasePacket_t)) {
    // nothing says whether an empty read is an empty datagram or an empty socket, the event fires again if there is more
    if (size == 0) {
      return false;
    }
    WriteInformation("invalid packet came\n");
    return true;
  }

  auto BasePacket = (ProtocolUDP::BasePacket_t*)buffer;
  auto SessionID = BasePacket->SessionID;
  if (Session::IsInvalid(SessionID) == true) {
    ++g_udp_stats.bad_session;
    return true;
  }
  auto Session = &g_pile->SessionList[SessionID];
  size = Session->UDP.Crypto.open(buffer, sizeof(ProtocolUDP::BasePacket_t), size, BasePacket->Nonce);
  if (!size) {
    ++g_udp_stats.bad_crypto;
    return true;
  }

  switch (BasePacket->Command) {
  case ProtocolUDP::C2S_t::KeepAlive: {

    g_pile->SessionList[SessionID].UDP.Address = dstaddr;

    UDP_send(BasePacket->SessionID, 0, ProtocolUDP::S2C_t::KeepAlive, {}, 0, 0);

    break;
  }
  case ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData: {
    auto RestPacket = (ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t*)&BasePacket[1];
    IO_size_t RestPacketSize = size - sizeof(ProtocolUDP::BasePacket_t);
    if (RestPacketSize < sizeof(*RestPacket)) {
      break;
    }
    if (IsAnythingInvalid(SessionID, RestPacket->ChannelID, RestPacket->ChannelSessionID) == true) {
      ++g_udp_stats.bad_channel;
      return true;
    }
    /* any member can share into the channel, the sender is the source of what it sends */

    auto StreamData = (uint8_t*)&RestPacket[1];
    IO_size_t StreamSize = size - (StreamData - buffer);

    // the header is read below, a shorter packet would have it come from whatever was in the buffer before
    if (StreamSize < sizeof(ScreenShare_StreamHeader_Body_t)) {
      break;
    }
    auto Body = (ScreenShare_StreamHeader_Body_t*)StreamData;
    uint16_t current = Body->GetCurrent();
    uint16_t sequence = Body->GetSequence();
    if (current == 0 && StreamSize < sizeof(ScreenShare_StreamHeader_Head_t)) {
      break;
    }
    ++g_udp_stats.stream_accepted;

    uint16_t possible = 0;
    if (current == 0) {
      auto Head = (ScreenShare_StreamHeader_Head_t*)StreamData;
      possible = Head->GetPossible();
    }

    ScreenShare_SetStreamer(RestPacket->ChannelID, SessionID, true, false);
    track_server_packet(SessionID, RestPacket->ChannelID, sequence, current, possible);

    ScreenShare_StreamPacket(RestPacket->ChannelID, SessionID, StreamData, StreamSize);
    break;
  }
  case ProtocolUDP::C2S_t::Channel_ScreenShare_Host_AudioData: {
    auto RestPacket = (ProtocolUDP::C2S_t::Channel_ScreenShare_Host_AudioData_t*)&BasePacket[1];
    IO_size_t RestPacketSize = size - sizeof(ProtocolUDP::BasePacket_t);
    if (RestPacketSize < sizeof(*RestPacket) + sizeof(ScreenShare_AudioHeader_t)) {
      break;
    }
    if (IsAnythingInvalid(SessionID, RestPacket->ChannelID, RestPacket->ChannelSessionID) == true) {
      break;
    }
    auto AudioData = (uint8_t*)&RestPacket[1];
    ScreenShare_AudioPacket(RestPacket->ChannelID, SessionID, AudioData, size - (AudioData - buffer));
    break;
  }
  case ProtocolUDP::C2S_t::Channel_Voice_Data: {
    auto RestPacket = (ProtocolUDP::C2S_t::Channel_Voice_Data_t*)&BasePacket[1];
    IO_size_t RestPacketSize = size - sizeof(ProtocolUDP::BasePacket_t);
    if (RestPacketSize < sizeof(*RestPacket)) {
      break;
    }
    if (IsAnythingInvalid(SessionID, RestPacket->ChannelID, RestPacket->ChannelSessionID) == true) {
      break;
    }
    auto VoiceData = (uint8_t*)&RestPacket[1];
    ScreenShare_VoicePacket(RestPacket->ChannelID, SessionID, VoiceData, size - (VoiceData - buffer));
    break;
  }
  case ProtocolUDP::C2S_t::Channel_Chat_Probe: {
    auto RestPacket = (ProtocolUDP::C2S_t::Channel_Chat_Probe_t*)&BasePacket[1];
    IO_size_t RestPacketSize = size - sizeof(ProtocolUDP::BasePacket_t);
    if (RestPacketSize < sizeof(*RestPacket)) {
      break;
    }
    if (IsAnythingInvalid(SessionID, RestPacket->ChannelID, RestPacket->ChannelSessionID) == true) {
      break;
    }
    ScreenShare_ChatProbe(RestPacket->ChannelID, SessionID);
    break;
  }
  case ProtocolUDP::C2S_t::Channel_Rename: {
    auto RestPacket = (ProtocolUDP::C2S_t::Channel_Rename_t*)&BasePacket[1];
    IO_size_t RestPacketSize = size - sizeof(ProtocolUDP::BasePacket_t);
    if (RestPacketSize < sizeof(*RestPacket)) {
      break;
    }
    if (IsAnythingInvalid(SessionID, RestPacket->ChannelID, RestPacket->ChannelSessionID) == true) {
      break;
    }
    auto Channel = &g_pile->ChannelList[RestPacket->ChannelID];
    if (Channel->Type != Protocol::ChannelType_ScreenShare_e) {
      break;
    }
    auto ChannelData = (Channel_ScreenShare_Data_t*)Channel->Buffer;
    /* only the host names it */
    if (ChannelData->HostSessionID != SessionID) {
      break;
    }
    /* up to the first control character, no leading or trailing spaces */
    char Title[sizeof(ChannelData->Title)] = {};
    size_t Length = 0;
    for (size_t i = 0; i < sizeof(RestPacket->Name) && Length < sizeof(Title) - 1; i++) {
      unsigned char c = (unsigned char)RestPacket->Name[i];
      if (c < 0x20 || c == 0x7f) {
        break;
      }
      if (c == ' ' && Length == 0) {
        continue;
      }
      Title[Length++] = (char)c;
    }
    while (Length && Title[Length - 1] == ' ') {
      Title[--Length] = 0;
    }
    if (std::strcmp(Title, ChannelData->Title) == 0) {
      break;
    }
    std::memcpy(ChannelData->Title, Title, sizeof(Title));
    Channel_BroadcastChannelList();
    break;
  }
  case ProtocolUDP::C2S_t::Channel_ScreenShare_RecoveryRequest: {
    auto RestPacket = (ProtocolUDP::C2S_t::Channel_ScreenShare_RecoveryRequest_t*)&BasePacket[1];
    IO_size_t RestPacketSize = size - sizeof(ProtocolUDP::BasePacket_t);
    if (RestPacketSize < sizeof(*RestPacket)) {
      break;
    }

    if (IsAnythingInvalid(SessionID, RestPacket->ChannelID, RestPacket->ChannelSessionID) == true) {
      break;
    }

    auto recovery_data = (uint8_t*)&RestPacket[1];
    IO_size_t recovery_size = size - (recovery_data - buffer);

    if (recovery_size < sizeof(recovery_request_t)) {
      break;
    }

    auto req = (recovery_request_t*)recovery_data;

    if (recovery_size < sizeof(recovery_request_t) + req->missing_count * sizeof(uint16_t)) {
      break;
    }

    auto missing_indices = (uint16_t*)(req + 1);

    ProcessRecoveryRequest(SessionID, RestPacket->ChannelID, req->sequence, missing_indices, req->missing_count);

    break;
  }
  }
  return true;
}

void evio_udp_cb(EV_t* listener, EV_event_t* evio_udp, uint32_t flag) {
  // a stream is hundreds of packets a second, one loop iteration per packet would be most of the work.
  // The limit lets tcp and the timers run between bursts
  for (uint32_t i = 0; i < 64 && udp_read_one(); ++i) {}
}

int main(int argc, char** argv) {
  fan::print(std::string("ecps server v") + ecps_version);
  uint16_t port = 43254;
  if (argc != 2) {
    fan::print("usage:a.exe <port number>");
    fan::print("defaulting to port:", port);
  }
  else {
    port = std::stoul(argv[1]);
  }

  g_pile = new pile_t;

  sint32_t err;

  RAND_hard_ram(g_pile->ServerRandom, sizeof(g_pile->ServerRandom));

  if (std::string key_error; !fan::network::crypto::load_or_create_key("ecps_server.key", g_pile->StaticPrivate, g_pile->StaticPublic, &key_error)) {
    fan::print("failed to load or create ecps_server.key:", key_error);
    std::exit(1);
  }
  fan::print("server key:", fan::network::crypto::to_hex(g_pile->StaticPublic.data(), g_pile->StaticPublic.size()));

  EV_open(&g_pile->listener);

  EV_event_t evio_stdin;
  IO_fd_t fd_stdin;
  IO_fd_set(&fd_stdin, FD_IN);
  EV_event_init_fd(&evio_stdin, &fd_stdin, evio_stdin_cb, EV_READ);
  EV_event_start(&g_pile->listener, &evio_stdin);

  if (NET_socket2(NET_AF_INET, NET_SOCK_DGRAM | NET_SOCK_NONBLOCK, NET_IPPROTO_UDP, &g_pile->udp) < 0) {
    WriteInformation("failed to create udp socket %lx\r\n", g_pile->udp);
    std::exit(1);
  }

  if (NET_setsockopt(&g_pile->udp, NET_SOL_SOCKET, NET_SO_SNDBUF, 8388608) < 0) {
    WriteInformation("Failed to set SO_SNDBUF\r\n");
  }

  if (NET_setsockopt(&g_pile->udp, NET_SOL_SOCKET, NET_SO_RCVBUF, 8388608) < 0) {
    WriteInformation("Failed to set SO_RCVBUF\r\n");
  }

  NET_addr_t udpaddr;
  udpaddr.ip = NET_INADDR_ANY;
  udpaddr.port = port;
  err = NET_bind(&g_pile->udp, &udpaddr);
  if (err) {
    WriteInformation("udp bind error %ld (is another server already using port %u?)\r\n", err, (unsigned)port);
    std::exit(1);
  }

  EV_event_t evio_udp;
  EV_event_init_socket(&evio_udp, &g_pile->udp, evio_udp_cb, EV_READ);
  EV_event_start(&g_pile->listener, &evio_udp);

  g_pile->TCP = NET_TCP_alloc(&g_pile->listener);
  g_pile->TCP->ssrcaddr.port = port;

  g_pile->extid = NET_TCP_EXT_new(g_pile->TCP, sizeof(TCPMain_SockData_t), sizeof(TCPMain_PeerData_t));
  TCPMain_SockData_t* SockData = (TCPMain_SockData_t*)NET_TCP_GetSockData(g_pile->TCP, g_pile->extid);

  NET_TCP_layer_state_open(g_pile->TCP, g_pile->extid, (NET_TCP_cb_state_t)TCPMain_state_cb);
  SockData->ReadLayerID = NET_TCP_layer_read_open(g_pile->TCP, g_pile->extid, (NET_TCP_cb_read_t)TCPMain_read_cb, A_resize, 0, 0);

  NET_TCP_open(g_pile->TCP);
  err = NET_TCP_listen(g_pile->TCP);
  if (err) {
    WriteInformation("listen error %ld (is another server already using port %u?)\r\n", err, (unsigned)port);
    std::exit(1);
  }
  EV_event_start(&g_pile->listener, &g_pile->TCP->ev);

  g_pile->InputSize = 0;

  EV_start(&g_pile->listener);
  return 0;
}

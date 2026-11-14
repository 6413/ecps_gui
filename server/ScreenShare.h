Protocol_CI_t
ScreenShare_AddPeer
(
  ChannelList_NodeReference_t ChannelID,
  Protocol_SessionID_t SessionID,
  Protocol_ChannelSessionID_t *ResultNR
){
  auto Channel = &g_pile->ChannelList[ChannelID];
  { /* double join check */
    auto nr = Channel->SessionList.GetNodeFirst();
    while(nr != Channel->SessionList.dst){
      auto n = Channel->SessionList.GetNodeByReference(nr);
      if(n->data.SessionID == SessionID){
        *ResultNR = nr;
        return Protocol_S2C_t::JoinChannel_OK;
      }
      nr = n->NextNodeReference;
    }
  }

  auto ChannelSessionID = Channel->SessionList.NewNodeLast();
  auto ChannelSession = &Channel->SessionList[ChannelSessionID];
  ChannelSession->SessionID = SessionID;

  auto Session = &g_pile->SessionList[SessionID];

  auto SessionChannelID = Session->ChannelList.NewNodeLast();
  auto SessionChannel = &Session->ChannelList[SessionChannelID];
  SessionChannel->ChannelID = ChannelID;

  ChannelSession->SessionChannelID = SessionChannelID;
  SessionChannel->ChannelSessionID = ChannelSessionID;

  *ResultNR = ChannelSessionID;
  return Protocol_S2C_t::JoinChannel_OK;
}

bool
ScreenShare_IsSessionHost(
  Protocol_SessionID_t SessionID,
  Protocol_ChannelID_t ChannelID
){
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  return ChannelData->HostSessionID == SessionID;
}

void
ScreenShare_SessionQuit
(
  Protocol_SessionID_t SessionID,
  SessionChannelList_NodeReference_t SessionChannelID
){
  auto Session = &g_pile->SessionList[SessionID];
  if(Session->ChannelList.IsNRSentinel(SessionChannelID)){
    /* bad */
    return;
  }
  if(Session->ChannelList.inri(SessionChannelID)){
    /* bad */
    return;
  }
  if(Session->ChannelList.IsNodeReferenceRecycled(SessionChannelID)){
    /* bad */
    return;
  }
  auto SessionChannel = &Session->ChannelList[SessionChannelID];

  auto Channel = &g_pile->ChannelList[SessionChannel->ChannelID];
  ChannelSessionList_NodeReference_t ChannelSessionID = SessionChannel->ChannelSessionID;
  Session->ChannelList.unlrec(SessionChannelID);
  Channel->SessionList.unlrec(ChannelSessionID);
}

void StorePacketForRecovery(Protocol_ChannelID_t channel_id, uint16_t sequence, 
                           uint16_t current, const void* data, size_t size) {
  if (IsChannelInvalid(channel_id)) return;
  
  auto Channel = &g_pile->ChannelList[channel_id];
  auto ChannelData = (Channel_ScreenShare_Data_t*)Channel->Buffer;
  
  // sequence and current are the host's numbers. Unchecked, 4096 packets for each kept sequence
  // would have the server hold hundreds of megabytes for one channel
  constexpr size_t MaxStoredBytes = 32 * 1024 * 1024;

  auto& tracker = ChannelData->Frames[sequence % ChannelData->Frames.size()];
  if (tracker.Sequence == sequence && tracker.Expected && current >= tracker.Expected) {
    return;
  }

  auto& packets = ChannelData->SentPackets[sequence];
  if (packets.size() <= current) {
    packets.resize(current + 1);
  }

  auto& slot = packets[current];
  if (ChannelData->SentBytes - slot.size() + size <= MaxStoredBytes) {
    ChannelData->SentBytes -= slot.size();
    slot.assign(
      static_cast<const uint8_t*>(data),
      static_cast<const uint8_t*>(data) + size
    );
    ChannelData->SentBytes += size;
  }

  if (ChannelData->SentPackets.size() > 50) {
    std::erase_if(ChannelData->SentPackets, [sequence, ChannelData](const auto& p) {
      if (((sequence - p.first) & 0xfff) <= 32) {
        return false;
      }
      for (const auto& packet : p.second) {
        ChannelData->SentBytes -= packet.size();
      }
      return true;
    });
  }
}

void ProcessRecoveryRequest(Protocol_SessionID_t requester, Protocol_ChannelID_t channel_id, uint16_t sequence,
                           const uint16_t* missing_indices, uint16_t missing_count) {
  if (IsChannelInvalid(channel_id)) return;
  
  auto Channel = &g_pile->ChannelList[channel_id];
  auto ChannelData = (Channel_ScreenShare_Data_t*)Channel->Buffer;
  
  auto it = ChannelData->SentPackets.find(sequence);
  if (it == ChannelData->SentPackets.end()) {
    return;
  }
  
  auto& packets = it->second;
  auto Session = &g_pile->SessionList[requester];
  if(Session->UDP.Address.ip == 0){
    return;
  }
  // one small request can ask for a megabyte of resends, and they are sent from inside the event loop.
  // A real viewer asks for a few packets of a few frames, this is far above that
  constexpr uint32_t MaxResendsPerSecond = 2000;
  uint64_t now = EV_nowi(&g_pile->listener);
  if (now - Session->UDP.RecoveryWindowAt >= 1000000000ULL) {
    Session->UDP.RecoveryWindowAt = now;
    Session->UDP.RecoverySent = 0;
  }
  ProtocolUDP::S2C_t::Channel_ScreenShare_View_StreamData_t cd;
  cd.ChannelID = channel_id;
  for (uint16_t i = 0; i < missing_count; ++i) {
    if (Session->UDP.RecoverySent >= MaxResendsPerSecond) {
      break;
    }
    uint16_t packet_index = missing_indices[i];
    if (packet_index < packets.size() && !packets[packet_index].empty()) {
      UDP_send(requester, 0, ProtocolUDP::S2C_t::Channel_ScreenShare_View_StreamData,
              cd, packets[packet_index].data(), packets[packet_index].size());
      ++Session->UDP.RecoverySent;
    }
  }
}

void ScreenShare_StreamPacket(Protocol_ChannelID_t ChannelID, void *Data, IO_size_t DataSize) {
  auto Body = (ScreenShare_StreamHeader_Body_t*)Data;
  uint16_t sequence = Body->GetSequence();
  uint16_t current = Body->GetCurrent();
  
  StorePacketForRecovery(ChannelID, sequence, current, Data, DataSize);
  
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelSubData = (Channel_ScreenShare_Data_t *)Channel->Buffer;

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == ChannelSubData->HostSessionID){
      continue;
    }
    auto Session = &g_pile->SessionList[n->data.SessionID];
    if(Session->UDP.Address.ip == 0){
      ++g_udp_stats.no_viewer_address;
      continue;
    }
    ProtocolUDP::S2C_t::Channel_ScreenShare_View_StreamData_t cd;
    cd.ChannelID = ChannelID;
    UDP_send(n->data.SessionID, 0, ProtocolUDP::S2C_t::Channel_ScreenShare_View_StreamData, cd, Data, DataSize);
    ++g_udp_stats.relayed;
  }
}

void ScreenShare_AudioPacket(Protocol_ChannelID_t ChannelID, void *Data, IO_size_t DataSize) {
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelSubData = (Channel_ScreenShare_Data_t *)Channel->Buffer;

  ProtocolUDP::S2C_t::Channel_ScreenShare_View_AudioData_t cd;
  cd.ChannelID = ChannelID;
  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == ChannelSubData->HostSessionID){
      continue;
    }
    if(g_pile->SessionList[n->data.SessionID].UDP.Address.ip == 0){
      continue;
    }
    UDP_send(n->data.SessionID, 0, ProtocolUDP::S2C_t::Channel_ScreenShare_View_AudioData, cd, Data, DataSize);
  }
}

void
ScreenShare_SendFlagTo(
  ChannelList_NodeReference_t ChannelID,
  Protocol_SessionID_t SessionID
){
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;

  Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewSetFlag_t rest;
  rest.ChannelID = ChannelID;
  rest.Flag = ChannelData->Flag;
  Session::WriteCommand(
    SessionID,
    0,
    Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewSetFlag,
    rest);
}

/* MIDI has its own flag: no stream needed, and the viewer still can't touch the mouse or keyboard */
bool
ScreenShare_CanSendMidiToHost(
  ChannelList_NodeReference_t ChannelID,
  Protocol_SessionID_t SessionID
){
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;

  if(!(ChannelData->Flag & ProtocolChannel::ScreenShare::ChannelFlag::MidiControl)){
    return false;
  }
  if(ChannelData->HostSessionID == SessionID){
    return false;
  }

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == SessionID){
      return true;
    }
  }
  return false;
}

/* the flag alone isnt enough, anyone logged in can name any channel */
bool
ScreenShare_CanControlHost(
  ChannelList_NodeReference_t ChannelID,
  Protocol_SessionID_t SessionID
){
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;

  if(!(ChannelData->Flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
    return false;
  }
  if(ChannelData->HostSessionID == SessionID){
    return false;
  }

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == SessionID){
      return true;
    }
  }
  return false;
}

void
ScreenShare_FlagIsChanged
(
  ChannelList_NodeReference_t ChannelID
){
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelSubData = (Channel_ScreenShare_Data_t *)Channel->Buffer;

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == ChannelSubData->HostSessionID){
      /* lets dont send to self */
      continue;
    }
    ScreenShare_SendFlagTo(ChannelID, n->data.SessionID);
  }
}

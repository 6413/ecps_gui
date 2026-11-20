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

bool ScreenShare_IsMember(Protocol_SessionID_t SessionID, Protocol_ChannelID_t ChannelID){
  if(IsChannelInvalid(ChannelID) == true){
    return false;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto nr = Channel->SessionList.GetNodeFirst();
  while(nr != Channel->SessionList.dst){
    auto n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == SessionID){
      return true;
    }
    nr = n->NextNodeReference;
  }
  return false;
}

/* the session whose video a viewer gets in a channel: the one it picked, else the host (older clients never pick) */
Protocol_SessionID_t ScreenShare_WatchedSource(Protocol_SessionID_t Viewer, Protocol_ChannelID_t ChannelID){
  auto Session = &g_pile->SessionList[Viewer];
  if(Session->Watching && Session->WatchChannel == ChannelID.i){
    return Protocol_SessionID_t(Session->WatchSource);
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  return ChannelData->HostSessionID;
}

void ScreenShare_FillStreamers(Protocol_ChannelID_t ChannelID, Protocol_S2C_t::Channel_ScreenShare_View_Streamers_t &Message){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  Message.ChannelID = ChannelID;
  Message.Count = 0;
  for(uint32_t Streamer : ChannelData->Streamers){
    if(Message.Count >= 8){
      break;
    }
    Message.Streamers[Message.Count++] = Protocol_SessionID_t((Protocol_SessionID_t::Type)Streamer);
  }
}

void ScreenShare_SendStreamersTo(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID){
  Protocol_S2C_t::Channel_ScreenShare_View_Streamers_t Message{};
  ScreenShare_FillStreamers(ChannelID, Message);
  Session::WriteCommand(SessionID, 0, Protocol_S2C_t::Channel_ScreenShare_View_Streamers, Message);
}

void ScreenShare_BroadcastStreamers(Protocol_ChannelID_t ChannelID){
  Protocol_S2C_t::Channel_ScreenShare_View_Streamers_t Message{};
  ScreenShare_FillStreamers(ChannelID, Message);
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto nr = Channel->SessionList.GetNodeFirst();
  while(nr != Channel->SessionList.dst){
    auto n = Channel->SessionList.GetNodeByReference(nr);
    auto Peer = g_pile->SessionList[n->data.SessionID].TCP.peer;
    if(Peer != nullptr && g_pile->SessionList[n->data.SessionID].TCP.Crypto.ready){
      Session::WriteCommand(n->data.SessionID, 0, Protocol_S2C_t::Channel_ScreenShare_View_Streamers, Message);
    }
    nr = n->NextNodeReference;
  }
}

void ScreenShare_SendVoiceMembers(Protocol_ChannelID_t ChannelID, const Protocol_SessionID_t *Only = nullptr){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  Protocol_S2C_t::Channel_Voice_Members_t Message{};
  Message.ChannelID = ChannelID;
  for(uint32_t Member : ChannelData->Voice){
    Message.Members[Message.Count++] = Protocol_SessionID_t((Protocol_SessionID_t::Type)Member);
  }
  if(Only != nullptr){
    Session::WriteCommand(*Only, 0, Protocol_S2C_t::Channel_Voice_Members, Message);
    return;
  }
  for(uint32_t Aware : ChannelData->VoiceAware){
    Session::WriteCommand(Protocol_SessionID_t((Protocol_SessionID_t::Type)Aware), 0, Protocol_S2C_t::Channel_Voice_Members, Message);
  }
}

void ScreenShare_SetVoice(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID, bool Joined){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  uint32_t Id = (uint32_t)SessionID.i;
  if(std::find(ChannelData->VoiceAware.begin(), ChannelData->VoiceAware.end(), Id) == ChannelData->VoiceAware.end()){
    ChannelData->VoiceAware.push_back(Id);
  }
  auto It = std::find(ChannelData->Voice.begin(), ChannelData->Voice.end(), Id);
  if(Joined && It == ChannelData->Voice.end() && ChannelData->Voice.size() < 16){
    ChannelData->Voice.push_back(Id);
  }
  else if(!Joined && It != ChannelData->Voice.end()){
    ChannelData->Voice.erase(It);
  }
  else{
    ScreenShare_SendVoiceMembers(ChannelID, &SessionID);
    return;
  }
  ScreenShare_SendVoiceMembers(ChannelID);
}

void ScreenShare_VoiceQuit(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  uint32_t Id = (uint32_t)SessionID.i;
  /* leaving the channel ends its chat for this session too */
  std::erase(ChannelData->ChatAware, Id);
  std::erase(ChannelData->VoiceAware, Id);
  if(std::erase(ChannelData->Voice, Id) != 0){
    ScreenShare_SendVoiceMembers(ChannelID);
  }
}

void ScreenShare_VoicePacket(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t Source, void *Data, IO_size_t DataSize){
  if(DataSize < sizeof(ScreenShare_AudioHeader_t) + 1){
    auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
    bool InVoice = std::find(ChannelData->Voice.begin(), ChannelData->Voice.end(), (uint32_t)Source.i) != ChannelData->Voice.end();
    ScreenShare_SetVoice(ChannelID, Source, InVoice);
    return;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  if(std::find(ChannelData->Voice.begin(), ChannelData->Voice.end(), (uint32_t)Source.i) == ChannelData->Voice.end()){
    return;
  }
  ProtocolUDP::S2C_t::Channel_Voice_Data_t cd;
  cd.ChannelID = ChannelID;
  cd.SessionID = Source;
  for(uint32_t Member : ChannelData->Voice){
    Protocol_SessionID_t Target((Protocol_SessionID_t::Type)Member);
    if(Target == Source || g_pile->SessionList[Target].UDP.Address.ip == 0){
      continue;
    }
    UDP_send(Target, 0, ProtocolUDP::S2C_t::Channel_Voice_Data, cd, Data, DataSize);
  }
}

/* Explicit is a session saying it started or stopped. Without it (video simply arriving) a session is added once
   and only a close or a stop from it takes it away again, so a few late packets can't bring back a stopped one */
void ScreenShare_SetStreamer(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID, bool Streaming, bool Explicit){
  if(IsChannelInvalid(ChannelID) == true){
    return;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  uint32_t Id = (uint32_t)SessionID.i;

  if(Explicit && std::find(ChannelData->Announced.begin(), ChannelData->Announced.end(), Id) == ChannelData->Announced.end()){
    ChannelData->Announced.push_back(Id);
  }
  bool Announced = std::find(ChannelData->Announced.begin(), ChannelData->Announced.end(), Id) != ChannelData->Announced.end();
  if(!Explicit && Announced && Streaming){
    return;
  }

  auto It = std::find(ChannelData->Streamers.begin(), ChannelData->Streamers.end(), Id);
  bool Changed = false;
  if(Streaming && It == ChannelData->Streamers.end() && ChannelData->Streamers.size() < 8){
    ChannelData->Streamers.push_back(Id);
    Changed = true;
  }
  else if(!Streaming && It != ChannelData->Streamers.end()){
    ChannelData->Streamers.erase(It);
    Changed = true;
  }
  if(!Streaming){
    ChannelData->Streams.erase(Id);
    if(!Explicit){
      std::erase(ChannelData->Announced, Id);
      ScreenShare_VoiceQuit(ChannelID, SessionID);
    }
  }
  if(Changed){
    ScreenShare_BroadcastStreamers(ChannelID);
  }
}

void StorePacketForRecovery(Protocol_ChannelID_t channel_id, Protocol_SessionID_t source, uint16_t sequence,
                           uint16_t current, const void* data, size_t size) {
  if (IsChannelInvalid(channel_id)) return;

  auto Channel = &g_pile->ChannelList[channel_id];
  auto ChannelData = (Channel_ScreenShare_Data_t*)Channel->Buffer;
  auto& stream = ChannelData->Streams[(uint32_t)source.i];

  // sequence and current are the sender's numbers. Unchecked, 4096 packets for each kept sequence
  // would have the server hold hundreds of megabytes for one channel
  constexpr size_t MaxStoredBytes = 32 * 1024 * 1024;

  auto& tracker = stream.Frames[sequence % stream.Frames.size()];
  if (tracker.Sequence == sequence && tracker.Expected && current >= tracker.Expected) {
    return;
  }

  auto& packets = stream.SentPackets[sequence];
  if (packets.size() <= current) {
    packets.resize(current + 1);
  }

  auto& slot = packets[current];
  if (stream.SentBytes - slot.size() + size <= MaxStoredBytes) {
    stream.SentBytes -= slot.size();
    slot.assign(
      static_cast<const uint8_t*>(data),
      static_cast<const uint8_t*>(data) + size
    );
    stream.SentBytes += size;
  }

  if (stream.SentPackets.size() > 50) {
    std::erase_if(stream.SentPackets, [sequence, &stream](const auto& p) {
      if (((sequence - p.first) & 0xfff) <= 32) {
        return false;
      }
      for (const auto& packet : p.second) {
        stream.SentBytes -= packet.size();
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

  // the resends come from the video of the session this viewer watches
  Protocol_SessionID_t source = ScreenShare_WatchedSource(requester, channel_id);
  auto stream_it = ChannelData->Streams.find((uint32_t)source.i);
  if (stream_it == ChannelData->Streams.end()) {
    return;
  }
  auto it = stream_it->second.SentPackets.find(sequence);
  if (it == stream_it->second.SentPackets.end()) {
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

/* Source is the session the video comes from. It goes to the viewers that picked it, and to the ones that picked
   nothing when it is the host */
void ScreenShare_StreamPacket(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t Source, void *Data, IO_size_t DataSize) {
  auto Body = (ScreenShare_StreamHeader_Body_t*)Data;
  uint16_t sequence = Body->GetSequence();
  uint16_t current = Body->GetCurrent();

  StorePacketForRecovery(ChannelID, Source, sequence, current, Data, DataSize);

  auto Channel = &g_pile->ChannelList[ChannelID];

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == Source){
      continue;
    }
    if(ScreenShare_WatchedSource(n->data.SessionID, ChannelID) != Source){
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

void ScreenShare_AudioPacket(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t Source, void *Data, IO_size_t DataSize) {
  auto Channel = &g_pile->ChannelList[ChannelID];

  ProtocolUDP::S2C_t::Channel_ScreenShare_View_AudioData_t cd;
  cd.ChannelID = ChannelID;
  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == Source){
      continue;
    }
    if(ScreenShare_WatchedSource(n->data.SessionID, ChannelID) != Source){
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

/* ---- chat ---- */

/* a session asks for the channel's chat: it gets the history and the end marker, and the chat from then on */
void ScreenShare_ChatProbe(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID){
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type != Protocol::ChannelType_ScreenShare_e){
    return;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  uint32_t Id = (uint32_t)SessionID.i;
  if(std::find(ChannelData->ChatAware.begin(), ChannelData->ChatAware.end(), Id) == ChannelData->ChatAware.end()){
    ChannelData->ChatAware.push_back(Id);
  }
  for(const auto &Message : ChannelData->ChatHistory){
    Session::WriteCommand(SessionID, 0, Protocol_S2C_t::Channel_Chat_Message, Message);
  }
  Protocol_S2C_t::Channel_Chat_History_End_t End;
  End.ChannelID = ChannelID;
  Session::WriteCommand(SessionID, 0, Protocol_S2C_t::Channel_Chat_History_End, End);
}

/* a message into the channel: kept, and sent to everyone in it who asked for the chat, the sender included */
void ScreenShare_ChatSend(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID, const char *Text, uint16_t Length){
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type != Protocol::ChannelType_ScreenShare_e){
    return;
  }
  if(Length == 0){
    return;
  }
  if(Length > Protocol::ChatTextMax){
    Length = Protocol::ChatTextMax;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  /* the sender is in the chat, also when its probe was lost */
  uint32_t SenderId = (uint32_t)SessionID.i;
  if(std::find(ChannelData->ChatAware.begin(), ChannelData->ChatAware.end(), SenderId) == ChannelData->ChatAware.end()){
    ChannelData->ChatAware.push_back(SenderId);
  }

  Protocol_S2C_t::Channel_Chat_Message_t Message;
  __builtin_memset(&Message, 0, sizeof(Message));
  Message.ChannelID = ChannelID;
  Message.From = SessionID;
  snprintf(Message.Name, sizeof(Message.Name), "%s", GetSessionUsername(SessionID));
  Message.Id = ChannelData->ChatNextId++;
  Message.Time = (int64_t)std::time(nullptr);
  Message.Length = Length;
  __builtin_memcpy(Message.Text, Text, Length);

  ChannelData->ChatHistory.push_back(Message);
  while(ChannelData->ChatHistory.size() > Protocol::ChatHistoryMax){
    ChannelData->ChatHistory.pop_front();
  }

  for(uint32_t Member : ChannelData->ChatAware){
    Protocol_SessionID_t Target((Protocol_SessionID_t::Type)Member);
    if(Session::IsInvalid(Target) || !ScreenShare_IsMember(Target, ChannelID)){
      continue;
    }
    Session::WriteCommand(Target, 0, Protocol_S2C_t::Channel_Chat_Message, Message);
  }
}

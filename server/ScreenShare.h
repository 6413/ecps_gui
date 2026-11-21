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

/* who has muted their microphone, to one session or to all that know to be told */
void ScreenShare_SendVoiceMuted(Protocol_ChannelID_t ChannelID, const Protocol_SessionID_t *Only = nullptr){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  Protocol_S2C_t::Channel_Voice_Muted_t Message{};
  Message.ChannelID = ChannelID;
  for(uint32_t Member : ChannelData->VoiceMuted){
    if(Message.Count >= 16){
      break;
    }
    Message.Members[Message.Count++] = Protocol_SessionID_t((Protocol_SessionID_t::Type)Member);
  }
  if(Only != nullptr){
    Session::WriteCommand(*Only, 0, Protocol_S2C_t::Channel_Voice_Muted, Message);
    return;
  }
  for(uint32_t Aware : ChannelData->VoiceMutedAware){
    Session::WriteCommand(Protocol_SessionID_t((Protocol_SessionID_t::Type)Aware), 0, Protocol_S2C_t::Channel_Voice_Muted, Message);
  }
}

/* what Channel_Voice_Join says: bit 1 in the voice chat, bit 2 microphone muted, bit 4 wants to be told who is muted */
void ScreenShare_SetVoiceState(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID, uint8_t Flags){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  uint32_t Id = (uint32_t)SessionID.i;
  bool Joined = (Flags & 1) != 0;
  bool Muted = Joined && (Flags & 2) != 0;
  ScreenShare_SetVoice(ChannelID, SessionID, Joined);
  if((Flags & 4) != 0 && std::find(ChannelData->VoiceMutedAware.begin(), ChannelData->VoiceMutedAware.end(), Id) == ChannelData->VoiceMutedAware.end()){
    ChannelData->VoiceMutedAware.push_back(Id);
    ScreenShare_SendVoiceMuted(ChannelID, &SessionID);
  }
  auto It = std::find(ChannelData->VoiceMuted.begin(), ChannelData->VoiceMuted.end(), Id);
  if(Muted && It == ChannelData->VoiceMuted.end()){
    ChannelData->VoiceMuted.push_back(Id);
  }
  else if(!Muted && It != ChannelData->VoiceMuted.end()){
    ChannelData->VoiceMuted.erase(It);
  }
  else{
    return;
  }
  ScreenShare_SendVoiceMuted(ChannelID);
}

void ScreenShare_VoiceQuit(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  uint32_t Id = (uint32_t)SessionID.i;
  /* leaving the channel ends its chat for this session too */
  std::erase(ChannelData->ChatAware, Id);
  std::erase(ChannelData->VoiceAware, Id);
  std::erase(ChannelData->VoiceMutedAware, Id);
  if(std::erase(ChannelData->VoiceMuted, Id) != 0){
    ScreenShare_SendVoiceMuted(ChannelID);
  }
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

/* Packets a viewer asks for that the server never got: they were lost between the sharer and the server. The request
   goes on to the sharer, whose resend the server keeps and relays to the frame's viewers. Several viewers missing the
   same packets ask about the same time, so one frame is asked about at most every 5 ms */
static void ForwardRecoveryToSource(ScreenShare_Stream_t& stream, Protocol_SessionID_t source, Protocol_ChannelID_t channel_id,
  uint16_t sequence, const std::vector<uint16_t>& indices) {
  if (indices.empty()) {
    return;
  }
  uint64_t now = EV_nowi(&g_pile->listener);
  auto& tracker = stream.Frames[sequence % stream.Frames.size()];
  if (tracker.Sequence == sequence) {
    if (now - tracker.LastRequestAt < 5000000ULL || tracker.Requests >= 6) {
      return;
    }
    tracker.LastRequestAt = now;
    ++tracker.Requests;
  }
  // the layout the sharer reads: sequence, count, then the packet numbers
  uint8_t buffer[4 + 512 * sizeof(uint16_t)];
  uint16_t count = (uint16_t)std::min<size_t>(indices.size(), 512);
  std::memcpy(buffer, &sequence, sizeof(uint16_t));
  std::memcpy(buffer + 2, &count, sizeof(uint16_t));
  std::memcpy(buffer + 4, indices.data(), count * sizeof(uint16_t));
  ProtocolUDP::S2C_t::Channel_ScreenShare_RecoveryRequest_t rest;
  rest.ChannelID = channel_id;
  UDP_send(source, 0, ProtocolUDP::S2C_t::Channel_ScreenShare_RecoveryRequest, rest, buffer, 4 + count * sizeof(uint16_t));
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
  auto Session = &g_pile->SessionList[requester];
  if(Session->UDP.Address.ip == 0){
    return;
  }
  auto it = stream_it->second.SentPackets.find(sequence);
  if (it == stream_it->second.SentPackets.end()) {
    // none of that frame reached the server
    std::vector<uint16_t> lacking(missing_indices, missing_indices + missing_count);
    ForwardRecoveryToSource(stream_it->second, source, channel_id, sequence, lacking);
    return;
  }

  auto& packets = it->second;
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
  std::vector<uint16_t> lacking;
  for (uint16_t i = 0; i < missing_count; ++i) {
    uint16_t packet_index = missing_indices[i];
    if (packet_index < packets.size() && !packets[packet_index].empty()) {
      if (Session->UDP.RecoverySent >= MaxResendsPerSecond) {
        continue;
      }
      UDP_send(requester, 0, ProtocolUDP::S2C_t::Channel_ScreenShare_View_StreamData,
              cd, packets[packet_index].data(), packets[packet_index].size());
      ++Session->UDP.RecoverySent;
    }
    else {
      lacking.push_back(packet_index);
    }
  }
  ForwardRecoveryToSource(stream_it->second, source, channel_id, sequence, lacking);
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

/* "[pin #12]..." or "[unpin #12]": the id after the prefix, up to the ] */
static bool ScreenShare_ChatTag(const Protocol_S2C_t::Channel_Chat_Message_t &Message, const char *Prefix, uint32_t &Id){
  size_t PrefixLength = strlen(Prefix);
  if(Message.Length <= PrefixLength || __builtin_memcmp(Message.Text, Prefix, PrefixLength) != 0){
    return false;
  }
  uint64_t Value = 0;
  size_t At = PrefixLength;
  size_t Digits = 0;
  while(At < Message.Length && Message.Text[At] >= '0' && Message.Text[At] <= '9' && Digits < 10){
    Value = Value * 10 + (uint64_t)(Message.Text[At] - '0');
    ++At;
    ++Digits;
  }
  if(Digits == 0 || At >= Message.Length || Message.Text[At] != ']'){
    return false;
  }
  Id = (uint32_t)Value;
  return true;
}

/* a pin keeps the pinned message, an unpin lets it go */
static void ScreenShare_ChatTrackPin(Channel_ScreenShare_Data_t *ChannelData, const Protocol_S2C_t::Channel_Chat_Message_t &Message){
  static constexpr size_t PinnedMax = 25;
  uint32_t Id;
  if(ScreenShare_ChatTag(Message, "[pin #", Id)){
    for(auto &Pinned : ChannelData->ChatPinned){
      if(Pinned.Id == Id){
        return;
      }
    }
    Channel_ScreenShare_Data_t::ChatPinned_t Pinned;
    Pinned.Id = Id;
    Pinned.Pin = Message;
    for(const auto &Old : ChannelData->ChatHistory){
      if(Old.Id == Id){
        Pinned.Original = Old;
        Pinned.HasOriginal = true;
      }
    }
    ChannelData->ChatPinned.push_back(Pinned);
    if(ChannelData->ChatPinned.size() > PinnedMax){
      ChannelData->ChatPinned.erase(ChannelData->ChatPinned.begin());
    }
  }
  else if(ScreenShare_ChatTag(Message, "[unpin #", Id)){
    std::erase_if(ChannelData->ChatPinned, [Id](const Channel_ScreenShare_Data_t::ChatPinned_t &Pinned){
      return Pinned.Id == Id;
    });
  }
}

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
  /* the pinned ones that left the history first, the client puts them in order by id */
  uint32_t HistoryFirst = ChannelData->ChatHistory.empty() ? ChannelData->ChatNextId : ChannelData->ChatHistory.front().Id;
  for(const auto &Pinned : ChannelData->ChatPinned){
    if(Pinned.HasOriginal && Pinned.Original.Id < HistoryFirst){
      Session::WriteCommand(SessionID, 0, Protocol_S2C_t::Channel_Chat_Message, Pinned.Original);
    }
    if(Pinned.Pin.Id < HistoryFirst){
      Session::WriteCommand(SessionID, 0, Protocol_S2C_t::Channel_Chat_Message, Pinned.Pin);
    }
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

  ScreenShare_ChatTrackPin(ChannelData, Message);
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

/* the author edits a message, the author or the channel's host deletes one (Length 0). It is sent on as the same
   message again: a client that has the id replaces the text or, when empty, drops the message. Older clients
   simply keep what they have, as they do with any message they already know */
void ScreenShare_ChatChange(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID, uint32_t MessageId, const char *Text, uint16_t Length){
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type != Protocol::ChannelType_ScreenShare_e){
    return;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(Length > Protocol::ChatTextMax){
    Length = Protocol::ChatTextMax;
  }
  bool IsHost = ChannelData->HostSessionID == SessionID;

  /* the message, from the history or else from what was pinned, and who wrote it */
  Protocol_S2C_t::Channel_Chat_Message_t Found;
  bool Have = false;
  for(const auto &Message : ChannelData->ChatHistory){
    if(Message.Id == MessageId){
      Found = Message;
      Have = true;
      break;
    }
  }
  for(const auto &Pinned : ChannelData->ChatPinned){
    if(Have){
      break;
    }
    if(Pinned.HasOriginal && Pinned.Original.Id == MessageId){
      Found = Pinned.Original;
      Have = true;
    }
    else if(Pinned.Pin.Id == MessageId){
      Found = Pinned.Pin;
      Have = true;
    }
  }
  if(!Have){
    return;
  }
  bool IsAuthor = Found.From == SessionID;
  if(Length == 0 ? !(IsAuthor || IsHost) : !IsAuthor){
    return;
  }

  auto Apply = [&](Protocol_S2C_t::Channel_Chat_Message_t &Message){
    Message.Length = Length;
    __builtin_memset(Message.Text, 0, sizeof(Message.Text));
    __builtin_memcpy(Message.Text, Text, Length);
  };
  if(Length == 0){
    std::erase_if(ChannelData->ChatHistory, [&](const auto &Message){ return Message.Id == MessageId; });
    std::erase_if(ChannelData->ChatPinned, [&](const auto &Pinned){ return Pinned.Pin.Id == MessageId; });
    for(auto &Pinned : ChannelData->ChatPinned){
      if(Pinned.HasOriginal && Pinned.Original.Id == MessageId){
        Pinned.HasOriginal = false;
      }
    }
  }
  else{
    for(auto &Message : ChannelData->ChatHistory){
      if(Message.Id == MessageId){
        Apply(Message);
      }
    }
    for(auto &Pinned : ChannelData->ChatPinned){
      if(Pinned.HasOriginal && Pinned.Original.Id == MessageId){
        Apply(Pinned.Original);
      }
      if(Pinned.Pin.Id == MessageId){
        Apply(Pinned.Pin);
      }
    }
  }

  Apply(Found);
  for(uint32_t Member : ChannelData->ChatAware){
    Protocol_SessionID_t Target((Protocol_SessionID_t::Type)Member);
    if(Session::IsInvalid(Target) || !ScreenShare_IsMember(Target, ChannelID)){
      continue;
    }
    Session::WriteCommand(Target, 0, Protocol_S2C_t::Channel_Chat_Message, Found);
  }
}

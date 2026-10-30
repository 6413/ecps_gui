bool IsChannelInvalid(Protocol_ChannelID_t ID){
  if(g_pile->ChannelList.inri(ID)){
    return true;
  }
  if(g_pile->ChannelList.IsNRSentinel(ID)){
    return true;
  }
  if(g_pile->ChannelList.IsNodeReferenceRecycled(ID)){
    return true;
  }
  return false;
}
bool IsChannelSessionInvalid(Protocol_ChannelID_t ChannelID, Protocol_ChannelSessionID_t ChannelSessionID){
  auto Channel = &g_pile->ChannelList[ChannelID];

  if(Channel->SessionList.inri(ChannelSessionID)){
    return true;
  }
  if(Channel->SessionList.IsNRSentinel(ChannelSessionID)){
    return true;
  }
  if(Channel->SessionList.IsNodeReferenceRecycled(ChannelSessionID)){
    return true;
  }
  return false;
}
bool
IsAnythingInvalid(
  Protocol_SessionID_t SessionID,
  Protocol_ChannelID_t ChannelID,
  Protocol_ChannelSessionID_t ChannelSessionID
){
  if(IsChannelInvalid(ChannelID) == true){
    return true;
  }
  if(IsChannelSessionInvalid(ChannelID, ChannelSessionID) == true){
    return true;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelSession = &Channel->SessionList[ChannelSessionID];
  if(ChannelSession->SessionID != SessionID){
    return true;
  }
  return false;
}

ChannelList_NodeReference_t AddChannel_ScreenShare(Protocol_SessionID_t HostSessionID){
  auto cnr = g_pile->ChannelList.NewNodeLast();
  auto cn = g_pile->ChannelList.GetNodeByReference(cnr);

  cn->data.Type = Protocol::ChannelType_ScreenShare_e;
  cn->data.SessionList.Open();
  cn->data.Buffer = A_resize(0, sizeof(Channel_ScreenShare_Data_t));
  auto data = new (cn->data.Buffer) Channel_ScreenShare_Data_t;
  data->HostSessionID = HostSessionID;

  return cnr;
}

void
Channel_KickSession(
  Protocol_ChannelID_t ChannelID,
  Protocol_ChannelSessionID_t ChannelSessionID,
  Protocol::KickedFromChannel_Reason_t Reason = Protocol::KickedFromChannel_Reason_t::Unknown
){
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelSession = &Channel->SessionList[ChannelSessionID];
  auto Session = &g_pile->SessionList[ChannelSession->SessionID];

  Protocol_S2C_t::KickedFromChannel_t rest;
  rest.ChannelID = ChannelID;
  rest.Reason = Reason;
  TCP_WriteCommand(
    Session->TCP.peer,
    0,
    Protocol_S2C_t::KickedFromChannel,
    rest);

  Session->ChannelList.unlrec(ChannelSession->SessionChannelID);
  Channel->SessionList.unlrec(ChannelSessionID);
}

uint32_t GetChannelUserCount(Protocol_ChannelID_t ChannelID) {
  if (IsChannelInvalid(ChannelID)) return 0;
  
  auto& Channel = g_pile->ChannelList[ChannelID];
  uint32_t count = 0;
  
  auto nr = Channel.SessionList.GetNodeFirst();
  while (nr != Channel.SessionList.dst) {
    auto n = Channel.SessionList.GetNodeByReference(nr);
    count++;
    nr = n->NextNodeReference;
  }
  
  return count;
}

const char* GetChannelName(Protocol_ChannelID_t ChannelID) {
  static char name_buffer[64];
  if (IsChannelInvalid(ChannelID)) return "Invalid";
  
  auto& Channel = g_pile->ChannelList[ChannelID];
  switch(Channel.Type) {
    case Protocol::ChannelType_ScreenShare_e:
      snprintf(name_buffer, sizeof(name_buffer), "Screen Share");
      break;
    default:
      snprintf(name_buffer, sizeof(name_buffer), "Channel #%d");
      break;
  }
  return name_buffer;
}

Protocol_SessionID_t GetChannelHost(Protocol_ChannelID_t ChannelID) {
  if (IsChannelInvalid(ChannelID)) {
    Protocol_SessionID_t invalid;
    invalid = (Protocol_SessionID_t::Type)-1;
    return invalid;
  }
  
  auto& Channel = g_pile->ChannelList[ChannelID];
  switch(Channel.Type) {
    case Protocol::ChannelType_ScreenShare_e: {
      auto ChannelData = (Channel_ScreenShare_Data_t*)Channel.Buffer;
      return ChannelData->HostSessionID;
    }
    default: {
      Protocol_SessionID_t invalid;
      invalid = (Protocol_SessionID_t::Type)-1;
      return invalid;
    }
  }
}

const char* GetSessionUsername(Protocol_SessionID_t SessionID) {
  static char username_buffer[32];
  snprintf(username_buffer, sizeof(username_buffer), "User_%d", (unsigned short)SessionID);
  return username_buffer;
}

void Channel_SendSessionList(NET_TCP_peer_t *peer, uint32_t ID, Protocol_ChannelID_t ChannelID) {
  auto& Channel = g_pile->ChannelList[ChannelID];
  Protocol_SessionID_t host_session = GetChannelHost(ChannelID);

  std::vector<Protocol_S2C_t::SessionInfo_t> infos;
  auto nr = Channel.SessionList.GetNodeFirst();
  while (nr != Channel.SessionList.dst) {
    auto n = Channel.SessionList.GetNodeByReference(nr);

    Protocol_S2C_t::SessionInfo_t info{};
    info.SessionID = n->data.SessionID;
    info.ChannelSessionID = Protocol_ChannelSessionID_t(nr);
    info.AccountID = g_pile->SessionList[n->data.SessionID].AccountID;
    info.IsHost = (n->data.SessionID == host_session) ? 1 : 0;
    info.JoinedAt = 0;
    strncpy((char*)info.Username, GetSessionUsername(n->data.SessionID), sizeof(info.Username) - 1);
    info.Username[sizeof(info.Username) - 1] = '\0';
    infos.push_back(info);

    nr = n->NextNodeReference;
  }

  Protocol_S2C_t::ChannelSessionList_t session_list;
  session_list.ChannelID = ChannelID;
  session_list.SessionCount = (uint16_t)infos.size();
  TCP_WriteCommand(peer, ID, Protocol_S2C_t::ChannelSessionList, session_list);
  if (!infos.empty()) {
    TCP_write_DynamicPointer(peer, infos.data(), infos.size() * sizeof(Protocol_S2C_t::SessionInfo_t));
  }
}

void Channel_SendChannelList(NET_TCP_peer_t *peer, uint32_t ID) {
  std::vector<Protocol_S2C_t::ChannelInfo_t> infos;
  auto nr = g_pile->ChannelList.GetNodeFirst();
  while (nr != g_pile->ChannelList.dst) {
    auto n = g_pile->ChannelList.GetNodeByReference(nr);
    Protocol_ChannelID_t channel_id(nr);

    Protocol_S2C_t::ChannelInfo_t info{};
    info.ChannelID = channel_id;
    info.Type = n->data.Type;
    info.UserCount = GetChannelUserCount(channel_id);
    info.HostSessionID = GetChannelHost(channel_id);
    info.IsPasswordProtected = 0;
    strncpy(info.Name, GetSessionUsername(GetChannelHost(channel_id)), sizeof(info.Name) - 1);
    info.Name[sizeof(info.Name) - 1] = '\0';
    infos.push_back(info);

    nr = n->NextNodeReference;
  }

  Protocol_S2C_t::ChannelList_t channel_list;
  channel_list.ChannelCount = (uint16_t)infos.size();
  TCP_WriteCommand(peer, ID, Protocol_S2C_t::ChannelList, channel_list);
  if (!infos.empty()) {
    TCP_write_DynamicPointer(peer, infos.data(), infos.size() * sizeof(Protocol_S2C_t::ChannelInfo_t));
  }
}

// sessions stay in SessionList after closing, TCP.peer is null for those
template <typename F>
void ForEachConnectedSession(F f) {
  auto nr = g_pile->SessionList.GetNodeFirst();
  while (nr != g_pile->SessionList.dst) {
    auto n = g_pile->SessionList.GetNodeByReference(nr);
    if (n->data.TCP.peer != nullptr && n->data.TCP.Crypto.ready) {
      f(n->data.TCP.peer);
    }
    nr = n->NextNodeReference;
  }
}

void Channel_BroadcastSessionList(Protocol_ChannelID_t ChannelID) {
  ForEachConnectedSession([&](NET_TCP_peer_t *peer) {
    Channel_SendSessionList(peer, 0, ChannelID);
  });
}

void Channel_BroadcastChannelList() {
  ForEachConnectedSession([](NET_TCP_peer_t *peer) {
    Channel_SendChannelList(peer, 0);
  });
}

void NotifyChannelListChanged() {
  auto nr = g_pile->SessionList.GetNodeFirst();
  while (nr != g_pile->SessionList.dst) {
    auto n = g_pile->SessionList.GetNodeByReference(nr);
    
    if (n->data.AccountID != Protocol_AccountID_t::GetInvalid()) {
      // can send refresh notify
    }
    
    nr = n->NextNodeReference;
  }
}

void NotifyChannelSessionListChanged(Protocol_ChannelID_t ChannelID) {
  if (IsChannelInvalid(ChannelID)) return;
  
  auto& Channel = g_pile->ChannelList[ChannelID];
  
  auto nr = Channel.SessionList.GetNodeFirst();
  while (nr != Channel.SessionList.dst) {
    auto n = Channel.SessionList.GetNodeByReference(nr);
    
    
    nr = n->NextNodeReference;
  }
}
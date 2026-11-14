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
      snprintf(name_buffer, sizeof(name_buffer), "Channel #%u", (unsigned)ChannelID.i);
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

// A session's name: the one it picked, or User_ and six hex digits made from its IP address. The digits are an
// HKDF of the address with the server's private key as the salt: the same address gets the same name on this
// server every time, and the name can't be turned back into the address (without the key there is nothing to try)
const char* GetSessionUsername(Protocol_SessionID_t SessionID) {
  /* a channel can have no host session, and ids of closed sessions are recycled */
  if (g_pile->SessionList.inri(SessionID) ||
    g_pile->SessionList.IsNRSentinel(SessionID) ||
    g_pile->SessionList.IsNodeReferenceRecycled(SessionID)) {
    return "";
  }
  return g_pile->SessionList[SessionID].Username;
}

bool Username_Equal(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
    if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
    if (x != y) return false;
    if (!x) return true;
  }
}

/* names are compared without case, and the default names of other sessions count */
bool Username_IsTaken(const char* Name, Protocol_SessionID_t Except) {
  auto nr = g_pile->SessionList.GetNodeFirst();
  while (nr != g_pile->SessionList.dst) {
    auto n = g_pile->SessionList.GetNodeByReference(nr);
    Protocol_SessionID_t Other(nr);
    if (Other != Except && n->data.Username[0] && Username_Equal(n->data.Username, Name)) {
      return true;
    }
    nr = n->NextNodeReference;
  }
  return false;
}

/* 1 to 24 of letters, digits, space, _ . -, no space at either end. The client checks the same */
bool Username_IsValid(const char* Name) {
  size_t length = strlen(Name);
  if (length < 1 || length > 24 || Name[0] == ' ' || Name[length - 1] == ' ') {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = Name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

void Username_AssignDefault(Protocol_SessionID_t SessionID, uint32_t Ip) {
  auto Session = &g_pile->SessionList[SessionID];
  Session->Username[0] = 0;

  uint8_t IpBytes[4];
  __builtin_memcpy(IpBytes, &Ip, sizeof(IpBytes));
  uint8_t Derived[3];
  char Base[16];
  if (fan::network::crypto::hkdf(
    IpBytes, sizeof(IpBytes),
    g_pile->StaticPrivate.data(), g_pile->StaticPrivate.size(),
    "ecps default username v1", Derived, sizeof(Derived))) {
    snprintf(Base, sizeof(Base), "User_%02x%02x%02x", Derived[0], Derived[1], Derived[2]);
  }
  else {
    snprintf(Base, sizeof(Base), "User_%u", (unsigned)(unsigned short)SessionID);
  }

  /* two clients behind one address would get the same digits */
  char Name[32];
  snprintf(Name, sizeof(Name), "%s", Base);
  for (int Copy = 2; Username_IsTaken(Name, SessionID) && Copy < 1000; ++Copy) {
    snprintf(Name, sizeof(Name), "%s_%d", Base, Copy);
  }
  snprintf(Session->Username, sizeof(Session->Username), "%s", Name);
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
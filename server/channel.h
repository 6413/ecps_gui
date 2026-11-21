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

/* HKDF with the server's private key as the salt: a stolen hash can't be tried against guesses without the key */
bool Channel_HashPassword(const char* Password, std::array<uint8_t, 32>& Out){
  return fan::network::crypto::hkdf(
    (const uint8_t*)Password, std::strlen(Password),
    g_pile->StaticPrivate.data(), g_pile->StaticPrivate.size(),
    "ecps channel password",
    Out.data(), Out.size());
}
bool Channel_HasPassword(Protocol_ChannelID_t ChannelID){
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type != Protocol::ChannelType_ScreenShare_e){
    return false;
  }
  return ((Channel_ScreenShare_Data_t *)Channel->Buffer)->HasPassword;
}
/* true when the channel has no password or this is it. Takes the same time whatever the password is */
bool Channel_CheckPassword(Protocol_ChannelID_t ChannelID, const char* Password){
  if(!Channel_HasPassword(ChannelID)){
    return true;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  std::array<uint8_t, 32> Given;
  if(!Channel_HashPassword(Password, Given)){
    return false;
  }
  uint8_t Difference = 0;
  for(size_t i = 0; i < Given.size(); i++){
    Difference |= Given[i] ^ ChannelData->PasswordHash[i];
  }
  return Difference == 0;
}

const char* GetSessionUsername(Protocol_SessionID_t SessionID);
bool Username_Equal(const char* a, const char* b);

ChannelList_NodeReference_t AddChannel_ScreenShare(Protocol_SessionID_t HostSessionID){
  auto cnr = g_pile->ChannelList.NewNodeLast();
  auto cn = g_pile->ChannelList.GetNodeByReference(cnr);

  cn->data.Type = Protocol::ChannelType_ScreenShare_e;
  cn->data.SessionList.Open();
  cn->data.Buffer = A_resize(0, sizeof(Channel_ScreenShare_Data_t));
  auto data = new (cn->data.Buffer) Channel_ScreenShare_Data_t;
  data->HostSessionID = HostSessionID;
  snprintf(data->OwnerName, sizeof(data->OwnerName), "%s", g_pile->SessionList[HostSessionID].Username);

  return cnr;
}

/* the host left or closed: the channel stays, without a host, for the owner to come back to */
void Channel_DropHost(Protocol_ChannelID_t ChannelID){
  auto ChannelData = (Channel_ScreenShare_Data_t *)g_pile->ChannelList[ChannelID].Buffer;
  ChannelData->HostSessionID = (Protocol_SessionID_t::Type)-1;
  /* control of the host's mouse, keyboard and MIDI was the host's to give: it ends with the host leaving */
  ChannelData->Flag = 0;
}

/* the owner joining a channel without a host is its host again */
void Channel_ReclaimHost(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID){
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type != Protocol::ChannelType_ScreenShare_e){
    return;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  const char *Name = g_pile->SessionList[SessionID].Username;
  if(GetSessionUsername(ChannelData->HostSessionID)[0] == 0 && Name[0] && Username_Equal(Name, ChannelData->OwnerName)){
    ChannelData->HostSessionID = SessionID;
  }
}

/* the name a channel shows: its host's, which follows renames, or the owner's while the host is away */
const char* Channel_ShownName(Protocol_ChannelID_t ChannelID){
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type != Protocol::ChannelType_ScreenShare_e){
    return "";
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(ChannelData->Title[0]){
    return ChannelData->Title;
  }
  const char *HostName = GetSessionUsername(ChannelData->HostSessionID);
  if(HostName[0]){
    snprintf(ChannelData->OwnerName, sizeof(ChannelData->OwnerName), "%s", HostName);
  }
  return ChannelData->OwnerName;
}

/* an owner's channel waiting without a host and without a password, for "Add new" to give back instead of a second one */
bool Channel_FindOwnedEmpty(Protocol_SessionID_t SessionID, Protocol_ChannelID_t *Out){
  const char *Name = g_pile->SessionList[SessionID].Username;
  if(!Name[0]){
    return false;
  }
  auto nr = g_pile->ChannelList.GetNodeFirst();
  while(nr != g_pile->ChannelList.dst){
    auto n = g_pile->ChannelList.GetNodeByReference(nr);
    Protocol_ChannelID_t ChannelID(nr);
    if(n->data.Type == Protocol::ChannelType_ScreenShare_e){
      auto ChannelData = (Channel_ScreenShare_Data_t *)n->data.Buffer;
      if(GetSessionUsername(ChannelData->HostSessionID)[0] == 0 && !ChannelData->HasPassword &&
        Username_Equal(Name, ChannelData->OwnerName)){
        *Out = ChannelID;
        return true;
      }
    }
    nr = n->NextNodeReference;
  }
  return false;
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

void Channel_BroadcastChannelList();

/* the host deletes its channel: everyone in it is sent out, told it was closed, and the channel is gone, with its
   chat and password. Its id can be handed out again, ids held by the clients are rejected as recycled */
void Channel_Delete(Protocol_ChannelID_t ChannelID){
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto nr = Channel->SessionList.GetNodeFirst();
  while(nr != Channel->SessionList.dst){
    auto n = Channel->SessionList.GetNodeByReference(nr);
    auto _nr = n->NextNodeReference;
    auto Session = &g_pile->SessionList[n->data.SessionID];
    if(Session->Watching && Session->WatchChannel == ChannelID.i){
      Session->Watching = 0;
    }
    Channel_KickSession(ChannelID, nr, Protocol::KickedFromChannel_Reason_t::ChannelIsClosed);
    nr = _nr;
  }
  if(Channel->Type == Protocol::ChannelType_ScreenShare_e){
    ((Channel_ScreenShare_Data_t *)Channel->Buffer)->~Channel_ScreenShare_Data_t();
  }
  A_resize(Channel->Buffer, 0);
  Channel->Buffer = nullptr;
  Channel->SessionList.Close();
  g_pile->ChannelList.unlrec(ChannelID);
  Channel_BroadcastChannelList();
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

/* who is in a channel with a password is told only to those in it: to the others the list is empty */
bool Channel_IsInChannel(Protocol_ChannelID_t ChannelID, Protocol_SessionID_t SessionID) {
  auto& Channel = g_pile->ChannelList[ChannelID];
  auto nr = Channel.SessionList.GetNodeFirst();
  while (nr != Channel.SessionList.dst) {
    auto n = Channel.SessionList.GetNodeByReference(nr);
    if (n->data.SessionID == SessionID) {
      return true;
    }
    nr = n->NextNodeReference;
  }
  return false;
}

void Channel_SendSessionList(NET_TCP_peer_t *peer, uint32_t ID, Protocol_ChannelID_t ChannelID, Protocol_SessionID_t Viewer) {
  auto& Channel = g_pile->ChannelList[ChannelID];
  Protocol_SessionID_t host_session = GetChannelHost(ChannelID);
  bool hidden = Channel_HasPassword(ChannelID) && !Channel_IsInChannel(ChannelID, Viewer);

  std::vector<Protocol_S2C_t::SessionInfo_t> infos;
  auto nr = hidden ? Channel.SessionList.dst : Channel.SessionList.GetNodeFirst();
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
    info.IsPasswordProtected = Channel_HasPassword(channel_id) ? 1 : 0;
    strncpy(info.Name, Channel_ShownName(channel_id), sizeof(info.Name) - 1);
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
      f(n->data.TCP.peer, Protocol_SessionID_t(nr));
    }
    nr = n->NextNodeReference;
  }
}

void Channel_BroadcastSessionList(Protocol_ChannelID_t ChannelID) {
  ForEachConnectedSession([&](NET_TCP_peer_t *peer, Protocol_SessionID_t Viewer) {
    Channel_SendSessionList(peer, 0, ChannelID, Viewer);
  });
}

void Channel_BroadcastChannelList() {
  ForEachConnectedSession([](NET_TCP_peer_t *peer, Protocol_SessionID_t) {
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
// File transfers are end-to-end encrypted between the two clients, the server never sees the
// key. It only checks that both sides share a channel and forwards the message with Peer
// rewritten from the receiver (C2S) to the sender (S2C).

bool FileTransfer_CanRelay(
  Protocol_SessionID_t From,
  Protocol_ChannelID_t ChannelID,
  Protocol_SessionID_t To
){
  if(From == To){
    return false;
  }
  if(IsChannelInvalid(ChannelID) == true){
    return false;
  }
  if(Session::IsInvalid(To) == true){
    return false;
  }
  auto ToSession = &g_pile->SessionList[To];
  if(ToSession->TCP.peer == nullptr || !ToSession->TCP.Crypto.ready){
    return false;
  }

  bool FromInChannel = false;
  bool ToInChannel = false;
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto nr = Channel->SessionList.GetNodeFirst();
  while(nr != Channel->SessionList.dst){
    auto n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == From){
      FromInChannel = true;
    }
    if(n->data.SessionID == To){
      ToInChannel = true;
    }
    nr = n->NextNodeReference;
  }
  return FromInChannel && ToInChannel;
}

template <typename S2C_t, typename C2S_t>
void FileTransfer_Relay(
  Protocol_SessionID_t From,
  Protocol_CI_t Command,
  const C2S_t *Request
){
  static_assert(sizeof(S2C_t) == sizeof(C2S_t));
  Protocol_SessionID_t To = Request->Peer;
  if(FileTransfer_CanRelay(From, Request->ChannelID, To) == false){
    return;
  }
  S2C_t Payload;
  __builtin_memcpy(&Payload, Request, sizeof(Payload));
  Payload.Peer = From;
  Session::WriteCommand(To, 0, Command, Payload);
}

case Protocol_C2S_t::KeepAlive:{
  TCP_WriteCommand(
    peer,
    BasePacket->ID,
    Protocol_S2C_t::KeepAlive);

  goto StateDone_gt;
}
case Protocol_C2S_t::KeyExchange:{
  auto Request = (Protocol_C2S_t::KeyExchange_t *)RestPacket;

  if(Session->TCP.Crypto.ready){
    goto StateDone_gt;
  }

  fan::network::crypto::x25519_key_t ClientPublic, ServerEphemeral;
  fan::network::crypto::session_keys_t Keys;
  __builtin_memcpy(ClientPublic.data(), Request->ClientPublic, ClientPublic.size());
  if(!fan::network::crypto::server_handshake(g_pile->StaticPrivate, g_pile->StaticPublic, ClientPublic, Protocol::CryptoContext, ServerEphemeral, Keys)){
    goto StateDone_gt;
  }

  Protocol_S2C_t::KeyExchange_t rest;
  __builtin_memcpy(rest.ServerEphemeral, ServerEphemeral.data(), ServerEphemeral.size());
  __builtin_memcpy(rest.ServerStatic, g_pile->StaticPublic.data(), g_pile->StaticPublic.size());
  __builtin_memcpy(rest.Confirm, Keys.confirm.data(), Keys.confirm.size());
  TCP_WriteCommand(
    peer,
    BasePacket->ID,
    Protocol_S2C_t::KeyExchange,
    rest);

  Session->UDP.Crypto.set_server(Keys);
  Session->TCP.Crypto.set(Keys.tcp_s2c, Keys.tcp_c2s);
  fan::network::crypto::cleanse(&Keys, sizeof(Keys));

  goto StateDone_gt;
}
case Protocol_C2S_t::Request_Login:{
  auto Request = (Protocol_C2S_t::Request_Login_t *)RestPacket;

  if(Session->AccountID != Protocol_AccountID_t::GetInvalid()){
    /* double login */
    goto StateDone_gt;
  }

  switch(Request->Type){
    case Protocol::LoginType_t::Anonymous:{
      // todo give account
      break;
    }
    default:{
      goto StateDone_gt;
    }
  }

  Protocol_S2C_t::Response_Login_t rest;
  rest.AccountID = Session->AccountID;
  rest.SessionID = SessionID;

  TCP_WriteCommand(
    peer,
    BasePacket->ID,
    Protocol_S2C_t::Response_Login,
    rest);

  {
    /* the client has no other way to know the name it was given */
    Protocol_S2C_t::SetUsername_Result_t NameInfo{};
    NameInfo.Result = (uint8_t)Protocol::SetUsername_Result_t::Ok;
    snprintf(NameInfo.Name, sizeof(NameInfo.Name), "%s", Session->Username);
    TCP_WriteCommand(peer, 0, Protocol_S2C_t::SetUsername_Result, NameInfo);
  }

  goto StateDone_gt;
}
case Protocol_C2S_t::SetUsername:{
  auto Request = (Protocol_C2S_t::SetUsername_t *)RestPacket;
  /* the end of a name from the wire is never trusted */
  Request->Name[sizeof(Request->Name) - 1] = 0;

  Protocol_S2C_t::SetUsername_Result_t Result{};
  uint64_t Now = EV_nowi(&g_pile->listener);
  bool Changed = false;

  if(Session->UsernameChangedAt != 0 && Now - Session->UsernameChangedAt < 500000000ULL){
    Result.Result = (uint8_t)Protocol::SetUsername_Result_t::TooFast;
  }
  else if(Request->Name[0] == 0){
    Username_AssignDefault(SessionID, peer->sdstaddr.ip);
    Result.Result = (uint8_t)Protocol::SetUsername_Result_t::Ok;
    Changed = true;
  }
  else if(!Username_IsValid(Request->Name)){
    Result.Result = (uint8_t)Protocol::SetUsername_Result_t::Invalid;
  }
  else if(Username_IsTaken(Request->Name, SessionID)){
    Result.Result = (uint8_t)Protocol::SetUsername_Result_t::Taken;
  }
  else{
    snprintf(Session->Username, sizeof(Session->Username), "%s", Request->Name);
    Result.Result = (uint8_t)Protocol::SetUsername_Result_t::Ok;
    Changed = true;
  }

  if(Changed){
    Session->UsernameChangedAt = Now;
  }
  snprintf(Result.Name, sizeof(Result.Name), "%s", Session->Username);
  TCP_WriteCommand(peer, BasePacket->ID, Protocol_S2C_t::SetUsername_Result, Result);

  if(Changed){
    /* everyone sees names in the user lists and as the channel names */
    auto nr = Session->ChannelList.GetNodeFirst();
    while(nr != Session->ChannelList.dst){
      auto n = Session->ChannelList.GetNodeByReference(nr);
      Channel_BroadcastSessionList(n->data.ChannelID);
      nr = n->NextNodeReference;
    }
    Channel_BroadcastChannelList();
  }

  goto StateDone_gt;
}
case Protocol_C2S_t::CreateChannel:{
  auto Request = (Protocol_C2S_t::CreateChannel_t *)RestPacket;
  if(Request->Type >= Protocol::ChannelType_Amount){
    Protocol_S2C_t::JoinChannel_Error_t rest;
    rest.Reason = Protocol::JoinChannel_Error_Reason_t::InvalidChannelType;
    TCP_WriteCommand(
      peer,
      BasePacket->ID,
      Protocol_S2C_t::CreateChannel_Error,
      rest);
    goto StateDone_gt;
  }

  Protocol_ChannelID_t ChannelID;
  Protocol_ChannelSessionID_t ChannelSessionID;
  switch(Request->Type){
    case Protocol::ChannelType_ScreenShare_e:{
      if(Channel_FindOwnedEmpty(SessionID, &ChannelID) && !ScreenShare_IsMember(SessionID, ChannelID)){
        Channel_ReclaimHost(ChannelID, SessionID);
      }
      else{
        ChannelID = AddChannel_ScreenShare(SessionID);
      }

      Protocol_CI_t CI = ScreenShare_AddPeer(
        ChannelID,
        SessionID,
        &ChannelSessionID);
      if(CI != Protocol_S2C_t::JoinChannel_OK){
        /* why */
        __abort();
      }

      break;
    }
    default:{
      __abort();
    }
  }

  {
    Protocol_S2C_t::JoinChannel_OK_t jc;
    jc.Type = Protocol::ChannelType_ScreenShare_e;
    jc.ChannelID = ChannelID;
    jc.ChannelSessionID = ChannelSessionID;

    TCP_WriteCommand(
      peer,
      BasePacket->ID,
      Protocol_S2C_t::CreateChannel_OK,
      jc);
  }

  Channel_BroadcastSessionList(ChannelID);
  Channel_BroadcastChannelList();

  goto StateDone_gt;
}
case Protocol_C2S_t::JoinChannelPassword:
case Protocol_C2S_t::JoinChannel:{

  /* JoinChannelPassword begins with the same ChannelID */
  auto Request = (Protocol_C2S_t::JoinChannel_t *)RestPacket;
  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  char Password[Protocol::ChannelPasswordMax] = {};
  if(BasePacket->Command == Protocol_C2S_t::JoinChannelPassword){
    auto WithPassword = (Protocol_C2S_t::JoinChannelPassword_t *)RestPacket;
    __builtin_memcpy(Password, WithPassword->Password, sizeof(Password) - 1);
  }

  if(IsChannelInvalid(ChannelID) == true){
    Protocol_S2C_t::JoinChannel_Error_t rest;
    rest.Reason = Protocol::JoinChannel_Error_Reason_t::InvalidChannelID;
    TCP_WriteCommand(
      peer,
      BasePacket->ID,
      Protocol_S2C_t::JoinChannel_Error,
      rest);
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];

  if(Channel_HasPassword(ChannelID) && GetChannelHost(ChannelID) != SessionID && !Channel_CheckPassword(ChannelID, Password)){
    Protocol_S2C_t::JoinChannel_Error_t rest;
    rest.Reason = Password[0] == 0
      ? Protocol::JoinChannel_Error_Reason_t::PasswordRequired
      : Protocol::JoinChannel_Error_Reason_t::WrongPassword;
    TCP_WriteCommand(
      peer,
      BasePacket->ID,
      Protocol_S2C_t::JoinChannel_Error,
      rest);
    goto StateDone_gt;
  }

  Protocol_ChannelSessionID_t ChannelSessionID;
  switch(Channel->Type){
    case Protocol::ChannelType_ScreenShare_e:{
      Protocol_CI_t CI = ScreenShare_AddPeer(
        ChannelID,
        SessionID,
        &ChannelSessionID);
      if(CI != Protocol_S2C_t::JoinChannel_OK){
        TCP_WriteCommand(peer, BasePacket->ID, CI);
        goto StateDone_gt;
      }
      Channel_ReclaimHost(ChannelID, SessionID);

      break;
    }
  }

  {
    Protocol_S2C_t::JoinChannel_OK_t jc;
    jc.Type = Protocol::ChannelType_ScreenShare_e;
    jc.ChannelID = ChannelID;
    jc.ChannelSessionID = ChannelSessionID;
    TCP_WriteCommand(
      peer,
      BasePacket->ID,
      Protocol_S2C_t::JoinChannel_OK,
      jc);
  }

  Channel_BroadcastSessionList(ChannelID);
  Channel_BroadcastChannelList();
  ScreenShare_SendFlagTo(ChannelID, {ChannelID, ChannelSessionID});
  ScreenShare_SendStreamersTo(ChannelID, SessionID);

  goto StateDone_gt;
}
case Protocol_C2S_t::SetChannelPassword:{
  auto Request = (Protocol_C2S_t::SetChannelPassword_t *)RestPacket;
  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type != Protocol::ChannelType_ScreenShare_e){
    goto StateDone_gt;
  }
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  /* only the host decides who comes in */
  if(ChannelData->HostSessionID != SessionID){
    goto StateDone_gt;
  }
  char Password[Protocol::ChannelPasswordMax] = {};
  __builtin_memcpy(Password, Request->Password, sizeof(Password) - 1);
  if(Password[0] == 0){
    ChannelData->HasPassword = false;
    ChannelData->PasswordHash.fill(0);
  }
  else if(Channel_HashPassword(Password, ChannelData->PasswordHash)){
    ChannelData->HasPassword = true;
  }
  __builtin_memset(Password, 0, sizeof(Password));
  Channel_BroadcastChannelList();
  /* with a password set the ones outside stop seeing who is in it, removed they see it again */
  Channel_BroadcastSessionList(ChannelID);
  goto StateDone_gt;
}
case Protocol_C2S_t::QuitChannel:{
  auto Request = (Protocol_C2S_t::QuitChannel_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  Protocol_ChannelSessionID_t ChannelSessionID = Request->ChannelSessionID;
  if(IsAnythingInvalid(SessionID, ChannelID, ChannelSessionID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  if(Channel->Type == Protocol::ChannelType_ScreenShare_e){
    auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
    if(ChannelData->HostSessionID == SessionID){
      /* the host leaving keeps the channel for the others and for the host's return: only its stream and voice stop */
      Channel_DropHost(ChannelID);
    }
    ScreenShare_SetStreamer(ChannelID, SessionID, false, false);
  }
  if(Session->Watching && Session->WatchChannel == ChannelID.i){
    Session->Watching = 0;
  }
  {
    auto ChannelSession = &Channel->SessionList[ChannelSessionID];
    Session->ChannelList.unlrec(ChannelSession->SessionChannelID);
    Channel->SessionList.unlrec(ChannelSessionID);
  }
  Channel_BroadcastSessionList(ChannelID);
  Channel_BroadcastChannelList();

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(ChannelData->HostSessionID != SessionID){
    goto StateDone_gt;
  }
  ChannelData->Flag = Request->Flag;
  ScreenShare_FlagIsChanged(ChannelID);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMouseCoordinate:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMouseCoordinate_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(ChannelData->HostSessionID != SessionID){
    goto StateDone_gt;
  }

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == SessionID){
      /* lets dont send to self */
      continue;
    }

    Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewMouseCoordinate_t Payload;
    Payload.ChannelID = ChannelID;
    Payload.pos = Request->pos;

    Session::WriteCommand(
      n->data.SessionID,
      0,
      Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewMouseCoordinate,
      Payload);
  }

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseCoordinate:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseCoordinate_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(!ScreenShare_CanControlHost(ChannelID, SessionID)){
    goto StateDone_gt;
  }

  Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseCoordinate_t Payload;
  Payload.ChannelID = ChannelID;
  Payload.pos = Request->pos;

  Session::WriteCommand(
    ChannelData->HostSessionID,
    0,
    Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseCoordinate,
    Payload);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseMotion:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseMotion_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(!ScreenShare_CanControlHost(ChannelID, SessionID)){
    goto StateDone_gt;
  }

  Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseMotion_t Payload;
  Payload.ChannelID = ChannelID;
  Payload.Motion = Request->Motion;

  Session::WriteCommand(
    ChannelData->HostSessionID,
    0,
    Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseMotion,
    Payload);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseButton:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMouseButton_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(!ScreenShare_CanControlHost(ChannelID, SessionID)){
    goto StateDone_gt;
  }

  Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseButton_t Payload;
  Payload.ChannelID = ChannelID;
  Payload.key = Request->key;
  Payload.state = Request->state;
  Payload.pos = Request->pos;

  Session::WriteCommand(
    ChannelData->HostSessionID,
    0,
    Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseButton,
    Payload);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostKeyboard:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostKeyboard_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(!ScreenShare_CanControlHost(ChannelID, SessionID)){
    goto StateDone_gt;
  }

  Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostKeyboard_t Payload;
  Payload.ChannelID = ChannelID;
  Payload.Scancode = Request->Scancode;
  Payload.State = Request->State;

  Session::WriteCommand(
    ChannelData->HostSessionID,
    0,
    Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostKeyboard,
    Payload);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMidi:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_View_ApplyToHostMidi_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(!ScreenShare_CanSendMidiToHost(ChannelID, SessionID)){
    goto StateDone_gt;
  }
  // only channel voice messages, no sysex or system realtime from a viewer
  if(Request->Status < 0x80 || Request->Status >= 0xF0 || (Request->Data1 & 0x80) || (Request->Data2 & 0x80)){
    goto StateDone_gt;
  }

  Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMidi_t Payload;
  Payload.ChannelID = ChannelID;
  Payload.Status = Request->Status;
  Payload.Data1 = Request->Data1;
  Payload.Data2 = Request->Data2;

  Session::WriteCommand(
    ChannelData->HostSessionID,
    0,
    Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMidi,
    Payload);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMidi:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewMidi_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  if(ChannelData->HostSessionID != SessionID){
    goto StateDone_gt;
  }
  // only channel voice messages
  if(Request->Status < 0x80 || Request->Status >= 0xF0 || (Request->Data1 & 0x80) || (Request->Data2 & 0x80)){
    goto StateDone_gt;
  }

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == SessionID){
      continue;
    }

    Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewMidi_t Payload;
    Payload.ChannelID = ChannelID;
    Payload.Status = Request->Status;
    Payload.Data1 = Request->Data1;
    Payload.Data2 = Request->Data2;

    Session::WriteCommand(
      n->data.SessionID,
      0,
      Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewMidi,
      Payload);
  }

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_Log_Request:{
  auto Request = (Protocol_C2S_t::Channel_Log_Request_t *)RestPacket;

  FileTransfer_Relay<Protocol_S2C_t::Channel_Log_Request_t>(
    SessionID,
    Protocol_S2C_t::Channel_Log_Request,
    Request);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_Log_Status:{
  auto Request = (Protocol_C2S_t::Channel_Log_Status_t *)RestPacket;

  FileTransfer_Relay<Protocol_S2C_t::Channel_Log_Status_t>(
    SessionID,
    Protocol_S2C_t::Channel_Log_Status,
    Request);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_Log_Data:{
  auto Request = (Protocol_C2S_t::Channel_Log_Data_t *)RestPacket;

  FileTransfer_Relay<Protocol_S2C_t::Channel_Log_Data_t>(
    SessionID,
    Protocol_S2C_t::Channel_Log_Data,
    Request);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_Share_StreamState:{
  auto Request = (Protocol_C2S_t::Channel_ScreenShare_Share_StreamState_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsAnythingInvalid(SessionID, ChannelID, Request->ChannelSessionID) == true){
    goto StateDone_gt;
  }
  ScreenShare_SetStreamer(ChannelID, SessionID, Request->Streaming != 0, true);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_Voice_Join:{
  auto Request = (Protocol_C2S_t::Channel_Voice_Join_t *)RestPacket;

  if(IsAnythingInvalid(SessionID, Request->ChannelID, Request->ChannelSessionID) == true){
    goto StateDone_gt;
  }
  ScreenShare_SetVoice(Request->ChannelID, SessionID, Request->Joined != 0);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_View_Watch:{
  auto Request = (Protocol_C2S_t::Channel_ScreenShare_View_Watch_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsAnythingInvalid(SessionID, ChannelID, Request->ChannelSessionID) == true){
    goto StateDone_gt;
  }
  if(Request->HasSource == 0){
    Session->Watching = 0;
    goto StateDone_gt;
  }
  /* only someone in the channel can be watched */
  if(Session::IsInvalid(Request->Source) == true || ScreenShare_IsMember(Request->Source, ChannelID) == false){
    goto StateDone_gt;
  }
  Session->Watching = 1;
  Session->WatchChannel = ChannelID.i;
  Session->WatchSource = Request->Source.i;

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewCursor:{

  auto Request = (Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewCursor_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsAnythingInvalid(SessionID, ChannelID, Request->ChannelSessionID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == SessionID){
      continue;
    }
    /* only the viewers that watch this session, the others would draw it over somebody else's picture */
    if(ScreenShare_WatchedSource(n->data.SessionID, ChannelID) != SessionID){
      continue;
    }

    Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewCursor_t Payload;
    Payload.ChannelID = ChannelID;
    Payload.X = Request->X;
    Payload.Y = Request->Y;
    Payload.Visible = Request->Visible ? 1 : 0;

    Session::WriteCommand(
      n->data.SessionID,
      0,
      Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewCursor,
      Payload);
  }

  goto StateDone_gt;
}
case Protocol_C2S_t::RequestChannelList: {
  Channel_SendChannelList(peer, BasePacket->ID);
  
  goto StateDone_gt;
}

case Protocol_C2S_t::RequestChannelSessionList: {
  auto Request = (Protocol_C2S_t::RequestChannelSessionList_t*)RestPacket;
  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  
  if (IsChannelInvalid(ChannelID)) {
    // Send empty list for invalid channels
    Protocol_S2C_t::ChannelSessionList_t empty_list;
    empty_list.ChannelID = ChannelID;
    empty_list.SessionCount = 0;
    
    TCP_WriteCommand(
      peer,
      BasePacket->ID,
      Protocol_S2C_t::ChannelSessionList,
      empty_list
    );
    
    #if set_Verbose
      //_print("[RequestChannelSessionList] Invalid channel %d requested by session %d\n", (int)ChannelID, (int)SessionID);
    #endif
    
    goto StateDone_gt;
  }
  
  Channel_SendSessionList(peer, BasePacket->ID, ChannelID, SessionID);
  
  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_ViewToShare:{
  auto Request = (Protocol_C2S_t::Channel_ScreenShare_ViewToShare_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  if(ScreenShare_IsMember(SessionID, ChannelID) == false){
    goto StateDone_gt;
  }

  /* from the viewer to the session it watches (the host unless it picked someone else) */
  Protocol_SessionID_t Target = ScreenShare_WatchedSource(SessionID, ChannelID);
  if(Session::IsInvalid(Target) == true || Target == SessionID){
    goto StateDone_gt;
  }
  Protocol_S2C_t::Channel_ScreenShare_ViewToShare_t Payload;
  Payload.ChannelID = ChannelID;
  Payload.Flag = Request->Flag;

  Session::WriteCommand(
    Target,
    0,
    Protocol_S2C_t::Channel_ScreenShare_ViewToShare,
    Payload);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_ShareToView:{
  auto Request = (Protocol_C2S_t::Channel_ScreenShare_ShareToView_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  
  if(ChannelData->HostSessionID != SessionID){
    goto StateDone_gt;
  }

  auto nr = Channel->SessionList.GetNodeFirst();
  ChannelSessionList_Node_t *n;
  for(; nr != Channel->SessionList.dst; nr = n->NextNodeReference){
    n = Channel->SessionList.GetNodeByReference(nr);
    if(n->data.SessionID == SessionID){
      /* don't send to self (host) */
      continue;
    }

    Protocol_S2C_t::Channel_ScreenShare_ShareToView_t Payload;
    Payload.ChannelID = ChannelID;
    Payload.Flag = Request->Flag;

    Session::WriteCommand(
      n->data.SessionID,
      0,
      Protocol_S2C_t::Channel_ScreenShare_ShareToView,
      Payload);
  }

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_File_Offer:{
  auto Request = (Protocol_C2S_t::Channel_File_Offer_t *)RestPacket;

  /* the receiver prints this name, never trust it to be terminated */
  Request->Name[sizeof(Request->Name) - 1] = 0;
  FileTransfer_Relay<Protocol_S2C_t::Channel_File_Offer_t>(
    SessionID,
    Protocol_S2C_t::Channel_File_Offer,
    Request);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_File_Response:{
  auto Request = (Protocol_C2S_t::Channel_File_Response_t *)RestPacket;

  FileTransfer_Relay<Protocol_S2C_t::Channel_File_Response_t>(
    SessionID,
    Protocol_S2C_t::Channel_File_Response,
    Request);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_File_Chunk:{
  auto Request = (Protocol_C2S_t::Channel_File_Chunk_t *)RestPacket;

  FileTransfer_Relay<Protocol_S2C_t::Channel_File_Chunk_t>(
    SessionID,
    Protocol_S2C_t::Channel_File_Chunk,
    Request);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_File_Ack:{
  auto Request = (Protocol_C2S_t::Channel_File_Ack_t *)RestPacket;

  FileTransfer_Relay<Protocol_S2C_t::Channel_File_Ack_t>(
    SessionID,
    Protocol_S2C_t::Channel_File_Ack,
    Request);

  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_File_Cancel:{
  auto Request = (Protocol_C2S_t::Channel_File_Cancel_t *)RestPacket;

  FileTransfer_Relay<Protocol_S2C_t::Channel_File_Cancel_t>(
    SessionID,
    Protocol_S2C_t::Channel_File_Cancel,
    Request);

  goto StateDone_gt;
}

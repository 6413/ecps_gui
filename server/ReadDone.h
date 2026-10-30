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
      ChannelID = AddChannel_ScreenShare(SessionID);

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
case Protocol_C2S_t::JoinChannel:{

  auto Request = (Protocol_C2S_t::JoinChannel_t *)RestPacket;
  Protocol_ChannelID_t ChannelID = Request->ChannelID;

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

  goto StateDone_gt;
}
case Protocol_C2S_t::QuitChannel:{
  /* not implemented yet */
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
  if(!(ChannelData->Flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
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
  if(!(ChannelData->Flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
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
  if(!(ChannelData->Flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
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
  if(!(ChannelData->Flag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
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
  
  Channel_SendSessionList(peer, BasePacket->ID, ChannelID);
  
  goto StateDone_gt;
}
case Protocol_C2S_t::Channel_ScreenShare_ViewToShare:{
  auto Request = (Protocol_C2S_t::Channel_ScreenShare_ViewToShare_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  if(IsChannelInvalid(ChannelID) == true){
    goto StateDone_gt;
  }
  auto Channel = &g_pile->ChannelList[ChannelID];
  auto ChannelData = (Channel_ScreenShare_Data_t *)Channel->Buffer;
  
  // Send the flag from viewer to host
  Protocol_S2C_t::Channel_ScreenShare_ViewToShare_t Payload;
  Payload.ChannelID = ChannelID;
  Payload.Flag = Request->Flag;

  Session::WriteCommand(
    ChannelData->HostSessionID,
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

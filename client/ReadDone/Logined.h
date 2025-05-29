case Protocol_S2C_t::InformInvalidIdentify:{
  auto Request = (Protocol_S2C_t::InformInvalidIdentify_t *)RestPacket;

  if(Request->ClientIdentify != g_pile->UDP.IdentifySecret){
    /* we didnt send that */
    goto StateDone_gt;
  }

  g_pile->UDP.IdentifySecret = Request->ServerIdentify;

  goto StateDone_gt;
}
case Protocol_S2C_t::CreateChannel_OK:{
  auto Request = (Protocol_S2C_t::CreateChannel_OK_t *)RestPacket;

  /* TODO check IDMap even before this file to prevent code spam */
  /* TODO check if that id was for create channel */
  if(IDMap_DoesInputExists(&g_pile->TCP.IDMap, &BasePacket->ID) == false){
    __abort();
    goto StateDone_gt;
  }
  IDMap_Remove(&g_pile->TCP.IDMap, &BasePacket->ID);

  switch(Request->Type){
    case Protocol::ChannelType_ScreenShare_e:{
      Channel_Common_t cc(ChannelState_t::ScreenShare, Request->ChannelID, Request->ChannelSessionID);
      cc.m_StateData = new Channel_ScreenShare_t(0);
      ChannelMap_InNew(&g_pile->ChannelMap, &Request->ChannelID, &cc);
      break;
    }
  }

  WriteInformation("[SERVER] CreateChannel_OK ID %lx\n", BasePacket->ID);
  WriteInformation("  ChannelID: %lx\n", Request->ChannelID);

  //HELP

  //std::lock_guard<std::mutex> v(queue_mutex);

  //state_queue.push_back([channel_id = Request->ChannelID.g()] {

  //  Protocol_C2S_t::JoinChannel_t rest;
  //  try {
  //    rest.ChannelID.g() = channel_id;
  //  }
  //  catch (const std::invalid_argument& e) {
  //    fan::print("The input couldn't be converted to a number");
  //    return;
  //  }
  //  catch (const std::out_of_range& e) {
  //    fan::print("The input was too large for unsigned long");
  //    return;
  //  }

  //  uint32_t ID = g_pile->TCP.IDSequence++;

  //  { /* TODO use actual output instead of filler */
  //    uint8_t filler = 0;
  //    IDMap_InNew(&g_pile->TCP.IDMap, &ID, &filler);
  //  }

  //  Channel_Common_t Output(ChannelState_t::WaittingForInformation, rest.ChannelID);
  //  ChannelMap_InNew(&g_pile->ChannelMap, &rest.ChannelID, &Output);

  //  TCP_WriteCommand(
  //    ID,
  //    Protocol_C2S_t::JoinChannel,
  //    rest
  //  );


  //});

  goto StateDone_gt;
}
case Protocol_S2C_t::CreateChannel_Error:{
  auto Request = (Protocol_S2C_t::CreateChannel_Error_t *)RestPacket;

  /* TODO check IDMap even before this file to prevent code spam */
  /* TODO check if that id was for create channel */
  if(IDMap_DoesInputExists(&g_pile->TCP.IDMap, &BasePacket->ID) == false){
    __abort();
    goto StateDone_gt;
  }
  IDMap_Remove(&g_pile->TCP.IDMap, &BasePacket->ID);

  WriteInformation("[SERVER] CreateChannel_Error ID %lx\n", BasePacket->ID);
  WriteInformation("  Reason %lx:%s\n", (uint32_t)Request->Reason, Protocol::JoinChannel_Error_Reason_String[(uint32_t)Request->Reason]);

  goto StateDone_gt;
}
case Protocol_S2C_t::JoinChannel_OK:{
  auto Request = (Protocol_S2C_t::JoinChannel_OK_t *)RestPacket;

  /* TODO check IDMap even before this file to prevent similar code spam */
  /* TODO check if that id was for create channel and check if channelid is right */
  if(IDMap_DoesInputExists(&g_pile->TCP.IDMap, &BasePacket->ID) == false){
    __abort();
    goto StateDone_gt;
  }
  IDMap_Remove(&g_pile->TCP.IDMap, &BasePacket->ID);

  switch(Request->Type){
    case Protocol::ChannelType_ScreenShare_e:{
      Channel_Common_t cc(ChannelState_t::ScreenShare, Request->ChannelID, Request->ChannelSessionID);
      cc.m_StateData = new Channel_ScreenShare_t(0);
      *ChannelMap_GetOutputPointer(&g_pile->ChannelMap, &Request->ChannelID) = cc;


      //HELP
      state_queue.push_back([channel_id = Request->ChannelID.g()] {
        Protocol_ChannelID_t ChannelID;
        ChannelID.g() = channel_id;
        auto ChannelCommon = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
        ChannelCommon->SetState(ChannelState_t::ScreenShare_View);
        auto Channel_ScreenShare = *(Channel_ScreenShare_t*)ChannelCommon->m_StateData;
        ChannelCommon->m_StateData = g_pile->view;
        g_pile->view->init(
          Channel_ScreenShare,
          ChannelCommon->m_ChannelID,
          ChannelCommon->m_ChannelSessionID,
          ChannelCommon->m_ChannelUnique
        );
      });

      break;
    }
    default:{
      __abort();
    }
  }

  WriteInformation("[SERVER] JoinChannel_OK ID %lx\n", BasePacket->ID);
  WriteInformation("  ChannelID: %lx\n", Request->ChannelID);

  goto StateDone_gt;
}
case Protocol_S2C_t::JoinChannel_Error:{
  auto Request = (Protocol_S2C_t::JoinChannel_Error_t *)RestPacket;

  /* TODO check IDMap even before this file to prevent code spam */
  /* TODO check if that id was for create channel */
  if(IDMap_DoesInputExists(&g_pile->TCP.IDMap, &BasePacket->ID) == false){
    __abort();
    goto StateDone_gt;
  }
  IDMap_Remove(&g_pile->TCP.IDMap, &BasePacket->ID);

  WriteInformation("[SERVER] JoinChannel_Error ID %lx\n", BasePacket->ID);
  WriteInformation("  Reason %lx:%s\n", (uint32_t)Request->Reason, Protocol::JoinChannel_Error_Reason_String[(uint32_t)Request->Reason]);

  goto StateDone_gt;
}
case Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewSetFlag:{
  auto Request = (Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewSetFlag_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  auto cc = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
  if(cc == NULL){
    // TODO communication problem
    goto StateDone_gt;
  }
  switch(cc->GetState()){
    case ChannelState_t::ScreenShare:{
      Channel_ScreenShare_t *sd = (Channel_ScreenShare_t *)cc->m_StateData;
      sd->m_ChannelFlag = Request->Flag;
      break;
    }
    case ChannelState_t::ScreenShare_View:{
      Channel_ScreenShare_View_t *sd = (Channel_ScreenShare_View_t *)cc->m_StateData;
      sd->m_ChannelFlag = Request->Flag;
      break;
    }
    default:{
      // TODO communication problem
      break;
    }
  }

  goto StateDone_gt;
}
case Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewMouseCoordinate:{
  auto Request = (Protocol_S2C_t::Channel_ScreenShare_View_InformationToViewMouseCoordinate_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  Channel_ScreenShare_View_t *View;
  {
    auto cc = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
    if(cc == NULL){
      // TODO communication problem
      goto StateDone_gt;
    }
    switch(cc->GetState()){
      case ChannelState_t::ScreenShare_View:{
        break;
      }
      default:{
        // TODO communication problem
        goto StateDone_gt;
      }
    }
    View = (Channel_ScreenShare_View_t *)cc->m_StateData;
  }

  {
    auto Window = View->ThreadCommon->ThreadWindow.Mark();
    if(Window == NULL){
      goto gt_NoWindow;
    }
    TH_lock(&Window->HostMouseCoordinate.Mutex);
    Window->HostMouseCoordinate.got = Request->pos;
    TH_unlock(&Window->HostMouseCoordinate.Mutex);
    View->ThreadCommon->ThreadWindow.Unmark();
  }

  gt_NoWindow:

  goto StateDone_gt;
}
case Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseCoordinate:{
  auto Request = (Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseCoordinate_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  Channel_ScreenShare_Share_t *Share;
  {
    auto cc = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
    if(cc == NULL){
      // TODO communication problem
      goto StateDone_gt;
    }
    switch(cc->GetState()){
      case ChannelState_t::ScreenShare_Share:{
        break;
      }
      default:{
        // TODO communication problem
        goto StateDone_gt;
      }
    }
    Share = (Channel_ScreenShare_Share_t *)cc->m_StateData;
    if(!(Share->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
      goto StateDone_gt;
    }
  }

  {
    MD_Mice_Error err = MD_Mice_Coordinate_Write(&Share->Mice, Request->pos.x, Request->pos.y);
    if(err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary){
      __abort();
    }
  }

  goto StateDone_gt;
}
case Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseMotion:{
  auto Request = (Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseMotion_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  Channel_ScreenShare_Share_t *Share;
  {
    auto cc = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
    if(cc == NULL){
      // TODO communication problem
      goto StateDone_gt;
    }
    switch(cc->GetState()){
      case ChannelState_t::ScreenShare_Share:{
        break;
      }
      default:{
        // TODO communication problem
        goto StateDone_gt;
      }
    }
    Share = (Channel_ScreenShare_Share_t *)cc->m_StateData;
    if(!(Share->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
      goto StateDone_gt;
    }
  }

  {
    MD_Mice_Error err = MD_Mice_Motion_Write(&Share->Mice, Request->Motion.x, Request->Motion.y);
    if(err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary){
      __abort();
    }
  }

  goto StateDone_gt;
}
case Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseButton:{
  auto Request = (Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostMouseButton_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  Channel_ScreenShare_Share_t *Share;
  {
    auto cc = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
    if(cc == NULL){
      // TODO communication problem
      goto StateDone_gt;
    }
    switch(cc->GetState()){
      case ChannelState_t::ScreenShare_Share:{
        break;
      }
      default:{
        // TODO communication problem
        goto StateDone_gt;
      }
    }
    Share = (Channel_ScreenShare_Share_t *)cc->m_StateData;
    if(!(Share->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
      goto StateDone_gt;
    }
  }

  {
    if(Request->pos != fan::vec2i(-1)){
      MD_Mice_Error err = MD_Mice_Coordinate_Write(&Share->Mice, Request->pos.x, Request->pos.y);
      if(err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary){
        __abort();
      }
    }
    {
      MD_Mice_Error err = MD_Mice_Button_Write(&Share->Mice, Request->key, Request->state);
      if(err != MD_Mice_Error_Success && err != MD_Mice_Error_Temporary){
        __abort();
      }
    }
  }

  goto StateDone_gt;
}
case Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostKeyboard:{
  auto Request = (Protocol_S2C_t::Channel_ScreenShare_Share_ApplyToHostKeyboard_t *)RestPacket;

  Protocol_ChannelID_t ChannelID = Request->ChannelID;
  Channel_ScreenShare_Share_t *Share;
  {
    auto cc = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
    if(cc == NULL){
      // TODO communication problem
      goto StateDone_gt;
    }
    switch(cc->GetState()){
      case ChannelState_t::ScreenShare_Share:{
        break;
      }
      default:{
        // TODO communication problem
        goto StateDone_gt;
      }
    }
    Share = (Channel_ScreenShare_Share_t *)cc->m_StateData;
    if(!(Share->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
      goto StateDone_gt;
    }
  }

  {
    MD_Keyboard_Error err = MD_Keyboard_WriteKey(&Share->Keyboard, Request->Scancode, Request->State);
    if(err == MD_Keyboard_Error_UnknownArgument){
      WriteInformation(
        "[CLIENT] [WARNING] %s %s:%lu MD_Keyboard_WriteKey, MD_Keyboard_Error_UnknownArgument\r\n",
        __FUNCTION__, __FILE__, __LINE__);
    }
    else if(err != MD_Keyboard_Error_Success && err != MD_Keyboard_Error_Temporary){
      __abort();
    }
  }

  goto StateDone_gt;
}
default:{
  WriteInformation("unknown read came %lx:%s\n", BasePacket->Command, Protocol_S2C.NA(BasePacket->Command)->sn);
  __abort();
  goto StateDone_gt;
}

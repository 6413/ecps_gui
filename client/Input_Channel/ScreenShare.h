if(CompareCommand(Input, &iCommand, InputSize, "SetMode")){
  gt_SetMode:
  if(!GetNextArgument(Input, &iCommand, InputSize)){
    WriteInformation("SetMode needs a argument\r\n");
    break;
  }
  if(CompareCommand(Input, &iCommand, InputSize, "Share")){
    ChannelCommon->SetState(ChannelState_t::ScreenShare_Share);
    auto Channel_ScreenShare = *(Channel_ScreenShare_t *)ChannelCommon->m_StateData;
    delete (Channel_ScreenShare_t *)ChannelCommon->m_StateData;
    ChannelCommon->m_StateData = new Channel_ScreenShare_Share_t(
      Channel_ScreenShare,
      ChannelCommon->m_ChannelID,
      ChannelCommon->m_ChannelSessionID,
      ChannelCommon->m_ChannelUnique);
  }
  else if(CompareCommand(Input, &iCommand, InputSize, "View")){
    ChannelCommon->SetState(ChannelState_t::ScreenShare_View);
    auto Channel_ScreenShare = *(Channel_ScreenShare_t *)ChannelCommon->m_StateData;
    delete (Channel_ScreenShare_t *)ChannelCommon->m_StateData;
    ChannelCommon->m_StateData = new Channel_ScreenShare_View_t(
      Channel_ScreenShare,
      ChannelCommon->m_ChannelID,
      ChannelCommon->m_ChannelSessionID,
      ChannelCommon->m_ChannelUnique);
  }
  else{
    WriteInformation("SetMode unknown mode\r\n");
  }
  break;
}
else{
  WriteInformation("unknown channel command read code for help %s:%lu\r\n", __FILE__, __LINE__);
}
break;

auto sd = (Channel_ScreenShare_View_t *)ChannelCommon->m_StateData;
if(CompareCommand(Input, &iCommand, InputSize, "SetMode")){
  auto ChannelFlag = sd->m_ChannelFlag;
  delete sd;
  ChannelCommon->m_StateData = new Channel_ScreenShare_t(ChannelFlag);
  goto gt_SetMode;
}
else if(CompareCommand(Input, &iCommand, InputSize, "Stats")){
  WriteInformation("Frame_Total %llx\r\n", sd->m_stats.Frame_Total);
  WriteInformation("Frame_Drop %llx\r\n", sd->m_stats.Frame_Drop);
  WriteInformation("Packet_Total %llx\r\n", sd->m_stats.Packet_Total);
  WriteInformation("Packet_HeadDrop %llx\r\n", sd->m_stats.Packet_HeadDrop);
  WriteInformation("Packet_BodyDrop %llx\r\n", sd->m_stats.Packet_BodyDrop);
}
else if(CompareCommand(Input, &iCommand, InputSize, "Decoder")){
  if(GetNextArgument(Input, &iCommand, InputSize) == false){
    WriteInformation("need decoder name as parameter\r\n");
    break;
  }
  uintptr_t size = GetSizeOfArgument(Input, &iCommand, InputSize);
  auto td = sd->ThreadCommon->ThreadDecode.GetOrphanPointer();
  if(size > sizeof(td->DecoderUserProperties.DecoderName)){
    WriteInformation("too long decoder name\r\n");
    break;
  }
  td->DecoderUserProperties.Mutex.Lock();
  td->DecoderUserProperties.DecoderNameSize = size;
  MEM_copy(&Input[iCommand], td->DecoderUserProperties.DecoderName, size);
  td->DecoderUserProperties.Updated = true;
  td->DecoderUserProperties.Mutex.Unlock();
}
else{
  WriteInformation("unknown channel command read code for help %s:%lu\r\n", __FILE__, __LINE__);
}
break;
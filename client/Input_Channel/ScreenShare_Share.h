auto sd = (Channel_ScreenShare_Share_t *)ChannelCommon->m_StateData;
if(CompareCommand(Input, &iCommand, InputSize, "SetMode")){
  auto ChannelFlag = sd->m_ChannelFlag;
  delete sd;
  ChannelCommon->m_StateData = new Channel_ScreenShare_t(ChannelFlag);
  goto gt_SetMode;
}
else if(CompareCommand(Input, &iCommand, InputSize, "Encoder")){
  if(GetNextArgument(Input, &iCommand, InputSize) == false){
    WriteInformation("need encoder name as parameter\r\n");
    break;
  }
  uintptr_t size = GetSizeOfArgument(Input, &iCommand, InputSize);
  if(size > sizeof(sd->ThreadCommon->EncoderSetting.EncoderSetting.EncoderName)){
    WriteInformation("too long encoder name\r\n");
    break;
  }
  sd->ThreadCommon->EncoderSetting.Mutex.Lock();
  sd->ThreadCommon->EncoderSetting.EncoderSetting.EncoderNameSize = size;
  __builtin_memcpy(sd->ThreadCommon->EncoderSetting.EncoderSetting.EncoderName, &Input[iCommand], size);
  sd->ThreadCommon->EncoderSetting.Updated = true;
  sd->ThreadCommon->EncoderSetting.Mutex.Unlock();
}
else if(CompareCommand(Input, &iCommand, InputSize, "FrameRate")){
  if(GetNextArgument(Input, &iCommand, InputSize)){
    uintptr_t size = GetSizeOfArgument(Input, &iCommand, InputSize);
    sd->ThreadCommon->EncoderSetting.Mutex.Lock();
    sd->ThreadCommon->EncoderSetting.EncoderSetting.Setting.InputFrameRate = STR_psf32(&Input[iCommand], size);
    sd->ThreadCommon->EncoderSetting.Updated = true;
    sd->ThreadCommon->EncoderSetting.Mutex.Unlock();
  }
  WriteInformation("FrameRate: %lf\r\n", sd->ThreadCommon->EncoderSetting.EncoderSetting.Setting.InputFrameRate);
}
else if(CompareCommand(Input, &iCommand, InputSize, "BitRate")){
  if(GetNextArgument(Input, &iCommand, InputSize)){
    uintptr_t size = GetSizeOfArgument(Input, &iCommand, InputSize);
    sd->ThreadCommon->EncoderSetting.Mutex.Lock();
    sd->ThreadCommon->EncoderSetting.EncoderSetting.Setting.RateControl.TBR.bps = STR_psh32_digit(&Input[iCommand], size);
    sd->ThreadCommon->EncoderSetting.Updated = true;
    sd->ThreadCommon->EncoderSetting.Mutex.Unlock();
    sd->CalculateNetworkFlowBucket();
  }
  WriteInformation("BitRate: %lx\r\n", sd->ThreadCommon->EncoderSetting.EncoderSetting.Setting.RateControl.TBR.bps);
}
else if(CompareCommand(Input, &iCommand, InputSize, "BucketSize")){
  if(GetNextArgument(Input, &iCommand, InputSize)){
    uintptr_t size = GetSizeOfArgument(Input, &iCommand, InputSize);
    sd->ThreadCommon->EncoderSetting.Mutex.Lock();
    sd->m_NetworkFlow.BucketSize = STR_psh32_digit(&Input[iCommand], size);
    sd->ThreadCommon->EncoderSetting.Mutex.Unlock();
  }
  WriteInformation("BucketSize: %lx\r\n", sd->m_NetworkFlow.BucketSize);
}
else if(CompareCommand(Input, &iCommand, InputSize, "InputControl")){
  if(GetNextArgument(Input, &iCommand, InputSize)){
    uintptr_t size = GetSizeOfArgument(Input, &iCommand, InputSize);
    uint32_t value = STR_psh32_digit(&Input[iCommand], size);
    if(value == 0){
      sd->m_ChannelFlag ^= ProtocolChannel::ScreenShare::ChannelFlag::InputControl;
    }
    else if(value == 1){
      sd->m_ChannelFlag |= ProtocolChannel::ScreenShare::ChannelFlag::InputControl;
    }
    else{
      WriteInformation("InputControl can only be 0 or 1\r\n");
      break;
    }
    sd->FlagIsChanged();
  }
  WriteInformation("InputControl: %lx\r\n",
    !!(sd->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl));
}
else if(CompareCommand(Input, &iCommand, InputSize, "Start")){
  if(sd->m_IsShareStarted == true){
    WriteInformation("share is already started\r\n");
    break;
  }
  sd->Start();
}
else if(CompareCommand(Input, &iCommand, InputSize, "Stop")){
  if(sd->m_IsShareStarted == false){
    WriteInformation("share is already stopped\r\n");
    break;
  }
  sd->Stop();
}
else{
  WriteInformation("unknown channel command read code for help %s:%lu\r\n", __FILE__, __LINE__);
}
break;
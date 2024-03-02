if(!GetNextArgument(Input, &iCommand, InputSize)){
  WriteInformation("Channel needs ChannelID as second argument\r\n");
  return;
}

uintptr_t ChannelIDSize = GetSizeOfArgument(Input, &iCommand, InputSize);
if(ChannelIDSize > 4){
  WriteInformation("ChannelID cant be that long\r\n");
  return;
}

Protocol_ChannelID_t ChannelID;
ChannelID.g() = STR_psh32_digit(&Input[iCommand], ChannelIDSize);

auto ChannelCommon = ChannelMap_GetOutputPointerSafe(&g_pile->ChannelMap, &ChannelID);
if(ChannelCommon == NULL){
  WriteInformation("failed to find channel\r\n");
  return;
}

if(!GetNextArgument(Input, &iCommand, InputSize)){
  WriteInformation("Channel needs third argument\r\n");
  return;
}

switch(ChannelCommon->GetState()){
  case ChannelState_t::WaittingForInformation:{
    WriteInformation("still waitting to channel information\n");
    break;
  }
  case ChannelState_t::ScreenShare:{
    #include "ScreenShare.h"
  }
  case ChannelState_t::ScreenShare_Share:{
    #include "ScreenShare_Share.h"
  }
  case ChannelState_t::ScreenShare_View:{
    #include "ScreenShare_View.h"
  }
}

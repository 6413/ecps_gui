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

MAP_out_t mout = MAP_out(&g_pile->ChannelMap, &ChannelID, sizeof(Protocol_ChannelID_t));
if(mout.data == 0){
  WriteInformation("failed to find channel\r\n");
  return;
}
auto ChannelCommon = (Channel_Common_t *)mout.data;

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

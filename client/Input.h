void ProcessInput(uint8_t *Input, uintptr_t InputSize){
  uintptr_t iCommand = 0;
  switch(g_pile->state){
    case PileState_t::NotConnected:{
      if(CompareCommand(Input, &iCommand, InputSize, "connect")){
        if(!GetNextArgument(Input, &iCommand, InputSize)){
          WriteInformation("connect needs second argument type help for info\r\n");
          return;
        }
        uintptr_t AddressSize = GetSizeOfArgument(Input, &iCommand, InputSize);
        if(AddressSize != 0x0c){
          WriteInformation("connect second argument InputSize expected c got %x\r\n", AddressSize);
          return;
        }
        NET_addr_t address;
        address.ip = STR_psh32_digit(Input + iCommand, 8);
        iCommand += 8;
        address.port = STR_psh32_digit(Input + iCommand, 4);
        iCommand += 4;

        NET_TCP_sockopt_t sockopt;
        sockopt.level = IPPROTO_TCP;
        sockopt.optname = TCP_NODELAY;
        sockopt.value = 1;
        sint32_t err = NET_TCP_connect(g_pile->TCP.tcp, &g_pile->TCP.peer, &address, &sockopt, 1);
        if(err != 0){
          WriteInformation("connect error: %ld\r\n", err);
          return;
        }

        g_pile->state = PileState_t::Connecting;
      }
      else{
        WriteInformation(
          "connect <ip port> - connects server\r\n"
        );
      }
      break;
    }
    case PileState_t::Connecting:{
      WriteInformation("wait for connect result\r\n");
      break;
    }
    case PileState_t::Connected:{
      WriteInformation("wait for login result\r\n");
      break;
    }
    case PileState_t::Logined:{
      if(CompareCommand(Input, &iCommand, InputSize, "CreateChannel")){
        if(!GetNextArgument(Input, &iCommand, InputSize)){
          WriteInformation("CreateChannel needs channel type as second argument\r\n");
          return;
        }

        Protocol_C2S_t::CreateChannel_t::dt rest;
        rest.Type = -1;

        for(uint8_t i = 0; i < Protocol::ChannelType_Amount; i++){
          if(CompareCommand(Input, &iCommand, InputSize, Protocol::ChannelType_Text[i])){
            rest.Type = i;
            break;
          }
        }

        if(rest.Type == (uint8_t)-1){
          WriteInformation("unknown channel type\r\n");
          return;
        }

        uint32_t ID = g_pile->TCP.IDSequence++;

        { /* TODO use actual output instead of filler */
          uint8_t filler = 0;
          IDMap_InNew(&g_pile->TCP.IDMap, &ID, &filler);
        }

        TCP_WriteCommand(
          ID,
          Protocol_C2S_t::AN(&Protocol_C2S_t::CreateChannel),
          rest);
        WriteInformation("[CLIENT] CreateChannel request ID %lx\n", ID);
      }
      else if(CompareCommand(Input, &iCommand, InputSize, "JoinChannel")){
        if(!GetNextArgument(Input, &iCommand, InputSize)){
          WriteInformation("JoinChannel needs ChannelID as second argument\r\n");
          return;
        }

        uintptr_t ChannelIDSize = GetSizeOfArgument(Input, &iCommand, InputSize);
        if(ChannelIDSize > 4){
          WriteInformation("ChannelID cant be that long\r\n");
          return;
        }

        Protocol_C2S_t::JoinChannel_t::dt rest;
        rest.ChannelID.g() = STR_psh32_digit(&Input[iCommand], ChannelIDSize);

        uint32_t ID = g_pile->TCP.IDSequence++;

        { /* TODO use actual output instead of filler */
          uint8_t filler = 0;
          IDMap_InNew(&g_pile->TCP.IDMap, &ID, &filler);
        }

        Channel_Common_t Output(ChannelState_t::WaittingForInformation, rest.ChannelID);
        ChannelMap_InNew(&g_pile->ChannelMap, &rest.ChannelID, &Output);

        TCP_WriteCommand(
          ID,
          Protocol_C2S_t::AN(&Protocol_C2S_t::JoinChannel),
          rest);
      }
      else if(CompareCommand(Input, &iCommand, InputSize, "Channel")){
        #include "Input_Channel/Input_Channel.h"
      }
      else{
        WriteInformation(
          "CreateChannel <type>\r\n"
          "JoinChannel <ChannelID>\r\n"
          "Channel <ChannelID> <Command...>\r\n"
          "QuitChannel <ChannelID>\r\n"
        );
      }
      break;
    }
    default:{
      __unreachable();
    }
  }
  return;
}

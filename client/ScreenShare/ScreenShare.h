struct Channel_ScreenShare_t{
  ProtocolChannel::ScreenShare::ChannelFlag::_t m_ChannelFlag;

  Channel_ScreenShare_t(ProtocolChannel::ScreenShare::ChannelFlag::_t ChannelFlag){
    m_ChannelFlag = ChannelFlag;
  }
};

#if ENDIAN == 0
  #error network related headers need endian fixed
#endif

#pragma pack(push, 1)

struct ScreenShare_StreamHeader_Body_t {
  uint8_t sc[3];

  void SetSequence(uint16_t Sequence) {
    *(uint16_t*)&sc[0] ^= *(uint16_t*)&sc[0] & 0xf0ff;
    *(uint16_t*)&sc[0] |= Sequence & 0x00ff | (Sequence & 0x0f00) << 4;
  }
  uint16_t GetSequence() {
    return *(uint16_t*)&sc[0] & 0x00ff | (*(uint16_t*)&sc[0] & 0xf000) >> 4;
  }
  void SetCurrent(uint16_t Current) {
    *(uint16_t*)&sc[1] ^= *(uint16_t*)&sc[1] & 0xff0f;
    *(uint16_t*)&sc[1] |= Current & 0x000f | (Current & 0x0ff0) << 4;
  }
  uint16_t GetCurrent() {
    return *(uint16_t*)&sc[1] & 0x000f | (*(uint16_t*)&sc[1] & 0xff00) >> 4;
  }
};
struct ScreenShare_StreamHeader_Head_t{
  ScreenShare_StreamHeader_Body_t Body;
  uint8_t pf[2];
  #if set_VerboseProtocol_HoldStreamTimes == 1
    struct _VerboseTime_t{
      uint64_t ScreenRead = 0;
      uint64_t SourceOptimize = 0;
      uint64_t Encode = 0;
      uint64_t WriteQueue = 0;
      uint64_t ThreadFrameEnd = 0;
      uint64_t NetworkWrite = 0;
    }_VerboseTime;
  #endif

  void SetPossible(uint16_t Possible){
    *(uint16_t *)&pf[0] ^= *(uint16_t *)&pf[0] & 0xf0ff;
    *(uint16_t *)&pf[0] |= Possible & 0x00ff | (Possible & 0x0f00) << 4;
  }
  uint16_t GetPossible(){
    return *(uint16_t *)&pf[0] & 0x00ff | (*(uint16_t *)&pf[0] & 0xf000) >> 4;
  }
  void SetFlag(uint8_t Flag){
    *(uint8_t *)&pf[1] ^= *(uint16_t *)&pf[2] & 0x0f;
    *(uint8_t *)&pf[1] |= Flag & 0x0f;
  }
  uint8_t GetFlag(){
    return *(uint8_t *)&pf[1] & 0x0f;
  }
};

#pragma pack(pop)

#include "Share/Share.h"
#include "View/View.h"

#if 0

void ScreenShare_CleanShareStarted(pile_t *pile, TCPMain_InChannel_ScreenShare_Share_t *Share){
  MD_SCR_close(&Share->mdscr);

  EV_timer_stop(&pile->listener, &Share->FrameTimer);

  AV_packet_close(Share->av.packet);
  AV_frame_close(Share->av.frame);
  AV_context_close(Share->av.context);
  AV_dict_free(&Share->av.dict);

  MD_Mice_close(&Share->Mice);
}

void ScreenShare_SetNewSequence(TCPMain_InChannel_ScreenShare_View_t *View, uint16_t Sequence){
  View->Sequence = Sequence;
  View->Possible = (uint16_t)-1;

  View->ModuloIsAt = (uint16_t)-1;

  __builtin_memset(View->DataCheck, 0, sizeof(View->DataCheck));
}

bool ScreenShare_IsSequencePast(uint16_t CurrentSequence, uint16_t PacketSequence){
  if(CurrentSequence > PacketSequence){
    if(CurrentSequence - PacketSequence < 0x800){
      return 1;
    }
    return 0;
  }
  else{
    if(PacketSequence - CurrentSequence < 0x800){
      return 0;
    }
    return 1;
  }
}

void ScreenShare_SetDataCheck(TCPMain_InChannel_ScreenShare_View_t *View, uint16_t Index){
  uint8_t *byte = &View->DataCheck[Index / 8];
  *byte |= 1 << Index % 8;
}
bool ScreenShare_GetDataCheck(TCPMain_InChannel_ScreenShare_View_t *View, uint16_t Index){
  uint8_t *byte = &View->DataCheck[Index / 8];
  return (*byte & (1 << Index % 8)) >> Index % 8;
}

uint16_t ScreenShare_FindLastDataCheck(TCPMain_InChannel_ScreenShare_View_t *View, uint16_t start){
  uint8_t *DataCheck = &View->DataCheck[start / 8];
  uint8_t *DataCheck_End = View->DataCheck - 1;
  do{
    if(*DataCheck){
      uint16_t r = (uintptr_t)DataCheck - (uintptr_t)View->DataCheck;
      for(r = r * 8 + 7;; r--){
        if(ScreenShare_GetDataCheck(View, r)){
          return r;
        }
      }
    }
    DataCheck--;
  }while(DataCheck != DataCheck_End);
  return (uint16_t)-1;
}

void ScreenShare_FixFramePacket(TCPMain_InChannel_ScreenShare_View_t *View){
  View->stats.Frame_Total++;

  uint16_t LastDataCheck;
  if(View->Possible == (uint16_t)-1){
    LastDataCheck = ScreenShare_FindLastDataCheck(View, 0x1000);
  }
  else{
    if(View->Possible){
      LastDataCheck = ScreenShare_FindLastDataCheck(View, View->Possible - 1 + !!View->Modulo);
    }
    else{
      LastDataCheck = (uint16_t)-1;
    }
  }

  if(LastDataCheck == (uint16_t)-1){
    /* we cant fix anything in this packet */
    View->stats.Frame_Drop++;
    View->Possible = (uint16_t)-1;
    return;
  }

  View->stats.Packet_Total++;
  if(View->Possible == (uint16_t)-1){
    View->stats.Packet_HeadDrop++;
    if(View->ModuloIsAt != (uint16_t)-1){
      View->Possible = View->ModuloIsAt;
      View->Modulo = View->ModuloSize;
    }
    else{
      View->Possible = LastDataCheck + 1;
      View->Modulo = 0;
    }
    View->Flag = 0;
  }

  View->stats.Packet_Total += View->Possible + !!View->Modulo;
  for(uint16_t i = 0; i < (View->Possible + !!View->Modulo); i++){
    if(!ScreenShare_GetDataCheck(View, i)){
      View->stats.Packet_BodyDrop++;
      __builtin_memset(&View->data[i * 0x400], 0, 0x400);
    }
  }
}

void ScreenShare_WindowResize_cb(fan::window_t *window, const fan::vec2i& res){
  loco_t *loco = loco_t::get_loco(window);
  TCPMain_InChannel_ScreenShare_View_t *View = OFFSETLESS(loco, TCPMain_InChannel_ScreenShare_View_t, window.loco);
  pile_t *pile = View->pile;
  fan::vec2 CursorPosition = (fan::vec2)View->HostMouseCoordinate.had / View->av.FrameResolution * View->window.FrameRenderSize * 2 - 1;
  View->window.loco.sprite.set(&View->window.CursorCID, &loco_t::sprite_t::instance_t::position, CursorPosition);
}

void ScreenShare_WindowTimer_cb(EV_t *listener, EV_timer_t *WindowTimer){
  TCPMain_InChannel_ScreenShare_View_t *View = OFFSETLESS(WindowTimer, TCPMain_InChannel_ScreenShare_View_t, window.WindowTimer);

  if(
    View->HostMouseCoordinate.got.x != View->HostMouseCoordinate.had.x ||
    View->HostMouseCoordinate.got.y != View->HostMouseCoordinate.had.y
  ){
    View->HostMouseCoordinate.had.x = View->HostMouseCoordinate.got.x;
    View->HostMouseCoordinate.had.y = View->HostMouseCoordinate.got.y;
    fan::vec2 CursorPosition = (fan::vec2)View->HostMouseCoordinate.had / View->av.FrameResolution * View->window.FrameRenderSize * 2 - 1;
    fan::print(View->HostMouseCoordinate.had, CursorPosition);
    View->window.loco.sprite.set(&View->window.CursorCID, &loco_t::sprite_t::instance_t::position, CursorPosition);
  }
  if (View->window.loco.process_loop()) {
    PR_exit(0);
  }
}

void ScreenShare_LoadImage_init(TCPMain_InChannel_ScreenShare_View_t *View){
  {
    uint8_t data0[4] = {0, 0, 0, 0};
    uint8_t data1[1] = {0};
    uint8_t data2[1] = {0};
    void *data[3] = {
      (void *)data0,
      (void *)data1,
      (void *)data2
    };
    loco_t::yuv420p_t::properties_t properties;
    properties.viewport = &View->window.viewport;
    properties.matrices = &View->window.matrices;
    properties.position = fan::vec3(0, 0, 0);
    properties.size = fan::vec2ui(1, 1);
    properties.load_yuv(&View->window.loco, (void **)data, fan::vec2ui(1, 1));
    View->window.loco.yuv420p.push_back(&View->window.FrameCID, properties);
  }
  {
    View->window.CursorImage.load(View->window.loco.get_context(), "images/cursor.webp");
    loco_t::sprite_t::properties_t properties;
    properties.viewport = &View->window.viewport;
    properties.matrices = &View->window.matrices;
    properties.position = fan::vec3(0, 0, 1);
    properties.size = 0.1;
    properties.image = &View->window.CursorImage;
    View->window.loco.sprite.push_back(&View->window.CursorCID, properties);
  }
}

void ScreenShare_LoadImage(TCPMain_InChannel_ScreenShare_View_t *View, uint8_t *const *data, const uint32_t *Stride){
  /* TODO stride
  View->window.yuv420p_renderer->set_texture_coordinates(0, {
    fan::vec2(0, 1),
    fan::vec2((f32_t)x / *Stride, 1),
    fan::vec2((f32_t)x / *Stride, 0),
    fan::vec2(0, 0)
  });
  */
  View->window.loco.yuv420p.reload_yuv(&View->window.FrameCID, (void **)data, View->av.FrameResolution);
}

void ScreenShare_ProcessFramePacket(EV_t *listener, TCPMain_InChannel_ScreenShare_View_t *View){
  if(View->Flag & ScreenShare_Flag_KeyFrame_e){
    View->DoWeHaveKeyFrame = 1;
  }

  uint32_t FramePacketSize = View->Possible * 0x400 + View->Modulo;
  IO_ssize_t routwrite = AV_outwrite(View->av.context, View->data, FramePacketSize, View->av.packet);
  if(routwrite != FramePacketSize){
    if(routwrite == AVERROR_INVALIDDATA){
      return;
    }
    else{
      assert(0);
    }
  }
  assert(routwrite == FramePacketSize);
  IO_ssize_t routread = AV_outread(View->av.context, View->av.frame);
  assert(routread >= 0);
  if(!routread){
    return;
  }
  View->av.FrameResolution = fan::vec2ui(View->av.frame->width, View->av.frame->height);
  ScreenShare_LoadImage(View, View->av.frame->data, (const uint32_t *)View->av.frame->linesize);
}

void ScreenShare_Share_evio_udp_cb(EV_t *listener, EV_event_t *evio_udp, uint32_t flag){
  pile_t *pile = OFFSETLESS(listener, pile_t, listener);
  TCPMain_InChannel_ScreenShare_Share_t *Share = OFFSETLESS(evio_udp, TCPMain_InChannel_ScreenShare_Share_t, evio_udp);

  uint8_t buffer[0x800];
  NET_addr_t dstaddr;
  IO_ssize_t size = NET_recvfrom(&Share->udp, buffer, sizeof(buffer), &dstaddr);
  if(size < 0){
    if(size == -EAGAIN){
      return;
    }
    WriteInformation("%lx\n", size);
    assert(0);
  }
  if(dstaddr.ip != Share->udpaddr.ip || dstaddr.port != Share->udpaddr.port){
    /* packet came from elsewhere */
    return;
  }
  if(size == 0){
    switch(pile->state){
      case PeerState_InChannel_ScreenShare_ShareStart_e:
      case PeerState_Waitting_InChannel_ScreenShare_ShareStart_MouseCoordinate_e:{
        break;
      }
      default:{
        InChannel_ScreenShare_Share_KeepAliveTimer_reset(pile, Share);
        break;
      }
    }
  }
}

void ScreenShare_View_evio_udp_cb(EV_t *listener, EV_event_t *evio_udp, uint32_t flag){
  pile_t *pile = OFFSETLESS(listener, pile_t, listener);
  TCPMain_InChannel_ScreenShare_View_t *View = OFFSETLESS(evio_udp, TCPMain_InChannel_ScreenShare_View_t, evio_udp);

  uint8_t buffer[0x800];
  NET_addr_t dstaddr;
  IO_ssize_t size = NET_recvfrom(&View->udp, buffer, sizeof(buffer), &dstaddr);
  if(size < 0){
    if(size == -EAGAIN){
      return;
    }
    WriteInformation("%lx\n", size);
    assert(0);
  }
  if(dstaddr.ip != View->udpaddr.ip || dstaddr.port != View->udpaddr.port){
    /* packet came from elsewhere */
    return;
  }
  InChannel_ScreenShare_View_KeepAliveTimer_reset(pile, View);
  if(size == 0){
    return;
  }
  if(size == sizeof(buffer)){
    WriteInformation("where is mtu for this packet??\n");
    assert(0);
  }

  if(size < sizeof(ScreenShare_StreamHeader_Body_t)){
    return;
  }

  ScreenShare_StreamHeader_Body_t *Body = (ScreenShare_StreamHeader_Body_t *)buffer;
  uint16_t Sequence = ScreenShare_StreamHeader_Body_GetSequence(Body);
  if(Sequence != View->Sequence){
    if(ScreenShare_IsSequencePast(View->Sequence, Sequence)){
      /* this packet came from past */
      return;
    }
    ScreenShare_FixFramePacket(View);
    if(View->Possible != (uint16_t)-1){
      ScreenShare_ProcessFramePacket(&pile->listener, View);
    }
    ScreenShare_SetNewSequence(View, Sequence);
  }
  uint8_t *PacketData;
  uint16_t Current = ScreenShare_StreamHeader_Body_GetCurrent(Body);
  if(Current == 0){
    if(size < sizeof(ScreenShare_StreamHeader_Head_t)){
      return;
    }
    if(View->Possible != (uint16_t)-1){
      /* we already have head somehow */
      return;
    }
    ScreenShare_StreamHeader_Head_t *Head = (ScreenShare_StreamHeader_Head_t *)buffer;
    View->Possible = ScreenShare_StreamHeader_Head_GetPossible(Head);
    View->Modulo = ScreenShare_StreamHeader_Head_GetModulo(Head);
    View->Flag = ScreenShare_StreamHeader_Head_GetFlag(Head);
    PacketData = &buffer[sizeof(ScreenShare_StreamHeader_Head_t)];
  }
  else{
    PacketData = &buffer[sizeof(ScreenShare_StreamHeader_Body_t)];
  }

  uint16_t PacketSize = size - ((uintptr_t)PacketData - (uintptr_t)buffer);
  if(PacketSize != 0x400){
    if(View->ModuloIsAt != (uint16_t)-1){
      /* we already have modulo?? */
      assert(0);
      return;
    }
    View->ModuloIsAt = Current;
    View->ModuloSize = PacketSize;
  }
  __builtin_memcpy(&View->data[Current * 0x400], PacketData, PacketSize);
  ScreenShare_SetDataCheck(View, Current);
  if(
    (View->Possible != (uint16_t)-1 && !View->Modulo && Current == View->Possible) ||
    PacketSize != 0x400
  ){
    ScreenShare_FixFramePacket(View);
    if(View->Possible != (uint16_t)-1){
      ScreenShare_ProcessFramePacket(&pile->listener, View);
    }
    ScreenShare_SetNewSequence(View, (View->Sequence + 1) % 0x1000);
  }
}

void ScreenShare_KeyboardView(fan::window_t *window, uint16_t key, fan::key_state state, void *filler){
  loco_t *loco = loco_t::get_loco(window);
  TCPMain_InChannel_ScreenShare_View_t *View = OFFSETLESS(loco, TCPMain_InChannel_ScreenShare_View_t, window.loco);
  pile_t *pile = View->pile;

  uint8_t mouse_key;
  switch(key){
    case fan::mouse_left:{
      mouse_key = 0;
      break;
    }
    case fan::mouse_right:{
      mouse_key = 1;
      break;
    }
    case fan::mouse_middle:{
      mouse_key = 2;
      break;
    }
    default:{
      return;
    }
  }

  if(state == (fan::key_state)1){
    uint8_t Data[1 + sizeof(Protocol_Request_InChannel_ScreenShare_View_MouseButtonPress_t)];
    Data[0] = Protocol_Request_InChannel_ScreenShare_View_MouseButtonPress_e;
    Protocol_Request_InChannel_ScreenShare_View_MouseButtonPress_t *MouseButtonPress = (Protocol_Request_InChannel_ScreenShare_View_MouseButtonPress_t *)&Data[1];
    fan::vec2i CursorPosition = (fan::vec2)View->ClientMouseCoordinate / View->window.FrameRenderSize * View->av.FrameResolution;
    MouseButtonPress->x = CursorPosition.x;
    MouseButtonPress->y = CursorPosition.y;
    MouseButtonPress->key = mouse_key;
    TCP_write_DynamicPointer(pile->peer, &Data[0], sizeof(Data));
  }
  else{
    uint8_t Data[1 + sizeof(Protocol_Request_InChannel_ScreenShare_View_MouseButtonRelease_t)];
    Data[0] = Protocol_Request_InChannel_ScreenShare_View_MouseButtonRelease_e;
    Protocol_Request_InChannel_ScreenShare_View_MouseButtonRelease_t *MouseButtonRelease = (Protocol_Request_InChannel_ScreenShare_View_MouseButtonRelease_t *)&Data[1];
    fan::vec2i CursorPosition = (fan::vec2)View->ClientMouseCoordinate / View->window.FrameRenderSize * View->av.FrameResolution;
    MouseButtonRelease->x = CursorPosition.x;
    MouseButtonRelease->y = CursorPosition.y;
    MouseButtonRelease->key = mouse_key;
    TCP_write_DynamicPointer(pile->peer, &Data[0], sizeof(Data));
  }
}
void ScreenShare_MiceView(fan::window_t *window, const fan::vec2i &Coordinate, void *filler){
  loco_t *loco = loco_t::get_loco(window);
  TCPMain_InChannel_ScreenShare_View_t *View = OFFSETLESS(loco, TCPMain_InChannel_ScreenShare_View_t, window.loco);
  pile_t *pile = View->pile;

  View->ClientMouseCoordinate = Coordinate;

  uint8_t Data[1 + sizeof(Protocol_Request_InChannel_ScreenShare_View_MouseCoordinate_t)];
  Data[0] = Protocol_Request_InChannel_ScreenShare_View_MouseCoordinate_e;
  Protocol_Request_InChannel_ScreenShare_View_MouseCoordinate_t *MouseCoordinate = (Protocol_Request_InChannel_ScreenShare_View_MouseCoordinate_t *)&Data[1];
  fan::vec2i CursorPosition = (fan::vec2)View->ClientMouseCoordinate / View->window.FrameRenderSize * View->av.FrameResolution;
  MouseCoordinate->x = CursorPosition.x;
  MouseCoordinate->y = CursorPosition.y;
  TCP_write_DynamicPointer(pile->peer, &Data[0], sizeof(Data));
}
#endif

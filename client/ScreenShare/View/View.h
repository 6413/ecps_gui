struct Channel_ScreenShare_View_t{

#include "gui.h"

  struct ChannelInfo_t{
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    uint32_t ChannelUnique;
  };
  ChannelInfo_t ChannelInfo;

  ProtocolChannel::ScreenShare::ChannelFlag::_t m_ChannelFlag;

  #if set_VerboseProtocol_HoldStreamTimes == 1
    uint64_t _VerboseTime;
  #endif

  uint16_t m_Sequence;
  uint16_t m_Possible;

  uint16_t m_ModuloSize;

  uint8_t m_DataCheck[0x201];
  uint8_t m_data[0x400400];

  struct{
    uint64_t Frame_Total;
    uint64_t Frame_Drop;
    uint64_t Packet_Total;
    uint64_t Packet_HeadDrop;
    uint64_t Packet_BodyDrop;
  }m_stats;

  struct ThreadCommon_t;

  struct ThreadDecode_t{
    #include "ThreadDecode.h"
  };
  struct ThreadWindow_t{
    #include "ThreadWindow.h"
  };

  struct ThreadCommon_t{
    #include "ThreadCommon.h"
  }*ThreadCommon;

  void SetNewSequence(uint16_t Sequence){
    m_Sequence = Sequence;
    m_Possible = (uint16_t)-1;

    __builtin_memset(m_DataCheck, 0, sizeof(m_DataCheck));
  }

  bool IsSequencePast(uint16_t PacketSequence){
    if(this->m_Sequence > PacketSequence){
      if(this->m_Sequence - PacketSequence < 0x800){
        return 1;
      }
      return 0;
    }
    else{
      if(PacketSequence - this->m_Sequence < 0x800){
        return 0;
      }
      return 1;
    }
  }

  void SetDataCheck(uint16_t Index){
    uint8_t *byte = &this->m_DataCheck[Index / 8];
    *byte |= 1 << Index % 8;
  }
  bool GetDataCheck(uint16_t Index){
    uint8_t *byte = &this->m_DataCheck[Index / 8];
    return (*byte & (1 << Index % 8)) >> Index % 8;
  }

  uint16_t FindLastDataCheck(uint16_t start){
    uint8_t *DataCheck = &this->m_DataCheck[start / 8];
    uint8_t *DataCheck_End = this->m_DataCheck - 1;
    do{
      if(*DataCheck){
        uint16_t r = (uintptr_t)DataCheck - (uintptr_t)this->m_DataCheck;
        for(r = r * 8 + 7;; r--){
          if(GetDataCheck(r)){
            return r;
          }
        }
      }
      DataCheck--;
    }while(DataCheck != DataCheck_End);
    return (uint16_t)-1;
  }

  void FixFramePacket(){
    this->m_stats.Frame_Total++;

    uint16_t LastDataCheck;
    if(this->m_Possible == (uint16_t)-1){
      LastDataCheck = FindLastDataCheck(0x1000);
    }
    else{
      LastDataCheck = FindLastDataCheck(this->m_Possible);
    }

    if(LastDataCheck == (uint16_t)-1){
      /* we cant fix anything in this packet */
      this->m_stats.Frame_Drop++;
      this->m_Possible = (uint16_t)-1;
      return;
    }

    this->m_stats.Packet_Total++;
    if(this->m_Possible == (uint16_t)-1){
      this->m_stats.Packet_HeadDrop++;
      this->m_Possible = LastDataCheck + 1;
    }

    this->m_stats.Packet_Total += this->m_Possible;
    for(uint16_t i = 0; i < this->m_Possible; i++){
      if(!GetDataCheck(i)){
        this->m_stats.Packet_BodyDrop++;
        __builtin_memset(&this->m_data[i * 0x400], 0, 0x400);
      }
    }
  }

  void WriteFramePacket(){
    #if set_VerboseProtocol_HoldStreamTimes == 1
      uint64_t t = T_nowi();
      WriteInformation("[CLIENT] [DEBUG] VerboseTime WriteFramePacket0 %llu\r\n", t - this->_VerboseTime);
      this->_VerboseTime = t;
    #endif
    uint32_t FramePacketSize = (uint32_t)(this->m_Possible - 1) * 0x400 + this->m_ModuloSize;

    auto Decode = this->ThreadCommon->ThreadDecode.GetOrphanPointer();

    Decode->FramePacketList_Mutex.Lock();

    auto fpnr = Decode->FramePacketList.NewNodeLast();
    auto fp = &Decode->FramePacketList[fpnr];
    fp->Size = FramePacketSize;
    fp->Data = (uint8_t *)A_resize(0, fp->Size);
    __builtin_memcpy(fp->Data, this->m_data, fp->Size);

    if(Decode->FramePacketList.Usage() == 1){
      this->ThreadCommon->StartDecoder();
      gviewing = 1;
    }

    #if set_VerboseProtocol_HoldStreamTimes == 1
      fp->_VerboseTime = T_nowi();
      WriteInformation("[CLIENT] [DEBUG] VerboseTime WriteFramePacket1 %llu %lx\r\n", fp->_VerboseTime - this->_VerboseTime, fpnr);
    #endif

    Decode->FramePacketList_Mutex.Unlock();
  }

  void init(
    Channel_ScreenShare_t& Channel_ScreenShare,
    Protocol_ChannelID_t p_ChannelID,
    Protocol_ChannelSessionID_t p_ChannelSessionID,
    uint32_t p_ChannelUnique
  ) {
    ChannelInfo.ChannelID = p_ChannelID;
    ChannelInfo.ChannelSessionID = p_ChannelSessionID;
    ChannelInfo.ChannelUnique = p_ChannelUnique;

    m_ChannelFlag = Channel_ScreenShare.m_ChannelFlag;

    SetNewSequence(0xffff);

    m_stats.Frame_Total = 0;
    m_stats.Frame_Drop = 0;
    m_stats.Packet_Total = 0;
    m_stats.Packet_HeadDrop = 0;
    m_stats.Packet_BodyDrop = 0;
  }

  Channel_ScreenShare_View_t() = default;

  Channel_ScreenShare_View_t(
    Channel_ScreenShare_t &Channel_ScreenShare,
    Protocol_ChannelID_t p_ChannelID,
    Protocol_ChannelSessionID_t p_ChannelSessionID,
    uint32_t p_ChannelUnique
  ){
    init(Channel_ScreenShare, p_ChannelID, p_ChannelSessionID, p_ChannelUnique);
    ThreadCommon = new ThreadCommon_t(this);
  }
  ~Channel_ScreenShare_View_t(
  ){

  }
};


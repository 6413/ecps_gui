struct Channel_ScreenShare_Share_t{
  struct ChannelInfo_t{
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    uint32_t ChannelUnique;
  };
  ChannelInfo_t m_ChannelInfo;

  bool m_IsShareStarted;

  ProtocolChannel::ScreenShare::ChannelFlag::_t m_ChannelFlag = 0;

  uint16_t m_Sequence = 0;

  struct ThreadCommon_t;

  struct ThreadFrame_t{
    #include "ThreadFrame.h"
  };

  struct ThreadCommon_t{
    #include "ThreadCommon.h"
  }*ThreadCommon;

  /* these used for only write */
  MD_Keyboard_t Keyboard;
  MD_Mice_t Mice;

  struct m_NetworkFlow_t{
    SleepyMutex_t Mutex;

    struct FrameListNodeData_t{
      #if set_VerboseProtocol_HoldStreamTimes == 1
        ScreenShare_StreamHeader_Head_t::_VerboseTime_t _VerboseTime;
      #endif
      VEC_t vec;
      uintptr_t SentOffset;
    };
    #define BLL_set_prefix FrameList
    #define BLL_set_Language 1
    #define BLL_set_AreWeInsideStruct 1
    #define BLL_set_CPP_ConstructDestruct 1
    #define BLL_set_NodeDataType FrameListNodeData_t
    #include <BLL/BLL.h>
    FrameList_t FrameList;
    uint64_t WantedInterval = 5000000;
    uint64_t Bucket; /* in bit */
    uint64_t BucketSize; /* in bit */
    EV_timer_t Timer;
    uint64_t TimerLastCallAt;
    uint64_t TimerCallCount;
  }m_NetworkFlow;

  void FlagIsChanged(){
    Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag_t Payload;
    Payload.ChannelID = m_ChannelInfo.ChannelID;
    Payload.ChannelSessionID = m_ChannelInfo.ChannelSessionID;
    Payload.Flag = m_ChannelFlag;
    TCP_WriteCommand(
      0,
      Protocol_C2S_t::Channel_ScreenShare_Share_InformationToViewSetFlag,
      Payload);
  }

  bool WriteStream(
    uint16_t Current,
    uint16_t Possible,
    uint8_t Flag,
    void *Data,
    uintptr_t DataSize,
    uint64_t *Bucket
  ){
    uint8_t buffer[sizeof(ScreenShare_StreamHeader_Head_t) + 0x400];

    auto Body = (ScreenShare_StreamHeader_Body_t *)buffer;
    Body->SetSequence(this->m_Sequence);
    Body->SetCurrent(Current);

    void *DataWillBeAt;
    if(Current == 0){
      auto Head = (ScreenShare_StreamHeader_Head_t *)buffer;
      Head->SetPossible(Possible);
      Head->SetFlag(Flag);
      DataWillBeAt = (void *)&Head[1];
    }
    else{
      DataWillBeAt = (void *)&Body[1];
    }

    uintptr_t BufferSize = (uintptr_t)DataWillBeAt - (uintptr_t)buffer + DataSize;
    if(Bucket != nullptr){
      if(*Bucket < BufferSize * 8){
        return 1;
      }
      *Bucket -= BufferSize * 8;
    }

    MEM_copy(Data, DataWillBeAt, DataSize);

    ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData_t rest;
    rest.ChannelID = this->m_ChannelInfo.ChannelID;
    rest.ChannelSessionID = this->m_ChannelInfo.ChannelSessionID;
    UDP_send(0, ProtocolUDP::C2S_t::Channel_ScreenShare_Host_StreamData, rest, buffer, BufferSize);

    return 0;
  }
  bool WriteStream(
    uint16_t Current,
    uint16_t Possible,
    uint8_t Flag,
    void *Data,
    uintptr_t DataSize
  ){return WriteStream(
    Current,
    Possible,
    Flag,
    Data,
    DataSize,
    &m_NetworkFlow.Bucket
  );}

  static void NetworkFlow_Timer_cb(EV_t *listener, EV_timer_t *Timer){
    auto Share = OFFSETLESS(Timer, Channel_ScreenShare_Share_t, m_NetworkFlow.Timer);

    Share->m_NetworkFlow.TimerCallCount++;

    uint64_t ctime = EV_nowi(listener);
    uint64_t DeltaTime = ctime - Share->m_NetworkFlow.TimerLastCallAt;
    Share->m_NetworkFlow.TimerLastCallAt = ctime;

    Share->m_NetworkFlow.Bucket +=
      (f32_t)DeltaTime / 1000000000 * Share->ThreadCommon->EncoderSetting.EncoderSetting.Setting.RateControl.TBR.bps * 2;
    if(Share->m_NetworkFlow.Bucket > Share->m_NetworkFlow.BucketSize){
      Share->m_NetworkFlow.Bucket = Share->m_NetworkFlow.BucketSize;
    }

    Share->m_NetworkFlow.Mutex.Lock();

    auto flnr = Share->m_NetworkFlow.FrameList.GetNodeFirst();
    if(flnr == Share->m_NetworkFlow.FrameList.dst){
      /* no frame waitting for us */
      Share->m_NetworkFlow.Mutex.Unlock();
      return;
    }

    f32_t Delay = (f32_t)Share->m_NetworkFlow.FrameList.Usage() / Share->ThreadCommon->EncoderSetting.EncoderSetting.Setting.InputFrameRate;
    if(Delay >= 0.4){
      //WriteInformation("[CLIENT] [WARNING] %s %s:%u Delay is above ~0.4s %lu %f\r\n", __FUNCTION__, __FILE__, __LINE__, Share->m_NetworkFlow.FrameList.Usage(), Share->ThreadCommon->EncoderSetting.EncoderSetting.Setting.InputFrameRate);
      if(Share->m_NetworkFlow.Bucket == Share->m_NetworkFlow.BucketSize){
        //WriteInformation("  Bucket == BucketSize, maybe decrease Interval and increase BucketSize?\r\n");
      }
    }

    auto f = &Share->m_NetworkFlow.FrameList[flnr];

    uint8_t Flag = 0; /* used to get flags from encode but looks useless */

    uint16_t Possible = (f->vec.Current / 0x400) + !!(f->vec.Current % 0x400);

    for(; f->SentOffset < Possible; f->SentOffset++){
      uintptr_t DataSize = f->vec.Current - f->SentOffset * 0x400;
      if(DataSize > 0x400){
        DataSize = 0x400;
      }
      if(Share->WriteStream(f->SentOffset, Possible, Flag, &f->vec.ptr[f->SentOffset * 0x400], DataSize) != 0){
        break;
      }
    }

    Share->m_NetworkFlow.Mutex.Unlock();

    if(f->SentOffset != Possible){
      return;
    }

    VEC_free(&f->vec);

    Share->m_NetworkFlow.FrameList.unlrec(flnr);

    Share->m_Sequence++;
  }

  void CalculateNetworkFlowBucket(){
    uintptr_t MaxBufferSize = (sizeof(ScreenShare_StreamHeader_Head_t) + 0x400) * 8;
    m_NetworkFlow.BucketSize = ThreadCommon->EncoderSetting.EncoderSetting.Setting.RateControl.TBR.bps / 0x20;
    if(m_NetworkFlow.BucketSize < MaxBufferSize){
      WriteInformation("[CLIENT] [WARNING] %s %s:%u BucketSize rounded up\r\n", __FUNCTION__, __FILE__, __LINE__);
      m_NetworkFlow.BucketSize = MaxBufferSize;
    }
    if(m_NetworkFlow.BucketSize / 8 > 0x7fff){
      WriteInformation("[CLIENT] [WARNING] %s %s:%u BucketSize(%x) is too big\r\n", __FUNCTION__, __FILE__, __LINE__, m_NetworkFlow.BucketSize);
    }
  }

  void Start(){
    m_NetworkFlow.Bucket = 0;
    EV_timer_init(&m_NetworkFlow.Timer, (f64_t)m_NetworkFlow.WantedInterval / 1000000000, NetworkFlow_Timer_cb);
    EV_timer_start(&g_pile->listener, &m_NetworkFlow.Timer);
    m_NetworkFlow.TimerLastCallAt = EV_nowi(&g_pile->listener);
    m_NetworkFlow.TimerCallCount = 0;

    ThreadCommon->m_IsShareStarted.CMutex.Lock();
    ThreadCommon->m_IsShareStarted.Value = true;
    ThreadCommon->m_IsShareStarted.CMutex.Signal();
    ThreadCommon->m_IsShareStarted.CMutex.Unlock();
  }
  void Stop(){
    ThreadCommon->m_IsShareStarted.CMutex.Lock();
    ThreadCommon->m_IsShareStarted.Value = false;
    ThreadCommon->m_IsShareStarted.CMutex.Unlock();

    EV_timer_stop(&g_pile->listener, &m_NetworkFlow.Timer);
    m_NetworkFlow.FrameList.Clear();
  }

  Channel_ScreenShare_Share_t(
    Channel_ScreenShare_t &Channel_ScreenShare,
    Protocol_ChannelID_t p_ChannelID,
    Protocol_ChannelSessionID_t p_ChannelSessionID,
    uint32_t p_ChannelUnique
  ){
    m_ChannelInfo.ChannelID = p_ChannelID;
    m_ChannelInfo.ChannelSessionID = p_ChannelSessionID;
    m_ChannelInfo.ChannelUnique = p_ChannelUnique;

    if(MD_Keyboard_open(&Keyboard) != MD_Keyboard_Error_Success){
      PR_abort();
    }
    if(MD_Mice_Open(&Mice) != MD_Mice_Error_Success){
      PR_abort();
    }

    ThreadCommon = new ThreadCommon_t(this);

    this->CalculateNetworkFlowBucket();

    Start();
  }
  ~Channel_ScreenShare_Share_t(
  ){
    PR_abort();
  }
};

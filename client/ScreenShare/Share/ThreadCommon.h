ChannelInfo_t ChannelInfo;

struct{
  bool Value = false;
  SleepyCMutex_t CMutex;
}m_IsShareStarted;

struct{
  ThreadFrame_t::EncoderSetting_t EncoderSetting = {
    .EncoderName = {'x', '2', '6', '4'},
    .EncoderNameSize = 4,
    .Setting = {
      .CodecStandard = ETC_VCODECSTD_H264,
      .UsageType = ETC_VEDC_EncoderSetting_UsageType_Realtime,
      .RateControl{
        .Type = ETC_VEDC_EncoderSetting_RateControlType_VBR,
        .VBR = { .bps = 0x200000 }
      },
      .InputFrameRate = 0x1e
    }
  };
  bool Updated = true;
  FastMutex_t Mutex;
}EncoderSetting;

struct : TRT_CON0_t<ThreadFrame_t>{EV_tp_t tp;}ThreadFrame;
struct : TRT_CON0_t<Channel_ScreenShare_Share_t>{}Share;
struct : TRT_CON0_Itself_t<ThreadCommon_t, 2>{}Itself;

static bool ThreadFrame_tp_outside_cb(EV_t *listener, EV_tp_t *tp){
  auto ThreadCommon = OFFSETLESS(tp, ThreadCommon_t, ThreadFrame.tp);

  auto This = new ThreadFrame_t;

  if(MD_Mice_Open(&This->Mice) != MD_Mice_Error_Success){
    PR_abort();
  }

  if(MD_SCR_open(&This->mdscr) != 0){
    PR_abort();
  }

  ETC_VEDC_Encode_OpenNothing(&This->Encode);

  uint64_t EncoderStartTime = T_nowi();
  uint64_t FrameProcessStartTime = EncoderStartTime;

  ThreadCommon->ThreadFrame.SetPointer(This);

  while(1){
    if(ThreadCommon->m_IsShareStarted.Value == 0){
      ThreadCommon->m_IsShareStarted.CMutex.Lock();
      while(ThreadCommon->m_IsShareStarted.Value == 0){
        ThreadCommon->m_IsShareStarted.CMutex.Wait();
      }
      ThreadCommon->m_IsShareStarted.CMutex.Unlock();
      if(ThreadCommon->Share.IsMarkable() == false){
        break;
      }
    }

    do{
      uint32_t x, y;
      auto r = MD_Mice_Coordinate_Read(&This->Mice, &x, &y);
      if(r == MD_Mice_Error_Temporary){
        break;
      }
      else if(r != MD_Mice_Error_Success){
        PR_abort();
      }

      ITC_Protocol_t::Channel_ScreenShare_Share_MouseCoordinate_t::dt dt;
      dt.Position.x = x;
      dt.Position.y = y;

      ITC_write(
        ITC_Protocol_t::AN(&ITC_Protocol_t::Channel_ScreenShare_Share_MouseCoordinate),
        ThreadCommon->ChannelInfo.ChannelID,
        ThreadCommon->ChannelInfo.ChannelUnique,
        dt);
    }while(0);

    if(ThreadCommon->EncoderSetting.Updated == true){
      ThreadCommon->EncoderSetting.Mutex.Lock();
      auto EncoderSetting = ThreadCommon->EncoderSetting.EncoderSetting;
      ThreadCommon->EncoderSetting.Updated = 0;
      ThreadCommon->EncoderSetting.Mutex.Unlock();
      EncoderSetting.Setting.FrameSizeX = This->mdscr.Geometry.Resolution.x;
      EncoderSetting.Setting.FrameSizeY = This->mdscr.Geometry.Resolution.y;
      if(ETC_VEDC_Encode_IsSame(&This->Encode, EncoderSetting.EncoderNameSize, EncoderSetting.EncoderName) == false){
        ETC_VEDC_Encode_Close(&This->Encode);
        ETC_VEDC_Encode_Error err = ETC_VEDC_Encode_Open(
          &This->Encode,
          EncoderSetting.EncoderNameSize,
          EncoderSetting.EncoderName,
          &EncoderSetting.Setting,
          NULL);
        if(err != ETC_VEDC_Encode_Error_Success){
          WriteInformation("failed to open encoder (%lu)\r\n", err);
          /* TODO */
          PR_abort();
        }
      }
      else{
        if(ETC_VEDC_EncoderSetting_RateControl_IsEqual(
          &This->EncoderSetting.Setting.RateControl, &EncoderSetting.Setting.RateControl
        ) == false){
          ETC_VEDC_Encode_Error err = ETC_VEDC_Encode_SetRateControl(
            &This->Encode,
            &EncoderSetting.Setting.RateControl);
          if(err != ETC_VEDC_Encode_Error_Success){
            /* TODO */
            PR_abort();
          }
        }
        if(This->EncoderSetting.Setting.InputFrameRate != EncoderSetting.Setting.InputFrameRate){
          ETC_VEDC_Encode_Error err = ETC_VEDC_Encode_SetInputFrameRate(
            &This->Encode,
            EncoderSetting.Setting.InputFrameRate);
          if(err != ETC_VEDC_Encode_Error_Success){
            /* TODO */
            PR_abort();
          }
        }
      }
      This->EncoderSetting = EncoderSetting;
    }

    #if set_VerboseProtocol_HoldStreamTimes == 1
      ScreenShare_StreamHeader_Head_t::_VerboseTime_t _VerboseTime;
      _VerboseTime.ScreenRead = T_nowi();
    #endif
    uint8_t *pixelbuf = MD_SCR_read(&This->mdscr);
    if(pixelbuf == 0){
      goto gt_Continue;
    }

    if(
      This->EncoderSetting.Setting.FrameSizeX != This->mdscr.Geometry.Resolution.x ||
      This->EncoderSetting.Setting.FrameSizeY != This->mdscr.Geometry.Resolution.y
    ){
      This->EncoderSetting.Setting.FrameSizeX = This->mdscr.Geometry.Resolution.x;
      This->EncoderSetting.Setting.FrameSizeY = This->mdscr.Geometry.Resolution.y;
      sint32_t err = ETC_VEDC_Encode_SetFrameSize(
        &This->Encode,
        This->EncoderSetting.Setting.FrameSizeX,
        This->EncoderSetting.Setting.FrameSizeY);
      if(err != 0){
        /* TODO */
        PR_abort();
      }
    }

    ETC_VEDC_Encode_Frame_t Frame;
    /* TOOD hardcode to spectific pixel format */
    Frame.Properties.PixelFormat = ETC_PIXF_BGRA;
    Frame.Properties.Stride[0] = This->mdscr.Geometry.LineSize;
    Frame.Properties.SizeX = This->mdscr.Geometry.Resolution.x;
    Frame.Properties.SizeY = This->mdscr.Geometry.Resolution.y;
    Frame.Data[0] = pixelbuf;
    Frame.TimeStamp = FrameProcessStartTime - EncoderStartTime;

    #if set_VerboseProtocol_HoldStreamTimes == 1
      _VerboseTime.Encode = T_nowi();
    #endif
    {
      ETC_VEDC_Encode_Error err = ETC_VEDC_Encode_Write(&This->Encode, ETC_VEDC_Encode_WriteType_Frame, &Frame);
      if(err != ETC_VEDC_Encode_Error_Success){
        /* TODO */
        PR_abort();
      }
    }

    #if set_VerboseProtocol_HoldStreamTimes == 1
      _VerboseTime.WriteQueue = T_nowi();
    #endif

    if(ETC_VEDC_Encode_IsReadable(&This->Encode) == false){
      goto gt_Continue;
    }

    {
      auto Share = ThreadCommon->Share.Mark();
      if(Share == NULL){
        break;
      }

      if(Share->m_NetworkFlow.BucketSize == 0){
        VEC_t FramePacket;
        VEC_init(&FramePacket, 1, A_resize);

        sint32_t rinread;
        void *PacketData;
        ETC_VEDC_Encode_PacketInfo PacketInfo;
        while((rinread = ETC_VEDC_Encode_Read(&This->Encode, &PacketInfo, &PacketData)) > 0){
          FramePacket.Current += rinread;
          VEC_handle(&FramePacket);
          MEM_copy(PacketData, &((uint8_t *)FramePacket.ptr)[FramePacket.Current - rinread], rinread);
        }
        if(rinread < 0){
          /* TODO */
          PR_abort();
        }

        uint8_t Flag = 0;

        uint16_t Possible = (FramePacket.Current / 0x400) + !!(FramePacket.Current % 0x400);
        uint16_t Current = 0;

        for(; Current < Possible; Current++){
          uintptr_t DataSize = FramePacket.Current - Current * 0x400;
          if(DataSize > 0x400){
            DataSize = 0x400;
          }
          Share->WriteStream(Current, Possible, Flag, &FramePacket.ptr[Current * 0x400], DataSize, nullptr);
        }

        VEC_free(&FramePacket);

        Share->m_Sequence++;
      }
      else{
        VEC_t FramePacket;
        VEC_init(&FramePacket, 1, A_resize);

        sint32_t rinread;
        void *PacketData;
        ETC_VEDC_Encode_PacketInfo PacketInfo;
        while((rinread = ETC_VEDC_Encode_Read(&This->Encode, &PacketInfo, &PacketData)) > 0){
          FramePacket.Current += rinread;
          VEC_handle(&FramePacket);
          MEM_copy(PacketData, &((uint8_t *)FramePacket.ptr)[FramePacket.Current - rinread], rinread);
        }
        if(rinread < 0){
          /* TODO */
          PR_abort();
        }

        {
          Share->m_NetworkFlow.Mutex.Lock();
          auto flnr = Share->m_NetworkFlow.FrameList.NewNodeLast();
          auto f = &Share->m_NetworkFlow.FrameList[flnr];
          #if set_VerboseProtocol_HoldStreamTimes == 1
            _VerboseTime.ThreadFrameEnd = T_nowi();
            f->_VerboseTime = _VerboseTime;
          #endif
          f->vec = FramePacket;
          f->SentOffset = 0;
          Share->m_NetworkFlow.Mutex.Unlock();
        }
      }

      ThreadCommon->Share.Unmark();
    }

    gt_Continue:
    {
      uint64_t OneFrameTime = 1000000000 / This->EncoderSetting.Setting.InputFrameRate;
      uint64_t CTime = T_nowi();
      sint64_t TimeDiff = CTime - FrameProcessStartTime;
      if(TimeDiff > OneFrameTime){
        FrameProcessStartTime = CTime;
      }
      else{
        uint64_t SleepTime = OneFrameTime - TimeDiff;
        FrameProcessStartTime = CTime + SleepTime;
        TH_sleepi(SleepTime);
      }
    }
  }

  PR_abort();
  ETC_VEDC_Encode_Close(&This->Encode);
  MD_SCR_close(&This->mdscr);

  return 0;
}
static void ThreadFrame_tp_inside_cb(EV_t *listener, EV_tp_t *tp, sint32_t err){

}

ThreadCommon_t(Channel_ScreenShare_Share_t *p_Share){
  ChannelInfo = p_Share->m_ChannelInfo;

  Share.SetPointer(p_Share);

  EV_tp_init(&ThreadFrame.tp, ThreadFrame_tp_outside_cb, ThreadFrame_tp_inside_cb, 1);
  EV_tp_start(&g_pile->listener, &ThreadFrame.tp);
}
~ThreadCommon_t(){
}

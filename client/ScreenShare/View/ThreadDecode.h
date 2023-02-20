struct FramePacketNodeData_t{
  #if set_VerboseProtocol_HoldStreamTimes == 1
    uint64_t _VerboseTime;
  #endif
  uint32_t Size;
  uint8_t *Data;
};

SleepyMutex_t FramePacketList_Mutex;
#define BLL_set_prefix FramePacketList
#define BLL_set_Language 1
#define BLL_set_CPP_ConstructDestruct
#define BLL_set_AreWeInsideStruct 1
#define BLL_set_NodeDataType FramePacketNodeData_t
#include _WITCH_PATH(BLL/BLL.h)
FramePacketList_t FramePacketList;

struct{
  SleepyMutex_t Mutex;
  uint8_t DecoderName[32];
  uint8_t DecoderNameSize = 0;
  bool Updated = true;
}DecoderUserProperties;

SleepyMutex_t Decoder_Mutex;
ETC_VEDC_Decoder_t Decoder;

ThreadDecode_t(){
  ETC_VEDC_Decoder_OpenNothing(&this->Decoder);
}
~ThreadDecode_t(){
  ETC_VEDC_Decoder_Close(&this->Decoder);
}

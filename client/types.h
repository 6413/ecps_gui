#ifndef set_VerboseProtocol_HoldStreamTimes
  #define set_VerboseProtocol_HoldStreamTimes 0
#endif

#define _INCLUDE_TOKEN(p0, p1) <p0/p1>

#ifdef ETC_VEDC_Decoder_DefineCodec_cuvid
  // #define __GPU_USE_FAKE_CUDA
  // #define __GPU_USE_FAKE_CUVID
  #include _WITCH_PATH(include/cuda.h)
#endif
#include _WITCH_PATH(PR/PR.h)
#include _WITCH_PATH(STR/psh.h)
#include _WITCH_PATH(STR/psf.h)
#include _WITCH_PATH(MD/SCR/SCR.h)
#include _WITCH_PATH(MD/Mice.h)
#include _WITCH_PATH(MD/Keyboard/Keyboard.h)
#include _WITCH_PATH(T/T.h)

#include "../common.h"

enum class ChannelState_t{
  WaittingForInformation,
  ScreenShare,
  ScreenShare_Share,
  ScreenShare_View
};

struct Channel_Common_t{
private:
  ChannelState_t m_state;
public:
  uint32_t m_ChannelUnique; /* TOOD this should be read only for public */
  Protocol_ChannelID_t m_ChannelID;
  Protocol_ChannelSessionID_t m_ChannelSessionID;
  inline static uint32_t _m_ChannelUnique = 0;
  void *m_StateData;

  void SetState(ChannelState_t state){
    m_state = state;
    m_ChannelUnique = _m_ChannelUnique++;
  }
  ChannelState_t GetState(){
    return m_state;
  }
  Channel_Common_t(ChannelState_t state, Protocol_ChannelID_t ChannelID){
    SetState(state);
    m_ChannelID = ChannelID;
  }
  Channel_Common_t(ChannelState_t state, Protocol_ChannelID_t ChannelID, Protocol_ChannelSessionID_t ChannelSessionID){
    SetState(state);
    m_ChannelID = ChannelID;
    m_ChannelSessionID = ChannelSessionID;
  }
  Channel_Common_t(){}
};

#define MAP_set_Prefix ChannelMap
#define MAP_set_InputType Protocol_ChannelID_t
#define MAP_set_OutputType Channel_Common_t
#include _WITCH_PATH(MAP/MAP.h)

/* TODO use actual output instead of 1 byte filler */
#define MAP_set_Prefix IDMap
#define MAP_set_InputType uint32_t
#define MAP_set_OutputType uint8_t
#include _WITCH_PATH(MAP/MAP.h)

#include _WITCH_PATH(TRT/CON0.h)

#define BME_set_Prefix SleepyMutex
#define BME_set_Language 1
#define BME_set_Sleep 1
#include <BME/BME.h>
#define BME_set_Prefix SleepyCMutex
#define BME_set_Language 1
#define BME_set_Sleep 1
#define BME_set_Conditional
#include <BME/BME.h>
#define BME_set_Prefix FastMutex
#define BME_set_Language 1
#define BME_set_Sleep 0
#include <BME/BME.h>

#include _WITCH_PATH(ETC/VEDC/Encode.h)
#include _WITCH_PATH(ETC/VEDC/Decoder.h)

#ifndef set_debug
  #define set_debug
#endif

#ifdef fan_compiler_visual_studio
  /* TODO durum isnt some of that linkings needs to be in fan side? */

  #pragma comment(lib, "Dbghelp.lib")
#endif

enum class PileState_t{
  NotConnected,
  Connecting,
  Connected,
  Logined
};

enum class TCPPeerState_t{
  Idle,
  Waitting_BasePacket,
  Waitting_Data
};

struct TCPMain_SockData_t{
  NET_TCP_layerid_t ReadLayerID;
};
struct TCPMain_PeerData_t{
  TCPPeerState_t state;
  uint32_t iBuffer;
  uint8_t *Buffer;
};

typedef uint32_t ChannelUnique_t;

struct pile_t{
  EV_t listener;

  PileState_t state = PileState_t::NotConnected;

  Protocol_SessionID_t SessionID;
  Protocol_AccountID_t AccountID;

  struct ITC_t{
    TH_mutex_t Mutex;
    #define BVEC_set_prefix bvec
    #define BVEC_set_NodeType uint16_t
    #define BVEC_set_NodeSizeType uint8_t
    #define BVEC_set_NodeData uint8_t
    #include <BVEC/BVEC.h>
    bvec_t bvec;
    EV_async_t ev_async;
  }ITC;

  struct{
    uint32_t IDSequence = 0;
    IDMap_t IDMap;
    NET_TCP_t *tcp;
    NET_TCP_extid_t extid;
    NET_TCP_peer_t *peer;
    uint8_t KeepAliveTimerStepCurrent;
    EV_timer_t KeepAliveTimer;
  }TCP;
  struct{
    NET_addr_t Address;
    NET_socket_t udp;
    EV_event_t ev_udp;
    uint64_t IdentifySecret;
    uint8_t KeepAliveTimerStepCurrent;
    EV_timer_t KeepAliveTimer;
  }UDP;

  ChannelMap_t ChannelMap;

  uint8_t Input[0x1000];
  uintptr_t InputSize = 0;
};

pile_t *g_pile;

struct ITCBasePacket_t{
  Protocol_CI_t Command;
  Protocol_ChannelID_t ChannelID;
  ChannelUnique_t ChannelUnique;
};

struct ITC_Protocol_t : __dme_inherit(ITC_Protocol_t){
  __dme(CloseChannel);
  __dme(Channel_ScreenShare_Share_MouseCoordinate,
    fan::vec2ui Position;
  );
  __dme(Channel_ScreenShare_View_MouseCoordinate,
    fan::vec2si Position;
  );
  __dme(Channel_ScreenShare_View_MouseMotion,
    fan::vec2si Motion;
  );
  __dme(Channel_ScreenShare_View_MouseButton,
    uint8_t key;
    bool state;
    fan::vec2si pos;
  );
  __dme(Channel_ScreenShare_View_KeyboardKey,
    uint16_t Scancode;
    uint8_t State;
  );
}ITC_Protocol;

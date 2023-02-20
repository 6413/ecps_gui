#pragma once

#include <fan/types/vector.h>

#pragma pack(push, 1)

#define set_ProtocolVersion 0x00000000

/* command integer */
typedef uint16_t Protocol_CI_t;

struct ProtocolBasePacket_t{
  uint32_t ID;
  Protocol_CI_t Command;
};

#include <functional>

template<std::size_t count>
struct _compiletime_str
{
  char buffer[count + 1]{};
  int length = count;

  constexpr _compiletime_str(char const* string)
  {
    for (std::size_t i = 0; i < count; ++i) {
      buffer[i] = string[i];
    }
  }
  constexpr operator char const* () const { return buffer; }
};

template<std::size_t count>
_compiletime_str(char const (&)[count])->_compiletime_str<count - 1>;

template <_compiletime_str StringName = "", typename type = long double>
struct ProtocolC_t{
  const char *sn = StringName;
  // data type
  using dt = type;
  /* data struct size */
  uint32_t m_DSS = fan::conditional_value_t<
      std::is_same<
        type, 
        long double
      >::value, 
      0, 
        fan::conditional_value<
          std::is_empty<dt>::value,
          0,
          sizeof(dt)
        >::value
    >::value;
};
#define _ProtocolC_t(p0, p1) \
  using CONCAT(p0,_t) = ProtocolC_t<STR(p0), __return_type_of<decltype([] { \
    struct{p1} v; \
    return v; \
  })>>; \
  CONCAT(p0,_t) p0

struct Protocol_SessionID_t{
  typedef uint32_t Type;
  Type i;
  Type &g(){
    return i;
  }
  Protocol_SessionID_t() = default;
  Protocol_SessionID_t(auto);
  Protocol_SessionID_t(auto, auto);
  bool operator==(Protocol_SessionID_t SessionID){
    return g() == SessionID.g();
  }
};
struct Protocol_AccountID_t{
  typedef uint32_t Type;
  Type i;
  Type &g(){
    return i;
  }
  Protocol_AccountID_t() = default;
  Protocol_AccountID_t(auto p);
  bool operator==(Protocol_AccountID_t AccountID){
    return g() == AccountID.g();
  }
  bool operator!=(Protocol_AccountID_t AccountID){
    return g() != AccountID.g();
  }
  static Protocol_AccountID_t GetInvalid(){
    Protocol_AccountID_t r;
    r.g() = (Type)-1;
    return r;
  }
};
struct Protocol_ChannelID_t{
  typedef uint16_t Type;
  Type i;
  Type &g(){
    return i;
  }
  Protocol_ChannelID_t() = default;
  Protocol_ChannelID_t(auto p);
};
struct Protocol_ChannelSessionID_t{
  typedef uint32_t Type;
  Type i;
  Type &g(){
    return i;
  }
  Protocol_ChannelSessionID_t() = default;
  Protocol_ChannelSessionID_t(auto p);
};
struct Protocol_SessionChannelID_t{
  typedef Protocol_ChannelID_t::Type Type;
  Type i;
  Type &g(){
    return i;
  }
  Protocol_SessionChannelID_t() = default;
  Protocol_SessionChannelID_t(auto p);
};

template <typename inherited_t>
struct _ProtocolC_common_t{
  static constexpr uint32_t GetMemberAmount(){
    return sizeof(inherited_t) / sizeof(ProtocolC_t<>);
  }

  /* number to address */
  ProtocolC_t<> *NA(Protocol_CI_t CI){
    return (ProtocolC_t<> *)((uint8_t *)this + CI * sizeof(ProtocolC_t<>));
  }

  /* address to number */
  static constexpr Protocol_CI_t AN(auto inherited_t:: *C){
    return fan::ofof(C) / sizeof(ProtocolC_t<>);
  }

  /* is number invalid */
  bool ICI(Protocol_CI_t CI){
    return CI * sizeof(ProtocolC_t<>) > sizeof(inherited_t);
  }
};

namespace Protocol{
  const uint64_t InformInvalidIdentifyAt = 1000000000;
  const uint32_t ChannelType_Amount = 1;
  enum{
    ChannelType_ScreenShare_e
  };
  const uint8_t *ChannelType_Text[ChannelType_Amount] = {
    (const uint8_t *)"ScreenShare"
  };

  enum class KickedFromChannel_Reason_t : uint8_t{
    Unknown,
    ChannelIsClosed
  };
  const char *KickedFromChannel_Reason_String[] = {
    "Unknown",
    "ChannelIsClosed"
  };

  enum class JoinChannel_Error_Reason_t : uint8_t{
    InvalidChannelType,
    InvalidChannelID
  };
  const char *JoinChannel_Error_Reason_String[] = {
    "InvalidChannelType",
    "InvalidChannelID"
  };
  enum class LoginType_t : uint8_t{
    Anonymous
  };
}

namespace ProtocolChannel{
  namespace ScreenShare{
    namespace ChannelFlag{
      using _t = uint8_t;
      _t InputControl = 0x01;
    }
    namespace StreamHeadFlag{
      using _t = uint8_t;
      _t KeyFrame = 0x01;
    }
  }
}

namespace ProtocolUDP{
  struct BasePacket_t{
    Protocol_SessionID_t SessionID;
    uint32_t ID;
    uint64_t IdentifySecret;
    Protocol_CI_t Command;
  };
  struct C2S_t : _ProtocolC_common_t<C2S_t>{
    _ProtocolC_t(KeepAlive,);
    _ProtocolC_t(Channel_ScreenShare_Host_StreamData,
      Protocol_ChannelID_t ChannelID;
      Protocol_ChannelSessionID_t ChannelSessionID;
    );
  }C2S;
  struct S2C_t : _ProtocolC_common_t<S2C_t>{
    _ProtocolC_t(KeepAlive,);
    _ProtocolC_t(Channel_ScreenShare_View_StreamData,
      Protocol_ChannelID_t ChannelID;
    );
  }S2C;
}

struct Protocol_C2S_t : _ProtocolC_common_t<Protocol_C2S_t>{
  _ProtocolC_t(KeepAlive,);
  _ProtocolC_t(Request_Login,
    Protocol::LoginType_t Type;
  );
  _ProtocolC_t(CreateChannel,
    uint8_t Type;
  );
  _ProtocolC_t(JoinChannel,
    Protocol_ChannelID_t ChannelID;
  );
  _ProtocolC_t(QuitChannel,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
  );
  _ProtocolC_t(Response_UDPIdentifySecret,
    uint64_t UDPIdentifySecret;
  );
  _ProtocolC_t(Channel_ScreenShare_Share_InformationToViewSetFlag,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    ProtocolChannel::ScreenShare::ChannelFlag::_t Flag;
  );
  _ProtocolC_t(Channel_ScreenShare_Share_InformationToViewMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    fan::vec2ui pos;
  );
  _ProtocolC_t(Channel_ScreenShare_View_ApplyToHostMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    fan::vec2si pos;
  );
  _ProtocolC_t(Channel_ScreenShare_View_ApplyToHostMouseMotion,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    fan::vec2si Motion;
  );
  _ProtocolC_t(Channel_ScreenShare_View_ApplyToHostMouseButton,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    uint8_t key;
    bool state;
    fan::vec2si pos;
  );
  _ProtocolC_t(Channel_ScreenShare_View_ApplyToHostKeyboard,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    uint16_t Scancode;
    bool State;
  );
}Protocol_C2S;

struct Protocol_S2C_t : _ProtocolC_common_t<Protocol_S2C_t>{
  _ProtocolC_t(KeepAlive,);
  _ProtocolC_t(InformInvalidIdentify,
    uint64_t ClientIdentify;
    uint64_t ServerIdentify;
  );
  _ProtocolC_t(Response_Login,
    Protocol_AccountID_t AccountID;
    Protocol_SessionID_t SessionID;
  );
  _ProtocolC_t(CreateChannel_OK,
    uint8_t Type;
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
  );
  _ProtocolC_t(CreateChannel_Error,
    Protocol::JoinChannel_Error_Reason_t Reason;
  );
  _ProtocolC_t(JoinChannel_OK,
    uint8_t Type;
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
  );
  _ProtocolC_t(JoinChannel_Error,
    Protocol::JoinChannel_Error_Reason_t Reason;
  );
  _ProtocolC_t(KickedFromChannel,
    Protocol_ChannelID_t ChannelID;
    Protocol::KickedFromChannel_Reason_t Reason;
  );
  _ProtocolC_t(Request_UDPIdentifySecret,);
  _ProtocolC_t(UseThisUDPIdentifySecret,
    uint64_t UDPIdentifySecret;
  );
  _ProtocolC_t(Channel_ScreenShare_View_InformationToViewSetFlag,
    Protocol_ChannelID_t ChannelID;
    ProtocolChannel::ScreenShare::ChannelFlag::_t Flag;
  );
  _ProtocolC_t(Channel_ScreenShare_View_InformationToViewMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    fan::vec2ui pos;
  );
  _ProtocolC_t(Channel_ScreenShare_Share_ApplyToHostMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    fan::vec2si pos;
  );
  _ProtocolC_t(Channel_ScreenShare_Share_ApplyToHostMouseMotion,
    Protocol_ChannelID_t ChannelID;
    fan::vec2si Motion;
  );
  _ProtocolC_t(Channel_ScreenShare_Share_ApplyToHostMouseButton,
    Protocol_ChannelID_t ChannelID;
    uint8_t key;
    bool state;
    fan::vec2si pos;
  );
  _ProtocolC_t(Channel_ScreenShare_Share_ApplyToHostKeyboard,
    Protocol_ChannelID_t ChannelID;
    uint16_t Scancode;
    uint8_t State;
  );
}Protocol_S2C;

#pragma pack(pop)

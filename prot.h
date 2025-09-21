#pragma pack(push, 1)

#define set_ProtocolVersion 0x00000000

/* command integer */
typedef uint16_t Protocol_CI_t;

struct ProtocolBasePacket_t{
  uint32_t ID;
  Protocol_CI_t Command;
};

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
  struct C2S_t : __dme_inherit(C2S_t){
    __dme(KeepAlive);
    __dme(Channel_ScreenShare_Host_StreamData,
      Protocol_ChannelID_t ChannelID;
      Protocol_ChannelSessionID_t ChannelSessionID;
    );
  }C2S;
  struct S2C_t : __dme_inherit(S2C_t){
    __dme(KeepAlive);
    __dme(Channel_ScreenShare_View_StreamData,
      Protocol_ChannelID_t ChannelID;
    );
  }S2C;
}

struct Protocol_C2S_t : __dme_inherit(Protocol_C2S_t){
  __dme(KeepAlive);
  
  __dme(Request_Login,
    Protocol::LoginType_t Type;
  );
  
  __dme(CreateChannel,
    uint8_t Type;
  );
  
  __dme(JoinChannel,
    Protocol_ChannelID_t ChannelID;
  );
  
  __dme(QuitChannel,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
  );
  
  __dme(Response_UDPIdentifySecret,
    uint64_t UDPIdentifySecret;
  );
  
  __dme(RequestChannelList,
    uint8_t pad; // for some reason command doesnt get processed without pad in server
  );
  __dme(RequestChannelSessionList,
    Protocol_ChannelID_t ChannelID;
  );
  
  __dme(Channel_ScreenShare_Share_InformationToViewSetFlag,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    ProtocolChannel::ScreenShare::ChannelFlag::_t Flag;
  );
  
  __dme(Channel_ScreenShare_Share_InformationToViewMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    fan::vec2ui pos;
  );
  
  __dme(Channel_ScreenShare_View_ApplyToHostMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    fan::vec2si pos;
  );
  
  __dme(Channel_ScreenShare_View_ApplyToHostMouseMotion,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    fan::vec2si Motion;
  );
  
  __dme(Channel_ScreenShare_View_ApplyToHostMouseButton,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    uint8_t key;
    bool state;
    fan::vec2si pos;
  );
  
  __dme(Channel_ScreenShare_View_ApplyToHostKeyboard,
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
    uint16_t Scancode;
    bool State;
  );

  __dme(Channel_ScreenShare_ViewToShare,
    Protocol_ChannelID_t ChannelID;
    uint16_t Flag;
  );
  __dme(Channel_ScreenShare_ShareToView,
    Protocol_ChannelID_t ChannelID;
    uint16_t Flag;
  );
}Protocol_C2S;

// Complete Protocol_S2C_t (Server to Client) structure:
struct Protocol_S2C_t : __dme_inherit(Protocol_S2C_t){
  __dme(KeepAlive);
  
  __dme(InformInvalidIdentify,
    uint64_t ClientIdentify;
    uint64_t ServerIdentify;
  );
  
  __dme(Response_Login,
    Protocol_AccountID_t AccountID;
    Protocol_SessionID_t SessionID;
  );
  
  __dme(CreateChannel_OK,
    uint8_t Type;
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
  );
  
  __dme(CreateChannel_Error,
    Protocol::JoinChannel_Error_Reason_t Reason;
  );
  
  __dme(JoinChannel_OK,
    uint8_t Type;
    Protocol_ChannelID_t ChannelID;
    Protocol_ChannelSessionID_t ChannelSessionID;
  );
  
  __dme(JoinChannel_Error,
    Protocol::JoinChannel_Error_Reason_t Reason;
  );
  
  __dme(KickedFromChannel,
    Protocol_ChannelID_t ChannelID;
    Protocol::KickedFromChannel_Reason_t Reason;
  );
  
  __dme(Request_UDPIdentifySecret);
  
  __dme(UseThisUDPIdentifySecret,
    uint64_t UDPIdentifySecret;
  );
  

  struct ChannelInfo_t {
    Protocol_ChannelID_t ChannelID;        // Channel's unique ID
    uint8_t Type;                          // Channel type (ScreenShare, etc.)
    uint32_t UserCount;                    // Current number of users in channel
    char Name[64];                         // Channel name (null-terminated)
    uint8_t IsPasswordProtected;           // 1 if password required, 0 if not
    Protocol_SessionID_t HostSessionID;    // Session ID of channel host/creator
  };
  
  // Helper structures for session list
  struct SessionInfo_t {
    Protocol_SessionID_t SessionID;           // User's session ID
    Protocol_ChannelSessionID_t ChannelSessionID; // User's channel session ID
    Protocol_AccountID_t AccountID;           // User's account ID
    char Username[32];                       // Username (null-terminated)
    uint8_t IsHost;                         // 1 if user is channel host, 0 if not
    uint64_t JoinedAt;                      // Timestamp when user joined channel
  };
  
  __dme(ChannelList,
    uint16_t ChannelCount;
    // Followed by ChannelCount * ChannelInfo_t
  );
  
  __dme(ChannelSessionList,
    Protocol_ChannelID_t ChannelID;        // Which channel this list is for
    uint16_t SessionCount;                 // Number of sessions/users in the channel
    // Followed by SessionCount * SessionInfo_t
  );
  
  // Screen sharing information messages
  __dme(Channel_ScreenShare_View_InformationToViewSetFlag,
    Protocol_ChannelID_t ChannelID;
    ProtocolChannel::ScreenShare::ChannelFlag::_t Flag;
  );
  
  __dme(Channel_ScreenShare_View_InformationToViewMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    fan::vec2ui pos;
  );
  
  __dme(Channel_ScreenShare_Share_ApplyToHostMouseCoordinate,
    Protocol_ChannelID_t ChannelID;
    fan::vec2si pos;
  );
  
  __dme(Channel_ScreenShare_Share_ApplyToHostMouseMotion,
    Protocol_ChannelID_t ChannelID;
    fan::vec2si Motion;
  );
  
  __dme(Channel_ScreenShare_Share_ApplyToHostMouseButton,
    Protocol_ChannelID_t ChannelID;
    uint8_t key;
    bool state;
    fan::vec2si pos;
  );
  
  __dme(Channel_ScreenShare_Share_ApplyToHostKeyboard,
    Protocol_ChannelID_t ChannelID;
    uint16_t Scancode;
    uint8_t State;
  );
  __dme(Channel_ScreenShare_ViewToShare,
    Protocol_ChannelID_t ChannelID;
    uint16_t Flag;
  );
  __dme(Channel_ScreenShare_ShareToView,
    Protocol_ChannelID_t ChannelID;
    uint16_t Flag;
  );
}Protocol_S2C;

#pragma pack(pop)

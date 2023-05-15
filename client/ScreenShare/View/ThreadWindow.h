/* for window callbacks */
ThreadCommon_t *ThreadCommon;

EV_t ev;
EV_async_t ev_async;

/* for callbacks to check */
ProtocolChannel::ScreenShare::ChannelFlag::_t m_ChannelFlag;

struct{
  /* TOOD need fast mutex */
  TH_mutex_t Mutex;
  fan::vec2i had;
  fan::vec2i got;
}HostMouseCoordinate;
fan::vec2i ViewMouseCoordinate = fan::vec2i(-1);

loco_t loco{{.vsync = false}};
fan::opengl::viewport_t viewport;
loco_t::camera_t camera;
loco_t::texturepack_t TexturePack;
loco_t::texturepack_t::ti_t TextureCursor;
fan::vec2 FrameRenderSize;
fan::vec2ui FrameSize = 1;
loco_t::shape_t FrameCID;
loco_t::shape_t CursorCID;

fan::window_t::keys_callback_NodeReference_t KeyboardKeyCallbackID;
fan::window_t::buttons_callback_NodeReference_t MouseButtonCallbackID;
/* these 2 will be added/removed depends about InputControlMode */
fan::window_t::mouse_position_callback_NodeReference_t CursorPositionCallbackID;
fan::window_t::mouse_motion_callback_NodeReference_t MouseMotionCallbackID;

bool EscapeKeyPressed = false;
enum class InputControlMode_t : uint8_t{
  NoControl,
  Keyboard_MouseC,
  Keyboard_MouseM
}InputControlMode = InputControlMode_t::NoControl;

ETC_VEDC_Decoder_ReadType LastReadMethod = ETC_VEDC_Decoder_ReadType_Unknown;
union ReadMethodData_t{
  ReadMethodData_t(){};
  ~ReadMethodData_t(){};
  #ifdef __GPU_CUDA
    struct CudaArrayFrame_t{
      loco_t::cuda_textures_t cuda_textures;
    }CudaArrayFrame;
  #endif
}ReadMethodData;
void NewReadMethod(ETC_VEDC_Decoder_ReadType Type){
  if(LastReadMethod == Type){
    return;
  }
  switch(LastReadMethod){
    #ifdef __GPU_CUDA
      case ETC_VEDC_Decoder_ReadType_CudaArrayFrame:{
        ReadMethodData.CudaArrayFrame.cuda_textures.close(gloco, this->FrameCID);
        break;
      }
    #endif
    default:{break;}
  }
  switch(Type){
    #ifdef __GPU_CUDA
      case ETC_VEDC_Decoder_ReadType_CudaArrayFrame:{
        new (&ReadMethodData.CudaArrayFrame) ReadMethodData_t::CudaArrayFrame_t;
        break;
      }
    #endif
    default:{break;}
  }
  LastReadMethod = Type;
}

static void CursorCoordinate_cb(const fan::window_t::mouse_move_cb_data_t &p){
  loco_t *loco = loco_t::get_loco(p.window);
  auto Window = OFFSETLESS(loco, ThreadWindow_t, loco);

  if(!(Window->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
    return;
  }

  /* TODO its not safe to access CodecFrame.Resolution without mutex */
  fan::vec2i Position = (fan::vec2)p.position / Window->viewport.get_size() * Window->FrameSize;
  //fan::vec2i Position = (fan::vec2)p.position / Window->viewport.get_size() * Window->CodecFrame.Resolution;
  Window->ViewMouseCoordinate = Position;

  ITC_Protocol_t::Channel_ScreenShare_View_MouseCoordinate_t::dt Payload;
  Payload.Position = Position;

  ITC_write(
    ITC_Protocol_t::AN(&ITC_Protocol_t::Channel_ScreenShare_View_MouseCoordinate),
    Window->ThreadCommon->ChannelInfo.ChannelID,
    Window->ThreadCommon->ChannelInfo.ChannelUnique,
    Payload);
}
static void MouseMotion_cb(const fan::window_t::mouse_motion_cb_data_t &p){
  loco_t *loco = loco_t::get_loco(p.window);
  auto Window = OFFSETLESS(loco, ThreadWindow_t, loco);

  if(!(Window->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
    return;
  }

  ITC_Protocol_t::Channel_ScreenShare_View_MouseMotion_t::dt Payload;
  Payload.Motion = p.motion;

  ITC_write(
    ITC_Protocol_t::AN(&ITC_Protocol_t::Channel_ScreenShare_View_MouseMotion),
    Window->ThreadCommon->ChannelInfo.ChannelID,
    Window->ThreadCommon->ChannelInfo.ChannelUnique,
    Payload);
}
static void MouseButtons_cb(const fan::window_t::mouse_buttons_cb_data_t &p){
  loco_t *loco = loco_t::get_loco(p.window);
  auto Window = OFFSETLESS(loco, ThreadWindow_t, loco);

  if(!(Window->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
    return;
  }
  if(Window->InputControlMode == ThreadWindow_t::InputControlMode_t::NoControl){
    return;
  }

  uint8_t mouse_key;
  switch(p.button){
    case fan::input::mouse_left:{
      mouse_key = 0;
      break;
    }
    case fan::input::mouse_right:{
      mouse_key = 1;
      break;
    }
    case fan::input::mouse_middle:{
      mouse_key = 2;
      break;
    }
    case fan::input::mouse_scroll_up:{
      mouse_key = 3;
      break;
    }
    case fan::input::mouse_scroll_down:{
      mouse_key = 4;
      break;
    }
    default:{
      return;
    }
  }

  ITC_Protocol_t::Channel_ScreenShare_View_MouseButton_t::dt Payload;
  Payload.key = mouse_key;
  if(mouse_key >= 3 && mouse_key <= 4){
    Payload.state = 0; /* unused */
  }
  else{
    Payload.state = p.state == fan::mouse_state::press;
  }
  if(Window->InputControlMode == ThreadWindow_t::InputControlMode_t::Keyboard_MouseM){
    Payload.pos = fan::vec2si(-1);
  }
  else{
    Payload.pos = Window->ViewMouseCoordinate;
  }

  ITC_write(
    ITC_Protocol_t::AN(&ITC_Protocol_t::Channel_ScreenShare_View_MouseButton),
    Window->ThreadCommon->ChannelInfo.ChannelID,
    Window->ThreadCommon->ChannelInfo.ChannelUnique,
    Payload);
}

void ChangeInputControlMode(InputControlMode_t Mode){
  switch(this->InputControlMode){
    case InputControlMode_t::NoControl:{
      break;
    }
    case InputControlMode_t::Keyboard_MouseC:{
      this->loco.get_window()->remove_mouse_move_callback(this->CursorPositionCallbackID);
      break;
    }
    case InputControlMode_t::Keyboard_MouseM:{
      this->loco.get_window()->erase_mouse_motion_callback(this->MouseMotionCallbackID);
      this->loco.get_window()->set_flag_value<fan::window_t::flags::no_mouse>(false);
      break;
    }
  }
  switch(Mode){
    case InputControlMode_t::NoControl:{
      break;
    }
    case InputControlMode_t::Keyboard_MouseC:{
      this->CursorPositionCallbackID = this->loco.get_window()->add_mouse_move_callback(CursorCoordinate_cb);
      break;
    }
    case InputControlMode_t::Keyboard_MouseM:{
      this->loco.get_window()->set_flag_value<fan::window_t::flags::no_mouse>(true);
      this->MouseMotionCallbackID = this->loco.get_window()->add_mouse_motion(MouseMotion_cb);
      break;
    }
  }
  this->InputControlMode = Mode;
}

static void Keys_cb(const fan::window_t::keyboard_keys_cb_data_t &p){
  loco_t *loco = loco_t::get_loco(p.window);
  auto Window = OFFSETLESS(loco, ThreadWindow_t, loco);

  if(p.key == fan::input::key_right_control){
    Window->EscapeKeyPressed = p.state != fan::keyboard_state::release;
    return;
  }
  if(Window->EscapeKeyPressed){
    switch(p.key){
      case fan::input::key_1:{
        Window->ChangeInputControlMode(InputControlMode_t::NoControl);
        break;
      }
      case fan::input::key_2:{
        Window->ChangeInputControlMode(InputControlMode_t::Keyboard_MouseC);
        break;
      }
      case fan::input::key_3:{
        Window->ChangeInputControlMode(InputControlMode_t::Keyboard_MouseM);
        break;
      }
      case fan::input::key_f:{
        if(p.state != fan::keyboard_state::press){
          break;
        }
        switch(p.window->flag_values.m_size_mode){
          case fan::window_t::mode::not_set:
          case fan::window_t::mode::windowed:
          {
            p.window->set_windowed_full_screen();
            break;
          }
          case fan::window_t::mode::borderless:
          case fan::window_t::mode::full_screen:
          {
            p.window->set_windowed();
            break;
          }
        }
        break;
      }
    }
    return;
  }

  if(!(Window->m_ChannelFlag & ProtocolChannel::ScreenShare::ChannelFlag::InputControl)){
    return;
  }
  if(Window->InputControlMode == ThreadWindow_t::InputControlMode_t::NoControl){
    return;
  }

  if(p.scancode == 0){
    /* TOOD can this happen? */
    WriteInformation("[CLIENT] [WARNING] %s %s:%lu Scancode is 0\r\n", __FUNCTION__, __FILE__, __LINE__);
    return;
  }
  if(!!(p.scancode & 0xff00) && !(p.scancode & 0x00ff)){
    /* TODO this is evil for network */
    /* does fan standard prevent it? */
    PR_abort();
  }

  ITC_Protocol_t::Channel_ScreenShare_View_KeyboardKey_t::dt Payload;
  Payload.Scancode = p.scancode;
  switch(p.state){
    case fan::keyboard_state::release: { Payload.State = 0; break; }
    case fan::keyboard_state::press:   { Payload.State = 1; break; }
    case fan::keyboard_state::repeat:  { Payload.State = 2; break; }
  }

  ITC_write(
    ITC_Protocol_t::AN(&ITC_Protocol_t::Channel_ScreenShare_View_KeyboardKey),
    Window->ThreadCommon->ChannelInfo.ChannelID,
    Window->ThreadCommon->ChannelInfo.ChannelUnique,
    Payload);
}

void OpenFrameAndCursor(){
  {
    loco_t::pixel_format_renderer_t::properties_t properties;
    properties.viewport = &viewport;
    properties.camera = &camera;
    properties.position = fan::vec3(0, 0, 0);
    properties.size = fan::vec2(1, 1);
    FrameCID = properties;
  }
  {
    loco_t::sprite_t::properties_t properties;

    properties.viewport = &viewport;
    properties.camera = &camera;

    properties.load_tp(&TextureCursor);

    properties.position = fan::vec3(0, 0, 1);
    properties.size = fan::vec2(16) / loco.get_window()->get_size();

    CursorCID = properties;
  }
}

void HandleCursor(){
  TH_lock(&HostMouseCoordinate.Mutex);

  if(
    HostMouseCoordinate.got.x != HostMouseCoordinate.had.x ||
    HostMouseCoordinate.got.y != HostMouseCoordinate.had.y
  ){
    HostMouseCoordinate.had.x = HostMouseCoordinate.got.x;
    HostMouseCoordinate.had.y = HostMouseCoordinate.got.y;
    fan::vec3 CursorPosition = fan::vec3((fan::vec2)HostMouseCoordinate.had / FrameSize * FrameRenderSize * 2 - 1, 1);
    CursorPosition += CursorCID.get_size();
    CursorCID.set_position(CursorPosition);
  }

  TH_unlock(&HostMouseCoordinate.Mutex);
}

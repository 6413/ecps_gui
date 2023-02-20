/* used for read cursor coordinate */
MD_Mice_t Mice;

MD_SCR_t mdscr;

struct EncoderSetting_t{
  uint8_t EncoderName[32];
  uint8_t EncoderNameSize = 0;
  ETC_VEDC_EncoderSetting_t Setting;
}EncoderSetting;
ETC_VEDC_Encode_t Encode;

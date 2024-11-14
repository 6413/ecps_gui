case Protocol_S2C_t::Response_Login:{
  auto Request = (Protocol_S2C_t::Response_Login_t *)RestPacket;

  /* TODO check IDMap even before this file to prevent code spam */
  /* TODO check if that id was for create channel */
  if(IDMap_DoesInputExists(&g_pile->TCP.IDMap, &BasePacket->ID) == false){
    PR_abort();
    goto StateDone_gt;
  }
  IDMap_Remove(&g_pile->TCP.IDMap, &BasePacket->ID);

  g_pile->SessionID = Request->SessionID;
  g_pile->AccountID = Request->AccountID;

  g_pile->state = PileState_t::Logined;

  UDP_KeepAliveTimer_start();

  WriteInformation("[SERVER] Response_login\r\n");
  WriteInformation("  SessionID: %lx\r\n", g_pile->SessionID);
  WriteInformation("  AccountID: %lx\r\n", g_pile->AccountID);

  goto StateDone_gt;
}

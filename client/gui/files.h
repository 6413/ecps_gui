// Part of ecps_gui_t, included inside the struct by gui.h: file transfer and drag and drop.

  // ---- secure file transfer ----

  std::string file_peer_name(ecps_backend_t::Protocol_SessionID_t peer) {
    for (const auto& [channel_id, sessions] : ecps_backend.channel_sessions) {
      for (const auto& session : sessions) {
        if (session.session_id.i == peer.i) {
          return session.username;
        }
      }
    }
    return "User " + std::to_string(peer.i);
  }

  void send_file_dialog(ecps_backend_t::Protocol_ChannelID_t channel_id, ecps_backend_t::Protocol_SessionID_t peer) {
    fan::graphics::open_file("", [this, channel_id, peer](std::string_view path) {
      // the task is returned, not run inside the lambda, so nothing dangles when the queue drops it
      backend_queue([channel_id, peer, path = std::string(path)]() -> fan::event::task_t {
        return ecps_backend.send_file(channel_id, peer, path);
      });
    });
  }

  void queue_file_action(uint32_t id, ecps_backend_t::Protocol_SessionID_t peer, bool outgoing, int action) {
    backend_queue([id, peer, outgoing, action]() -> fan::event::task_t {
      switch (action) {
      case 0: return ecps_backend.accept_file(id, peer);
      case 1: return ecps_backend.decline_file(id, peer);
      default: return ecps_backend.cancel_file(id, peer, outgoing);
      }
    });
  }

  // ---- drag and drop ----

  fan::window_t::drop_handle_t file_drop_handle;
  std::vector<std::string> dropped_files;
  bool drop_popup_requested = false;

  // dropped files go to the selected channel if we are in it, else to the only one we are in
  bool drop_channel(ecps_backend_t::Protocol_ChannelID_t& out) {
    for (const auto& c : ecps_backend.channel_info) {
      if (c.channel_id.i == selected_channel_id.i) {
        out = c.channel_id;
        return true;
      }
    }
    if (ecps_backend.channel_info.size() == 1) {
      out = ecps_backend.channel_info[0].channel_id;
      return true;
    }
    return false;
  }

  std::vector<ecps_backend_t::session_info_t> drop_peers(ecps_backend_t::Protocol_ChannelID_t channel_id) {
    std::vector<ecps_backend_t::session_info_t> peers;
    auto it = ecps_backend.channel_sessions.find(channel_id.i);
    if (it != ecps_backend.channel_sessions.end()) {
      for (const auto& session : it->second) {
        if (session.session_id.i != ecps_backend.session_id.i) {
          peers.push_back(session);
        }
      }
    }
    return peers;
  }

  void send_dropped_files(ecps_backend_t::Protocol_ChannelID_t channel_id, ecps_backend_t::Protocol_SessionID_t peer) {
    for (const auto& path : dropped_files) {
      backend_queue([channel_id, peer, path]() -> fan::event::task_t {
        return ecps_backend.send_file(channel_id, peer, path);
      });
    }
    dropped_files.clear();
  }

  void render_file_drop() {
    static bool popup_shown = false;

    if (drop_popup_requested) {
      drop_popup_requested = false;
      ecps_backend_t::Protocol_ChannelID_t channel_id;
      bool has_channel = drop_channel(channel_id);
      auto peers = has_channel ? drop_peers(channel_id) : std::vector<ecps_backend_t::session_info_t>{};
      if (peers.size() == 1) {
        send_dropped_files(channel_id, peers[0].session_id);
      }
      else {
        if (has_channel) {
          // the user list may not be loaded yet, the popup picks it up when it arrives
          backend_queue([channel_id]() -> fan::event::task_t {
            return ecps_backend.request_channel_session_list(channel_id);
          });
        }
        gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
        gui::open_popup("Send dropped files");
        popup_shown = true;
      }
    }
    if (!popup_shown) {
      return;
    }
    if (!gui::begin_popup_modal("Send dropped files")) {
      popup_shown = false;
      dropped_files.clear();
      return;
    }
    popup_size = gui::get_window_size();

    std::string what = ecps_backend_t::file_utf8(ecps_backend_t::file_fs_path(dropped_files.empty() ? std::string() : dropped_files[0]).filename());
    if (dropped_files.size() > 1) {
      what += " and " + std::to_string(dropped_files.size() - 1) + " more";
    }
    gui::text(what);
    gui::spacing();

    ecps_backend_t::Protocol_ChannelID_t channel_id;
    auto peers = drop_channel(channel_id) ? drop_peers(channel_id) : std::vector<ecps_backend_t::session_info_t>{};
    if (peers.empty()) {
      gui::text("There is nobody to send to. Join a channel with another user first.");
    }
    else {
      gui::text("Send to:");
      for (const auto& peer : peers) {
        gui::push_id((int)peer.session_id.i);
        if (gui::button(peer.username)) {
          send_dropped_files(channel_id, peer.session_id);
          gui::close_current_popup();
          popup_shown = false;
        }
        gui::pop_id();
      }
    }
    gui::spacing();
    if (popup_shown && gui::button("Cancel")) {
      dropped_files.clear();
      gui::close_current_popup();
      popup_shown = false;
    }
    gui::end_popup();
  }

  static std::string file_progress_text(const ecps_backend_t::file_transfer_view_t& v) {
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%.1f%%", v.size ? (double)v.transferred * 100.0 / (double)v.size : 0.0);
    return std::string(percent) + "  " + ecps_backend_t::file_format_bytes(v.transferred) + " / " + ecps_backend_t::file_format_bytes(v.size);
  }

  // a modal for the oldest file somebody wants to send us
  void render_file_offers() {
    using state_e = ecps_backend_t::file_transfer_t::state_e;
    static bool popup_shown = false;

    auto views = ecps_backend.file_transfer_views();
    const ecps_backend_t::file_transfer_view_t* offer = nullptr;
    for (const auto& v : views) {
      // a picture sent into the chat is shown there: taken at once, no question
      if (!v.outgoing && v.state == state_e::offered && chat_takes_picture(v)) {
        if (chat_accepted_pictures.insert(((std::uint64_t)v.peer.i << 32) | v.id).second) {
          backend_queue([id = v.id, peer = v.peer, folder = chat_picture_folder()]() -> fan::event::task_t {
            return ecps_backend.accept_file(id, peer, folder);
          });
        }
        continue;
      }
      if (!v.outgoing && v.state == state_e::offered) {
        offer = &v;
        break;
      }
    }
    if (offer && !popup_shown) {
      gui::set_next_window_pos(engine.window.get_size() / 2 - popup_size / 2);
      gui::open_popup("Incoming file");
      popup_shown = true;
    }
    if (!popup_shown) {
      return;
    }
    if (!gui::begin_popup_modal("Incoming file")) {
      popup_shown = false;
      return;
    }
    if (!offer) {
      gui::close_current_popup();
      popup_shown = false;
    }
    else {
      popup_size = gui::get_window_size();
      gui::text(file_peer_name(offer->peer) + " wants to send you a file");
      gui::spacing();
      gui::text(offer->name);
      gui::text(ecps_backend_t::file_format_bytes(offer->size));
      gui::spacing();
      text_wrapped_disabled("End-to-end encrypted. It is saved to the ecps_downloads folder.");
      gui::spacing();
      if (gui::button("Accept")) {
        queue_file_action(offer->id, offer->peer, false, 0);
      }
      gui::same_line();
      if (gui::button("Decline")) {
        queue_file_action(offer->id, offer->peer, false, 1);
      }
    }
    gui::end_popup();
  }

  void render_file_transfers() {
    using state_e = ecps_backend_t::file_transfer_t::state_e;
    auto views = ecps_backend.file_transfer_views();

    gui::spacing();
    text_wrapped_disabled("Use 'Send file' next to a user in the Users tab.");
    gui::spacing();

    if (gui::begin_child("##file_transfers", fan::vec2(0, -80), true)) {
      if (views.empty()) {
        gui::text("No file transfers yet.");
      }
      for (size_t i = views.size(); i-- > 0;) {
        const auto& v = views[i];
        gui::push_id((int)i);

        gui::text(std::string(v.outgoing ? "Sending to " : "Receiving from ") + file_peer_name(v.peer));
        gui::text(v.name + "  (" + ecps_backend_t::file_format_bytes(v.size) + ")");
        if (v.state == state_e::transferring) {
          gui::text(file_progress_text(v));
        }
        else {
          gui::text(v.status);
        }
        if (v.state == state_e::done && !v.outgoing) {
          text_wrapped_disabled("Saved to " + v.path);
        }
        if (!v.verification.empty()) {
          gui::text("Verification code: " + v.verification);
        }

        if (!v.outgoing && v.state == state_e::offered) {
          if (gui::button("Accept")) {
            queue_file_action(v.id, v.peer, false, 0);
          }
          gui::same_line();
          if (gui::button("Decline")) {
            queue_file_action(v.id, v.peer, false, 1);
          }
        }
        else if (!ecps_backend_t::file_state_finished(v.state)) {
          if (gui::button("Cancel")) {
            queue_file_action(v.id, v.peer, v.outgoing, 2);
          }
        }
        gui::separator();
        gui::pop_id();
      }
    }
    gui::end_child();

    if (gui::button("Clear finished")) {
      ecps_backend.clear_finished_file_transfers();
    }
    text_wrapped_disabled("If the verification code is the same for both of you, nobody in between,");
    text_wrapped_disabled("the server included, could have read or changed the file.");
  }

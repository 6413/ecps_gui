// included inside ecps_backend_t
//
// secure file transfer between two users of a channel.
//  - the sender offers a file with a fresh x25519 key, the receiver answers with its own,
//    both derive the same chacha20-poly1305 key (hkdf over the dh result, the two public
//    keys, the transfer id, the size and the name). the server only relays sealed chunks.
//  - every chunk is sealed with its index as the nonce, so chunks can't be reordered,
//    replayed or cut off without the receiver noticing.
//  - sizes, offsets and chunk indexes are all 64 bit, there is no file size limit.
//  - a short verification code comes out of the same key derivation. if both users read
//    the same code nobody in the middle (the server included) swapped the keys.

struct file_transfer_t {
  enum class state_e : uint8_t {
    offered,      // incoming, waiting for the user
    waiting,      // outgoing, waiting for the receiver
    transferring,
    done,
    declined,
    cancelled,
    failed
  };

  uint32_t id = 0;
  bool outgoing = false;
  Protocol_ChannelID_t channel_id{};
  Protocol_SessionID_t peer{};
  std::string name;
  std::string path;
  std::string part_path;
  std::array<uint8_t, Protocol::FileNameSize> name_field{};
  uint64_t size = 0;
  uint64_t total_chunks = 0;
  uint64_t next_index = 0;
  uint64_t acked = 0;
  std::atomic<uint64_t> transferred{ 0 };
  std::atomic<state_e> state{ state_e::offered };
  std::string status;
  std::string verification;
  fan::network::crypto::x25519_key_t eph_private{};
  fan::network::crypto::x25519_key_t eph_public{};
  fan::network::crypto::x25519_key_t peer_public{};
  fan::network::crypto::x25519_key_t key{};
  std::ifstream in;
  std::ofstream out;
  fan::event::task_t send_task;
};

struct file_transfer_view_t {
  uint32_t id;
  bool outgoing;
  Protocol_ChannelID_t channel_id;
  Protocol_SessionID_t peer;
  std::string name;
  std::string path;
  std::string status;
  std::string verification;
  uint64_t size;
  uint64_t transferred;
  file_transfer_t::state_e state;
};

static constexpr uint64_t file_chunk = Protocol::FileChunkSize;
// chunks the sender may have in flight, this is what keeps a slow receiver from
// making the server queue the whole file
static constexpr uint64_t file_window = 64;
static constexpr uint64_t file_ack_every = 16;
static constexpr size_t file_max_pending_offers = 8;

std::mutex file_mutex;
std::vector<std::shared_ptr<file_transfer_t>> file_transfers;

static bool file_state_finished(file_transfer_t::state_e s) {
  return s == file_transfer_t::state_e::done || s == file_transfer_t::state_e::declined ||
    s == file_transfer_t::state_e::cancelled || s == file_transfer_t::state_e::failed;
}

static std::filesystem::path file_fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}
static std::string file_utf8(const std::filesystem::path& p) {
  auto s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// the name comes from the other side, it must never be able to leave the downloads folder
static std::string file_sanitize_name(std::string name) {
  auto cut = name.find_last_of("/\\");
  if (cut != std::string::npos) {
    name = name.substr(cut + 1);
  }
  std::string out;
  for (unsigned char c : name) {
    if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
      c = '_';
    }
    out.push_back((char)c);
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
    out.pop_back();
  }
  if (out.empty()) {
    out = "file";
  }
  std::string stem = out.substr(0, out.find('.'));
  for (auto& c : stem) {
    c = (char)std::toupper((unsigned char)c);
  }
  static const char* reserved[] = { "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
  for (auto r : reserved) {
    if (stem == r) {
      out = "_" + out;
      break;
    }
  }
  return out;
}

static std::string file_format_bytes(uint64_t bytes) {
  static const char* units[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 6) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), u ? "%.2f %s" : "%.0f %s", v, units[u]);
  return buf;
}

static void file_make_aad(uint8_t (&aad)[14], uint32_t id, uint64_t index, uint16_t size) {
  std::memcpy(aad, &id, 4);
  std::memcpy(aad + 4, &index, 8);
  std::memcpy(aad + 12, &size, 2);
}

std::shared_ptr<file_transfer_t> find_file_transfer(uint32_t id, Protocol_SessionID_t peer, bool outgoing) {
  std::lock_guard<std::mutex> lock(file_mutex);
  for (auto& t : file_transfers) {
    if (t->id == id && t->peer.i == peer.i && t->outgoing == outgoing) {
      return t;
    }
  }
  return {};
}

void set_file_state(const std::shared_ptr<file_transfer_t>& t, file_transfer_t::state_e s, std::string text = {}) {
  std::lock_guard<std::mutex> lock(file_mutex);
  t->status = std::move(text);
  t->state = s;
}

// drops files and the half written output, call from the backend thread only
void close_file_transfer(const std::shared_ptr<file_transfer_t>& t, bool remove_partial) {
  if (t->in.is_open()) {
    t->in.close();
  }
  if (t->out.is_open()) {
    t->out.close();
  }
  if (remove_partial && !t->part_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(file_fs_path(t->part_path), ec);
  }
  fan::network::crypto::cleanse(t->eph_private.data(), t->eph_private.size());
  fan::network::crypto::cleanse(t->key.data(), t->key.size());
}

void end_file_transfer(const std::shared_ptr<file_transfer_t>& t, file_transfer_t::state_e s, std::string text) {
  close_file_transfer(t, !t->outgoing);
  set_file_state(t, s, std::move(text));
}

void fail_active_file_transfers(const char* reason) {
  std::vector<std::shared_ptr<file_transfer_t>> active;
  {
    std::lock_guard<std::mutex> lock(file_mutex);
    for (auto& t : file_transfers) {
      if (!file_state_finished(t->state)) {
        active.push_back(t);
      }
    }
  }
  for (auto& t : active) {
    end_file_transfer(t, file_transfer_t::state_e::failed, reason);
  }
}

std::vector<file_transfer_view_t> file_transfer_views() {
  std::lock_guard<std::mutex> lock(file_mutex);
  std::vector<file_transfer_view_t> views;
  views.reserve(file_transfers.size());
  for (auto& t : file_transfers) {
    views.push_back({ t->id, t->outgoing, t->channel_id, t->peer, t->name, t->path, t->status,
      t->verification, t->size, t->transferred.load(), t->state.load() });
  }
  return views;
}

void clear_finished_file_transfers() {
  std::lock_guard<std::mutex> lock(file_mutex);
  std::erase_if(file_transfers, [](const auto& t) { return file_state_finished(t->state); });
}

// both sides run this with the same inputs
bool derive_file_key(file_transfer_t& t) {
  using namespace fan::network::crypto;
  const x25519_key_t& sender_public = t.outgoing ? t.eph_public : t.peer_public;
  const x25519_key_t& receiver_public = t.outgoing ? t.peer_public : t.eph_public;

  uint8_t dh[32];
  if (!x25519(t.eph_private, t.peer_public, dh)) {
    return false;
  }
  std::vector<uint8_t> salt;
  salt.insert(salt.end(), sender_public.begin(), sender_public.end());
  salt.insert(salt.end(), receiver_public.begin(), receiver_public.end());
  salt.insert(salt.end(), (uint8_t*)&t.id, (uint8_t*)&t.id + sizeof(t.id));
  salt.insert(salt.end(), (uint8_t*)&t.size, (uint8_t*)&t.size + sizeof(t.size));
  salt.insert(salt.end(), t.name_field.begin(), t.name_field.end());

  uint8_t okm[36];
  bool ok = hkdf(dh, sizeof(dh), salt.data(), salt.size(), Protocol::FileCryptoContext, okm, sizeof(okm));
  cleanse(dh, sizeof(dh));
  if (!ok) {
    return false;
  }
  std::memcpy(t.key.data(), okm, 32);
  uint32_t code;
  std::memcpy(&code, okm + 32, 4);
  cleanse(okm, sizeof(okm));
  code %= 1000000;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%03u %03u", code / 1000, code % 1000);
  t.verification = buf;
  cleanse(t.eph_private.data(), t.eph_private.size());
  return true;
}

// ---- sending ----

fan::event::task_t send_file(Protocol_ChannelID_t channel_id, Protocol_SessionID_t peer, std::string path) {
  auto t = std::make_shared<file_transfer_t>();
  t->outgoing = true;
  t->channel_id = channel_id;
  t->peer = peer;
  t->path = path;
  auto fs_path = file_fs_path(path);
  t->name = file_sanitize_name(file_utf8(fs_path.filename()));
  t->id = std::random_device{}();

  std::string error;
  std::error_code ec;
  auto size = std::filesystem::file_size(fs_path, ec);
  if (ec || !std::filesystem::is_regular_file(fs_path, ec)) {
    error = "can't read the file";
  }
  else if (size == 0) {
    error = "the file is empty";
  }
  else {
    t->size = size;
    t->total_chunks = (size + file_chunk - 1) / file_chunk;
    t->in.open(fs_path, std::ios::binary);
    if (!t->in) {
      error = "can't open the file";
    }
    else if (!fan::network::crypto::x25519_keygen(t->eph_private, t->eph_public)) {
      error = "key generation failed";
    }
  }
  if (!error.empty()) {
    t->state = file_transfer_t::state_e::failed;
    t->status = error;
  }
  else {
    t->state = file_transfer_t::state_e::waiting;
    t->status = "waiting for the receiver";
    // the key derivation uses the name exactly as the receiver will see it
    std::string name = t->name;
    if (name.size() > Protocol::FileNameSize - 1) {
      // cut on a character boundary, the first dropped byte must not be a continuation byte
      size_t n = Protocol::FileNameSize - 1;
      while (n > 0 && ((uint8_t)name[n] & 0xC0) == 0x80) {
        --n;
      }
      name.resize(n);
    }
    t->name = name.empty() ? "file" : name;
    std::memcpy(t->name_field.data(), t->name.data(), t->name.size());
  }
  {
    std::lock_guard<std::mutex> lock(file_mutex);
    file_transfers.push_back(t);
  }
  if (!error.empty()) {
    close_file_transfer(t, false);
    co_return;
  }

  try {
    Protocol_C2S_t::Channel_File_Offer_t rest;
    std::memset(&rest, 0, sizeof(rest));
    rest.ChannelID = channel_id;
    rest.Peer = peer;
    rest.TransferID = t->id;
    rest.FileSize = t->size;
    std::memcpy(rest.EphemeralPublic, t->eph_public.data(), t->eph_public.size());
    std::memcpy(rest.Name, t->name_field.data(), t->name_field.size());
    co_await tcp_write(Protocol_C2S_t::Channel_File_Offer, &rest, sizeof(rest));
  }
  catch (...) {
    end_file_transfer(t, file_transfer_t::state_e::failed, "couldn't send the offer");
  }
}

void on_file_response(const Protocol_S2C_t::Channel_File_Response_t& m) {
  auto t = find_file_transfer(m.TransferID, m.Peer, true);
  if (!t || t->state != file_transfer_t::state_e::waiting) {
    return;
  }
  if (!m.Accepted) {
    end_file_transfer(t, file_transfer_t::state_e::declined, "declined by the receiver");
    return;
  }
  std::memcpy(t->peer_public.data(), m.EphemeralPublic, t->peer_public.size());
  {
    std::lock_guard<std::mutex> lock(file_mutex);
    if (!derive_file_key(*t)) {
      t->status = "key exchange failed";
      t->state = file_transfer_t::state_e::failed;
    }
    else {
      t->status = "sending";
      t->state = file_transfer_t::state_e::transferring;
    }
  }
  if (t->state == file_transfer_t::state_e::transferring) {
    t->send_task = file_send_loop(t);
  }
  else {
    close_file_transfer(t, false);
  }
}

fan::event::task_t file_send_loop(std::shared_ptr<file_transfer_t> t) {
  try {
    Protocol_C2S_t::Channel_File_Chunk_t rest;
    while (t->next_index < t->total_chunks) {
      if (t->state != file_transfer_t::state_e::transferring) {
        co_return;
      }
      if (t->next_index - t->acked >= file_window) {
        co_await fan::co_sleep(5);
        continue;
      }
      uint64_t offset = t->next_index * file_chunk;
      uint16_t n = (uint16_t)std::min<uint64_t>(file_chunk, t->size - offset);

      std::memset(&rest, 0, sizeof(rest));
      t->in.read((char*)rest.Data, n);
      if ((uint64_t)t->in.gcount() != n) {
        end_file_transfer(t, file_transfer_t::state_e::failed, "reading the file failed");
        break;
      }
      rest.ChannelID = t->channel_id;
      rest.Peer = t->peer;
      rest.TransferID = t->id;
      rest.Index = t->next_index;
      rest.Size = n;
      uint8_t aad[14];
      file_make_aad(aad, t->id, rest.Index, n);
      if (!fan::network::crypto::aead(true, t->key, rest.Index, aad, sizeof(aad), rest.Data, n, rest.Tag)) {
        end_file_transfer(t, file_transfer_t::state_e::failed, "encryption failed");
        break;
      }
      co_await tcp_write(Protocol_C2S_t::Channel_File_Chunk, &rest, sizeof(rest));
      t->next_index++;
      t->transferred += n;
    }
  }
  catch (...) {
    if (!file_state_finished(t->state)) {
      end_file_transfer(t, file_transfer_t::state_e::failed, "connection lost");
    }
  }
}

void on_file_ack(const Protocol_S2C_t::Channel_File_Ack_t& m) {
  auto t = find_file_transfer(m.TransferID, m.Peer, true);
  if (!t || t->state != file_transfer_t::state_e::transferring) {
    return;
  }
  t->acked = std::max(t->acked, std::min<uint64_t>(m.Received, t->next_index));
  if (t->acked >= t->total_chunks) {
    end_file_transfer(t, file_transfer_t::state_e::done, "sent");
  }
}

// ---- receiving ----

fan::event::task_t on_file_offer(Protocol_S2C_t::Channel_File_Offer_t m) {
  size_t pending = 0;
  {
    std::lock_guard<std::mutex> lock(file_mutex);
    for (auto& t : file_transfers) {
      if (!t->outgoing && t->state == file_transfer_t::state_e::offered) {
        ++pending;
      }
    }
  }
  bool valid = m.FileSize != 0 && pending < file_max_pending_offers && !find_file_transfer(m.TransferID, m.Peer, false);
  if (!valid) {
    co_await send_file_response(m.ChannelID, m.Peer, m.TransferID, nullptr);
    co_return;
  }
  auto t = std::make_shared<file_transfer_t>();
  t->outgoing = false;
  t->id = m.TransferID;
  t->channel_id = m.ChannelID;
  t->peer = m.Peer;
  t->size = m.FileSize;
  t->total_chunks = (m.FileSize + file_chunk - 1) / file_chunk;
  std::memcpy(t->peer_public.data(), m.EphemeralPublic, t->peer_public.size());
  std::memcpy(t->name_field.data(), m.Name, t->name_field.size());
  t->name_field.back() = 0;
  t->name = file_sanitize_name(std::string(m.Name, strnlen(m.Name, Protocol::FileNameSize - 1)));
  t->state = file_transfer_t::state_e::offered;
  t->status = "waiting for you";
  std::lock_guard<std::mutex> lock(file_mutex);
  file_transfers.push_back(t);
}

fan::event::task_t send_file_response(Protocol_ChannelID_t channel_id, Protocol_SessionID_t peer, uint32_t id, const fan::network::crypto::x25519_key_t* eph_public) {
  Protocol_C2S_t::Channel_File_Response_t rest;
  std::memset(&rest, 0, sizeof(rest));
  rest.ChannelID = channel_id;
  rest.Peer = peer;
  rest.TransferID = id;
  rest.Accepted = eph_public != nullptr;
  if (eph_public) {
    std::memcpy(rest.EphemeralPublic, eph_public->data(), eph_public->size());
  }
  try {
    co_await tcp_write(Protocol_C2S_t::Channel_File_Response, &rest, sizeof(rest));
  }
  catch (...) {}
}

static std::filesystem::path file_unique_path(const std::filesystem::path& dir, const std::string& name) {
  auto base = file_fs_path(name);
  auto candidate = dir / base;
  std::error_code ec;
  for (int i = 1; std::filesystem::exists(candidate, ec) || std::filesystem::exists(file_fs_path(file_utf8(candidate) + ".part"), ec); ++i) {
    candidate = dir / file_fs_path(file_utf8(base.stem()) + " (" + std::to_string(i) + ")" + file_utf8(base.extension()));
  }
  return candidate;
}

fan::event::task_t accept_file(uint32_t id, Protocol_SessionID_t peer, std::string folder = std::string()) {
  auto t = find_file_transfer(id, peer, false);
  if (!t || t->state != file_transfer_t::state_e::offered) {
    co_return;
  }
  std::string error;
  std::error_code ec;
  auto dir = folder.empty() ? std::filesystem::path("ecps_downloads") : file_fs_path(folder);
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    error = "can't create the downloads folder";
  }
  else {
    auto space = std::filesystem::space(dir, ec);
    if (!ec && space.available < t->size) {
      error = "not enough free disk space (" + file_format_bytes(t->size) + " needed)";
    }
  }
  if (error.empty()) {
    auto final_path = file_unique_path(dir, t->name);
    t->path = file_utf8(final_path);
    t->part_path = t->path + ".part";
    t->out.open(file_fs_path(t->part_path), std::ios::binary | std::ios::trunc);
    if (!t->out) {
      error = "can't create the output file";
    }
    else if (!fan::network::crypto::x25519_keygen(t->eph_private, t->eph_public)) {
      error = "key generation failed";
    }
    else {
      std::lock_guard<std::mutex> lock(file_mutex);
      if (!derive_file_key(*t)) {
        error = "key exchange failed";
      }
    }
  }
  if (!error.empty()) {
    end_file_transfer(t, file_transfer_t::state_e::failed, error);
    co_await send_file_response(t->channel_id, t->peer, t->id, nullptr);
    co_return;
  }
  set_file_state(t, file_transfer_t::state_e::transferring, "receiving");
  co_await send_file_response(t->channel_id, t->peer, t->id, &t->eph_public);
}

fan::event::task_t decline_file(uint32_t id, Protocol_SessionID_t peer) {
  auto t = find_file_transfer(id, peer, false);
  if (!t || t->state != file_transfer_t::state_e::offered) {
    co_return;
  }
  end_file_transfer(t, file_transfer_t::state_e::declined, "you declined");
  co_await send_file_response(t->channel_id, t->peer, t->id, nullptr);
}

fan::event::task_t send_file_cancel(Protocol_ChannelID_t channel_id, Protocol_SessionID_t peer, uint32_t id) {
  Protocol_C2S_t::Channel_File_Cancel_t rest;
  std::memset(&rest, 0, sizeof(rest));
  rest.ChannelID = channel_id;
  rest.Peer = peer;
  rest.TransferID = id;
  try {
    co_await tcp_write(Protocol_C2S_t::Channel_File_Cancel, &rest, sizeof(rest));
  }
  catch (...) {}
}

fan::event::task_t cancel_file(uint32_t id, Protocol_SessionID_t peer, bool outgoing) {
  auto t = find_file_transfer(id, peer, outgoing);
  if (!t || file_state_finished(t->state)) {
    co_return;
  }
  end_file_transfer(t, file_transfer_t::state_e::cancelled, "cancelled");
  co_await send_file_cancel(t->channel_id, t->peer, t->id);
}

// the other side stopped, a sender gets this when the receiver quit mid transfer and a
// receiver when the sender did
void on_file_cancel(const Protocol_S2C_t::Channel_File_Cancel_t& m) {
  // the id alone can't tell the direction, but one side can't be both for the same peer and id
  for (bool outgoing : { true, false }) {
    auto t = find_file_transfer(m.TransferID, m.Peer, outgoing);
    if (t && !file_state_finished(t->state)) {
      end_file_transfer(t, file_transfer_t::state_e::cancelled, "cancelled by the other side");
    }
  }
}

fan::event::task_t on_file_chunk(Protocol_S2C_t::Channel_File_Chunk_t m) {
  auto t = find_file_transfer(m.TransferID, m.Peer, false);
  if (!t || t->state != file_transfer_t::state_e::transferring) {
    co_return;
  }
  uint64_t remaining = t->size - t->transferred;
  uint64_t expected = std::min<uint64_t>(file_chunk, remaining);
  if (m.Index != t->next_index || m.Size != expected) {
    end_file_transfer(t, file_transfer_t::state_e::failed, "protocol error");
    co_await send_file_cancel(t->channel_id, t->peer, t->id);
    co_return;
  }
  uint8_t aad[14];
  file_make_aad(aad, t->id, m.Index, m.Size);
  if (!fan::network::crypto::aead(false, t->key, m.Index, aad, sizeof(aad), m.Data, m.Size, m.Tag)) {
    end_file_transfer(t, file_transfer_t::state_e::failed, "data was corrupted or tampered with");
    co_await send_file_cancel(t->channel_id, t->peer, t->id);
    co_return;
  }
  t->out.write((const char*)m.Data, m.Size);
  if (!t->out) {
    end_file_transfer(t, file_transfer_t::state_e::failed, "writing the file failed (disk full?)");
    co_await send_file_cancel(t->channel_id, t->peer, t->id);
    co_return;
  }
  t->transferred += m.Size;
  t->next_index++;

  bool finished = t->next_index == t->total_chunks;
  if (finished) {
    t->out.close();
    std::error_code ec;
    std::filesystem::rename(file_fs_path(t->part_path), file_fs_path(t->path), ec);
    if (ec) {
      end_file_transfer(t, file_transfer_t::state_e::failed, "couldn't finish the file: " + ec.message());
      co_await send_file_cancel(t->channel_id, t->peer, t->id);
      co_return;
    }
    t->part_path.clear();
    end_file_transfer(t, file_transfer_t::state_e::done, "saved");
  }
  if (finished || t->next_index % file_ack_every == 0) {
    Protocol_C2S_t::Channel_File_Ack_t rest;
    std::memset(&rest, 0, sizeof(rest));
    rest.ChannelID = t->channel_id;
    rest.Peer = t->peer;
    rest.TransferID = t->id;
    rest.Received = t->next_index;
    try {
      co_await tcp_write(Protocol_C2S_t::Channel_File_Ack, &rest, sizeof(rest));
    }
    catch (...) {}
  }
}

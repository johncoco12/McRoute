#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#ifdef _WIN32
#include <curses.h>
#else
#include <ncurses.h>
#include <netdb.h>
#endif
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using SocketHandle = SOCKET;
using PollDescriptor = WSAPOLLFD;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
constexpr short ReadEvent = POLLRDNORM;
constexpr int ShutdownBoth = SD_BOTH;
constexpr int ShutdownWrite = SD_SEND;
inline int poll_sockets(PollDescriptor *sockets, ULONG count, int timeout) {
  return WSAPoll(sockets, count, timeout);
}
inline int socket_error() { return WSAGetLastError(); }
inline void close_socket(SocketHandle socket) { closesocket(socket); }
inline int set_socket_option(SocketHandle socket, int level, int name,
                             const void *value, int length) {
  return setsockopt(socket, level, name, static_cast<const char *>(value), length);
}
inline int get_socket_option(SocketHandle socket, int level, int name,
                             void *value, int *length) {
  return getsockopt(socket, level, name, static_cast<char *>(value), length);
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using SocketHandle = int;
using PollDescriptor = pollfd;
constexpr SocketHandle InvalidSocket = -1;
constexpr short ReadEvent = POLLIN;
constexpr int ShutdownBoth = SHUT_RDWR;
constexpr int ShutdownWrite = SHUT_WR;
inline int poll_sockets(PollDescriptor *sockets, nfds_t count, int timeout) {
  return poll(sockets, count, timeout);
}
inline int socket_error() { return errno; }
inline void close_socket(SocketHandle socket) { close(socket); }
inline int set_socket_option(SocketHandle socket, int level, int name,
                             const void *value, socklen_t length) {
  return setsockopt(socket, level, name, value, length);
}
inline int get_socket_option(SocketHandle socket, int level, int name,
                             void *value, socklen_t *length) {
  return getsockopt(socket, level, name, value, length);
}
#endif

namespace {

struct Preset {
  std::string name;
  std::string host;
  uint16_t port;
  std::string motd;
  std::string subtitle;
  int online_players = -1;
  int max_players = -1;
  std::string server_title;
};

class DebugLog {
public:
  void add(const std::string &message) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(message);
    if (entries_.size() > 200)
      entries_.pop_front();
  }

  std::vector<std::string> recent(size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t start = entries_.size() > count ? entries_.size() - count : 0;
    return {entries_.begin() + static_cast<std::ptrdiff_t>(start),
            entries_.end()};
  }

  std::string latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty() ? "" : entries_.back();
  }

private:
  mutable std::mutex mutex_;
  std::deque<std::string> entries_;
};

class PresetStore {
public:
  explicit PresetStore(std::string path) : path_(std::move(path)) {}

  bool load() {
    std::ifstream input(path_);
    if (!input) {
      presets_ = {{"local", "127.0.0.1", 25566, "", "", -1, -1, ""}};
      save();
      return true;
    }

    std::string line;
    while (std::getline(input, line)) {
      if (line.rfind("@active|", 0) == 0) {
        active_name_ = line.substr(8);
        continue;
      }
      std::stringstream fields(line);
      std::string name, host, port_text, motd, subtitle, online_text, max_text,
          title;
      if (!std::getline(fields, name, '|') ||
          !std::getline(fields, host, '|') ||
          !std::getline(fields, port_text, '|') || name.empty() ||
          host.empty()) {
        continue;
      }
      try {
        const auto port = std::stoul(port_text);
        if (port > 0 && port <= 65535) {
          std::getline(fields, motd, '|');
          std::getline(fields, subtitle, '|');
          std::getline(fields, online_text, '|');
          std::getline(fields, max_text, '|');
          std::getline(fields, title, '|');
          const int online = online_text.empty() ? -1 : std::stoi(online_text);
          const int maximum = max_text.empty() ? -1 : std::stoi(max_text);
          presets_.push_back({name, host, static_cast<uint16_t>(port), motd,
                              subtitle, online, maximum, title});
        }
      } catch (const std::exception &) {
        continue;
      }
    }
    if (presets_.empty()) {
      presets_ = {{"local", "127.0.0.1", 25566, "", "", -1, -1, ""}};
    }
    return true;
  }

  bool save() const {
    const std::filesystem::path config_file(path_);
    if (!config_file.parent_path().empty()) {
      std::error_code error;
      std::filesystem::create_directories(config_file.parent_path(), error);
      if (error)
        return false;
    }
    std::ofstream output(path_, std::ios::trunc);
    if (!output)
      return false;
    if (!active_name_.empty())
      output << "@active|" << active_name_ << '\n';
    for (const auto &preset : presets_) {
      output << preset.name << '|' << preset.host << '|' << preset.port << '|'
             << preset.motd << '|' << preset.subtitle << '|'
             << preset.online_players << '|' << preset.max_players << '|'
             << preset.server_title << '\n';
    }
    return true;
  }

  std::vector<Preset> &all() { return presets_; }
  const std::vector<Preset> &all() const { return presets_; }

  int active_index() const {
    for (size_t index = 0; index < presets_.size(); ++index) {
      if (presets_[index].name == active_name_)
        return static_cast<int>(index);
    }
    return 0;
  }

  void set_active_name(const std::string &name) { active_name_ = name; }

private:
  std::string path_;
  std::vector<Preset> presets_;
  std::string active_name_;
};

bool read_varint(const std::vector<char> &bytes, size_t &offset,
                 int32_t &value) {
  value = 0;
  int shift = 0;
  while (offset < bytes.size() && shift < 35) {
    const unsigned char byte = static_cast<unsigned char>(bytes[offset++]);
    value |= static_cast<int32_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0)
      return true;
    shift += 7;
  }
  return false;
}

void append_varint(std::vector<char> &bytes, int32_t value) {
  do {
    char byte = static_cast<char>(value & 0x7f);
    value >>= 7;
    if (value != 0)
      byte |= static_cast<char>(0x80);
    bytes.push_back(byte);
  } while (value != 0);
}

std::string json_escape(const std::string &text) {
  std::string escaped;
  for (const char character : text) {
    if (character == '\\' || character == '"')
      escaped += '\\';
    if (character == '\n')
      escaped += "\\n";
    else if (character == '\r')
      escaped += "\\r";
    else if (character == '\t')
      escaped += "\\t";
    else if (character >= 0x20)
      escaped += character;
  }
  return escaped;
}

std::string status_json(const Preset &preset) {
  const std::string motd = preset.motd.empty() ? preset.name : preset.motd;
  std::string description = "{\"text\":\"" + json_escape(motd) + "\"";
  if (!preset.subtitle.empty()) {
    description +=
        ",\"extra\":[{\"text\":\"\\n" + json_escape(preset.subtitle) + "\"}]";
  }
  description += "}";
  const std::string title =
      preset.server_title.empty() ? "MCRoute" : preset.server_title;
  std::string result =
      "{\"version\":{\"name\":\"" + json_escape(title) + "\",\"protocol\":-1}";
  if (preset.online_players >= 0 || preset.max_players >= 0) {
    result += ",\"players\":{";
    result += "\"max\":" + std::to_string(std::max(0, preset.max_players));
    result +=
        ",\"online\":" + std::to_string(std::max(0, preset.online_players)) +
        "}";
  }
  result += ",\"description\":" + description + "}";
  return result;
}

class Proxy {
public:
  Proxy(std::shared_ptr<Preset> active, DebugLog &log)
      : active_(std::move(active)), log_(log) {}
  ~Proxy() { stop(); }

  bool start(uint16_t listen_port, std::string &error) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == InvalidSocket) {
      error = std::strerror(errno);
      return false;
    }
    int reuse = 1;
    set_socket_option(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(listen_port);
    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0 ||
        listen(listen_fd_, 32) < 0) {
      error = std::strerror(errno);
      close_socket(listen_fd_);
      listen_fd_ = InvalidSocket;
      return false;
    }
    running_ = true;
    log_.add("Listening on 0.0.0.0:" + std::to_string(listen_port));
    accept_thread_ = std::thread([this] { accept_loop(); });
    return true;
  }

  void set_active(std::shared_ptr<Preset> preset) {
    std::lock_guard<std::mutex> lock(active_mutex_);
    active_ = std::move(preset);
  }

  void stop() {
    if (!running_.exchange(false))
      return;
    shutdown(listen_fd_, ShutdownBoth);
    close_socket(listen_fd_);
    if (accept_thread_.joinable())
      accept_thread_.join();
  }

private:
  std::shared_ptr<Preset> active_copy() {
    std::lock_guard<std::mutex> lock(active_mutex_);
    return active_;
  }

  static SocketHandle connect_target(const Preset &preset) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *results = nullptr;
    const auto port = std::to_string(preset.port);
    if (getaddrinfo(preset.host.c_str(), port.c_str(), &hints, &results) != 0)
      return InvalidSocket;
    SocketHandle target = InvalidSocket;
    for (addrinfo *item = results; item; item = item->ai_next) {
      target = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
      if (target == InvalidSocket)
        continue;
    #ifdef _WIN32
      u_long nonblocking = 1;
      ioctlsocket(target, FIONBIO, &nonblocking);
      int result = connect(target, item->ai_addr, static_cast<int>(item->ai_addrlen));
    #else
      const int flags = fcntl(target, F_GETFL, 0);
      fcntl(target, F_SETFL, flags | O_NONBLOCK);
      int result = connect(target, item->ai_addr, item->ai_addrlen);
    #endif
      if (result < 0 && socket_error() ==
    #ifdef _WIN32
          WSAEINPROGRESS
    #else
          EINPROGRESS
    #endif
      ) {
        PollDescriptor wait_socket{target, POLLOUT, 0};
        if (poll_sockets(&wait_socket, 1, 5000) > 0) {
          int connection_error = 0;
      #ifdef _WIN32
          int error_size = sizeof(connection_error);
      #else
          socklen_t error_size = sizeof(connection_error);
      #endif
          get_socket_option(target, SOL_SOCKET, SO_ERROR, &connection_error,
                            &error_size);
          if (connection_error == 0)
            result = 0;
        }
      }
      if (result == 0) {
#ifdef _WIN32
        nonblocking = 0;
        ioctlsocket(target, FIONBIO, &nonblocking);
#else
        fcntl(target, F_SETFL, flags);
#endif
        break;
      }
      close_socket(target);
      target = InvalidSocket;
    }
    freeaddrinfo(results);
    return target;
  }

  static bool send_all(SocketHandle socket, const std::vector<char> &bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
      const int written =
          send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
      if (written <= 0)
        return false;
      sent += static_cast<size_t>(written);
    }
    return true;
  }

  static bool receive_exact(SocketHandle socket, char *buffer, size_t size) {
    size_t received = 0;
    while (received < size) {
      const int count = recv(socket, buffer + received, static_cast<int>(size - received), 0);
      if (count <= 0)
        return false;
      received += static_cast<size_t>(count);
    }
    return true;
  }

  static bool receive_packet(SocketHandle socket, std::vector<char> &packet) {
    packet.clear();
    int32_t length = 0;
    int shift = 0;
    while (shift < 35) {
      char byte = 0;
      if (!receive_exact(socket, &byte, 1))
        return false;
      packet.push_back(byte);
      const unsigned char unsigned_byte = static_cast<unsigned char>(byte);
      length |= static_cast<int32_t>(unsigned_byte & 0x7f) << shift;
      if ((unsigned_byte & 0x80) == 0)
        break;
      shift += 7;
    }
    if (length < 0 || length > 2 * 1024 * 1024)
      return false;
    const size_t header_size = packet.size();
    packet.resize(header_size + static_cast<size_t>(length));
    return receive_exact(socket, packet.data() + header_size,
                         static_cast<size_t>(length));
  }

  static bool packet_payload(const std::vector<char> &packet,
                             int32_t &packet_id, std::vector<char> &payload) {
    size_t offset = 0;
    int32_t frame_length = 0;
    if (!read_varint(packet, offset, frame_length) || frame_length < 0 ||
        packet.size() < offset + static_cast<size_t>(frame_length))
      return false;
    const size_t frame_end = offset + static_cast<size_t>(frame_length);
    if (!read_varint(packet, offset, packet_id))
      return false;
    if (offset > frame_end || frame_end > packet.size())
      return false;
    payload.assign(packet.begin() + static_cast<std::ptrdiff_t>(offset),
                   packet.begin() + static_cast<std::ptrdiff_t>(frame_end));
    return true;
  }

  static std::vector<char> make_packet(const std::vector<char> &body) {
    std::vector<char> packet;
    append_varint(packet, static_cast<int32_t>(body.size()));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
  }

  static bool rewrite_handshake_target(const std::vector<char> &packet,
                                       const Preset &preset,
                                       std::vector<char> &rewritten) {
    size_t offset = 0;
    int32_t frame_length = 0;
    if (!read_varint(packet, offset, frame_length) || frame_length < 0 ||
        packet.size() < offset + static_cast<size_t>(frame_length))
      return false;
    const size_t frame_end = offset + static_cast<size_t>(frame_length);
    int32_t packet_id = 0, protocol = 0, host_length = 0, next_state = 0;
    if (!read_varint(packet, offset, packet_id) || packet_id != 0 ||
        !read_varint(packet, offset, protocol) ||
        !read_varint(packet, offset, host_length) || host_length < 0 ||
        offset + static_cast<size_t>(host_length) + 3 > frame_end)
      return false;
    offset += static_cast<size_t>(host_length);
    const size_t port_offset = offset;
    offset += 2;
    if (!read_varint(packet, offset, next_state) || offset > frame_end)
      return false;

    std::vector<char> body;
    append_varint(body, packet_id);
    append_varint(body, protocol);
    append_varint(body, static_cast<int32_t>(preset.host.size()));
    body.insert(body.end(), preset.host.begin(), preset.host.end());
    body.insert(body.end(),
                packet.begin() + static_cast<std::ptrdiff_t>(port_offset),
                packet.begin() + static_cast<std::ptrdiff_t>(port_offset + 2));
    append_varint(body, next_state);
    rewritten = make_packet(body);
    return true;
  }

  static bool serve_status(SocketHandle client, const Preset &preset, DebugLog *log) {
    std::vector<char> request;
    std::vector<char> request_payload;
    int32_t packet_id = 0;
    if (!receive_packet(client, request) ||
        !packet_payload(request, packet_id, request_payload) || packet_id != 0)
      return false;

    const std::string json = status_json(preset);
    std::vector<char> response_body;
    append_varint(response_body, 0);
    append_varint(response_body, static_cast<int32_t>(json.size()));
    response_body.insert(response_body.end(), json.begin(), json.end());
    if (!send_all(client, make_packet(response_body)))
      return false;

    std::vector<char> ping;
    std::vector<char> ping_payload;
    if (!receive_packet(client, ping) ||
        !packet_payload(ping, packet_id, ping_payload) || packet_id != 1) {
      return false;
    }
    std::vector<char> pong_body;
    append_varint(pong_body, 1);
    pong_body.insert(pong_body.end(), ping_payload.begin(), ping_payload.end());
    if (!send_all(client, make_packet(pong_body)))
      return false;
    log->add("Served mocked server-list info for " + preset.name);
    return true;
  }

  static bool detect_status_handshake(const std::vector<char> &bytes) {
    size_t offset = 0;
    int32_t frame_length = 0;
    if (!read_varint(bytes, offset, frame_length) || frame_length < 0 ||
        bytes.size() < offset + static_cast<size_t>(frame_length))
      return false;
    const size_t frame_end = offset + static_cast<size_t>(frame_length);
    int32_t packet_id = 0, protocol = 0, address_length = 0, next_state = 0;
    if (!read_varint(bytes, offset, packet_id) || packet_id != 0 ||
        !read_varint(bytes, offset, protocol) ||
        !read_varint(bytes, offset, address_length) || address_length < 0 ||
        offset + static_cast<size_t>(address_length) + 2 > frame_end) {
      return false;
    }
    offset += static_cast<size_t>(address_length) + 2;
    return read_varint(bytes, offset, next_state) && next_state == 1;
  }

  static bool rewrite_status_response(std::vector<char> &pending,
                                      const Preset &preset,
                                      std::vector<char> &output) {
    size_t offset = 0;
    int32_t frame_length = 0;
    if (!read_varint(pending, offset, frame_length) || frame_length < 0)
      return false;
    if (pending.size() < offset + static_cast<size_t>(frame_length))
      return false;
    const size_t frame_end = offset + static_cast<size_t>(frame_length);
    int32_t packet_id = 0;
    if (!read_varint(pending, offset, packet_id))
      return false;
    if (packet_id != 0) {
      output.insert(output.end(), pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
      pending.erase(pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
      return true;
    }

    if (preset.motd.empty() && preset.subtitle.empty() &&
        preset.online_players < 0 && preset.max_players < 0) {
      output.insert(output.end(), pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
      pending.erase(pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
      return true;
    }

    const std::string json = status_json(preset);
    std::vector<char> body;
    append_varint(body, 0);
    append_varint(body, static_cast<int32_t>(json.size()));
    body.insert(body.end(), json.begin(), json.end());
    std::vector<char> replacement;
    append_varint(replacement, static_cast<int32_t>(body.size()));
    replacement.insert(replacement.end(), body.begin(), body.end());
    output.insert(output.end(), replacement.begin(), replacement.end());
    pending.erase(pending.begin(),
                  pending.begin() + static_cast<std::ptrdiff_t>(frame_end));
    return true;
  }

  static void bridge(SocketHandle client, std::shared_ptr<Preset> preset,
                     DebugLog *log) {
    log->add("Client connected; waiting for Minecraft handshake");
    std::vector<char> handshake;
    if (!receive_packet(client, handshake)) {
      log->add("Client disconnected before sending a valid handshake");
      close_socket(client);
      return;
    }

    if (detect_status_handshake(handshake)) {
      log->add("Server-list request received for " + preset->name);
      serve_status(client, *preset, log);
      shutdown(client, ShutdownBoth);
      close_socket(client);
      return;
    }

    log->add("Login request received; connecting to " + preset->host + ":" +
             std::to_string(preset->port));
    const SocketHandle target = connect_target(*preset);
    if (target == InvalidSocket) {
      log->add("Target unavailable: " + preset->name + " (" + preset->host +
               ":" + std::to_string(preset->port) + ")");
      close_socket(client);
      return;
    }
    log->add("Connected client to " + preset->name + " (" + preset->host + ":" +
             std::to_string(preset->port) + ")");
    std::vector<char> target_handshake;
    if (!rewrite_handshake_target(handshake, *preset, target_handshake) ||
        !send_all(target, target_handshake)) {
      log->add("Could not forward the Minecraft handshake to " + preset->name);
      close_socket(client);
      close_socket(target);
      return;
    }

    PollDescriptor sockets[] = {{client, ReadEvent, 0}, {target, ReadEvent, 0}};
    bool client_open = true;
    bool target_open = true;
    char buffer[8192];
    while (client_open || target_open) {
      sockets[0].events = client_open ? ReadEvent : 0;
      sockets[1].events = target_open ? ReadEvent : 0;
      if (poll_sockets(sockets, 2, -1) < 0) {
        if (socket_error() ==
#ifdef _WIN32
            WSAEINTR
#else
            EINTR
#endif
        )
          continue;
        break;
      }

      for (int direction = 0; direction < 2; ++direction) {
        if (!sockets[direction].revents)
          continue;
        const SocketHandle from = sockets[direction].fd;
        const SocketHandle to = sockets[1 - direction].fd;
        const int received = recv(from, buffer, sizeof(buffer), 0);
        if (received <= 0) {
          if (direction == 0)
            client_open = false;
          else
            target_open = false;
          shutdown(to, ShutdownWrite);
          continue;
        }
        std::vector<char> outgoing(buffer, buffer + received);
        if (!send_all(to, outgoing)) {
          client_open = false;
          target_open = false;
          break;
        }
      }
    }
    shutdown(client, ShutdownBoth);
    shutdown(target, ShutdownBoth);
    close_socket(client);
    close_socket(target);
  }

  void accept_loop() {
    while (running_) {
      const SocketHandle client = accept(listen_fd_, nullptr, nullptr);
      if (client == InvalidSocket) {
        if (running_)
          continue;
        break;
      }
      log_.add("Accepted client connection");
      std::thread(bridge, client, active_copy(), &log_).detach();
    }
  }

  std::shared_ptr<Preset> active_;
  std::mutex active_mutex_;
  DebugLog &log_;
  std::atomic<bool> running_{false};
  SocketHandle listen_fd_ = InvalidSocket;
  std::thread accept_thread_;
};

std::string config_path() {
  const char *home = std::getenv("HOME");
  if (!home) {
    return "presets.conf";
  }
  const std::string current = std::string(home) + "/.config/mcroute/presets.conf";
  const std::string legacy = std::string(home) + "/.config/macroute/presets.conf";
  return std::filesystem::exists(current) || !std::filesystem::exists(legacy)
             ? current
             : legacy;
}

void draw_ui(const PresetStore &store, int selected, bool running,
             uint16_t listen_port, const std::string &status,
             const DebugLog &log, bool debug_view, size_t log_offset) {
  erase();
  if (debug_view) {
    attron(A_BOLD | COLOR_PAIR(1));
    mvprintw(1, 2, "MCROUTE / DEBUG");
    attroff(A_BOLD | COLOR_PAIR(1));
    mvprintw(2, 2, "UP/DOWN scroll   L or ESC return");
    mvhline(3, 2, ACS_HLINE, COLS - 4);
    const auto entries =
        log.recent(static_cast<size_t>(std::max(1, LINES - 7)) + log_offset);
    const size_t first =
        entries.size() > log_offset + static_cast<size_t>(LINES - 7)
            ? entries.size() - log_offset - static_cast<size_t>(LINES - 7)
            : 0;
    int row = 5;
    for (size_t index = first; index < entries.size() && row < LINES - 1;
         ++index) {
      mvprintw(row++, 2, "%.*s", std::max(0, COLS - 4), entries[index].c_str());
    }
    refresh();
    return;
  }
  attron(A_BOLD | COLOR_PAIR(1));
  mvprintw(1, 2, "MCROUTE");
  attroff(A_BOLD | COLOR_PAIR(1));
  mvprintw(2, 2, "Minecraft TCP gateway  |  listen 0.0.0.0:%u  |  %s",
           listen_port, running ? "ONLINE" : "OFFLINE");
  mvhline(3, 2, ACS_HLINE, COLS - 4);
  mvprintw(5, 4, "PRESETS");
  for (size_t index = 0; index < store.all().size(); ++index) {
    const auto &preset = store.all()[index];
    if (static_cast<int>(index) == selected)
      attron(A_REVERSE);
    mvprintw(7 + static_cast<int>(index), 4, "  %-20s  %s:%u",
             preset.name.c_str(), preset.host.c_str(), preset.port);
    if (static_cast<int>(index) == selected)
      attroff(A_REVERSE);
  }
  mvprintw(LINES - 4, 2,
           "UP/DOWN select   ENTER activate   A add   I server info   D delete "
           "  L debug   Q quit");
  if (!status.empty())
    mvprintw(LINES - 2, 2, "%s", status.c_str());
  const std::string latest = log.latest();
  if (!latest.empty())
    mvprintw(LINES - 1, 2, "Last: %.*s", std::max(0, COLS - 8), latest.c_str());
  refresh();
}

bool edit_server_info(PresetStore &store, int selected) {
  auto &preset = store.all()[selected];
  echo();
  char title[256]{}, motd[256]{}, subtitle[256]{}, online_text[16]{},
      max_text[16]{};
  mvprintw(LINES - 6, 2, "Title (version label): ");
  getnstr(title, sizeof(title) - 1);
  mvprintw(LINES - 5, 2, "MOTD: ");
  getnstr(motd, sizeof(motd) - 1);
  mvprintw(LINES - 4, 2, "Subtitle: ");
  getnstr(subtitle, sizeof(subtitle) - 1);
  mvprintw(LINES - 3, 2, "Online players (-1 = target): ");
  getnstr(online_text, sizeof(online_text) - 1);
  mvprintw(LINES - 2, 2, "Max players (-1 = target): ");
  getnstr(max_text, sizeof(max_text) - 1);
  noecho();
  try {
    const int online = online_text[0] ? std::stoi(online_text) : -1;
    const int maximum = max_text[0] ? std::stoi(max_text) : -1;
    if (online < -1 || maximum < -1)
      return false;
    preset.server_title = title;
    preset.motd = motd;
    preset.subtitle = subtitle;
    preset.online_players = online;
    preset.max_players = maximum;
    return store.save();
  } catch (const std::exception &) {
    return false;
  }
}

bool add_preset(PresetStore &store) {
  echo();
  char name[64]{}, host[256]{}, port_text[16]{}, motd[256]{}, subtitle[256]{},
      online_text[16]{}, max_text[16]{};
  mvprintw(LINES - 9, 2, "Name: ");
  getnstr(name, sizeof(name) - 1);
  mvprintw(LINES - 8, 2, "Host: ");
  getnstr(host, sizeof(host) - 1);
  mvprintw(LINES - 7, 2, "Port: ");
  getnstr(port_text, sizeof(port_text) - 1);
  mvprintw(LINES - 6, 2, "MOTD (blank = preset name): ");
  getnstr(motd, sizeof(motd) - 1);
  mvprintw(LINES - 5, 2, "Subtitle (optional): ");
  getnstr(subtitle, sizeof(subtitle) - 1);
  mvprintw(LINES - 4, 2, "Online players (-1 = target): ");
  getnstr(online_text, sizeof(online_text) - 1);
  mvprintw(LINES - 3, 2, "Max players (-1 = target): ");
  getnstr(max_text, sizeof(max_text) - 1);
  noecho();
  try {
    const auto port = std::stoul(port_text);
    const int online = online_text[0] ? std::stoi(online_text) : -1;
    const int maximum = max_text[0] ? std::stoi(max_text) : -1;
    if (name[0] && host[0] && port > 0 && port <= 65535 && online >= -1 &&
        maximum >= -1) {
      store.all().push_back({name, host, static_cast<uint16_t>(port), motd,
                             subtitle, online, maximum, ""});
      return store.save();
    }
  } catch (const std::exception &) {
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
  WSADATA winsock_data{};
  if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
    std::cerr << "Could not initialize Windows sockets\n";
    return 1;
  }
#endif
  uint16_t listen_port = 25565;
  std::string path = config_path();
  std::string requested_preset;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if ((argument == "--listen" || argument == "--config" ||
         argument == "--preset") &&
        index + 1 < argc) {
      const std::string value = argv[++index];
      if (argument == "--listen")
        listen_port = static_cast<uint16_t>(std::stoul(value));
      if (argument == "--config")
        path = value;
      if (argument == "--preset")
        requested_preset = value;
    } else if (argument == "--help") {
      std::cout << "Usage: MCRoute [--listen PORT] [--config FILE] [--preset "
                   "NAME]\n";
    #ifdef _WIN32
      WSACleanup();
    #endif
      return 0;
    }
  }

  PresetStore store(path);
  store.load();
  int selected = store.active_index();
  for (size_t index = 0; index < store.all().size(); ++index) {
    if (store.all()[index].name == requested_preset)
      selected = static_cast<int>(index);
  }
  auto active = std::make_shared<Preset>(store.all()[selected]);
  DebugLog log;
  Proxy proxy(active, log);
  std::string error;
  if (!proxy.start(listen_port, error)) {
    std::cerr << "Could not listen on port " << listen_port << ": " << error
              << '\n';
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  }

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  start_color();
  use_default_colors();
  init_pair(1, COLOR_CYAN, -1);
  std::string status = "Active target: " + active->name;
  bool quit = false;
  bool debug_view = false;
  size_t log_offset = 0;
  while (!quit) {
    draw_ui(store, selected, true, listen_port, status, log, debug_view,
            log_offset);
    const int key = getch();
    if (key == 'q' || key == 'Q')
      quit = true;
    else if (key == 'l' || key == 'L' || key == 27) {
      debug_view = !debug_view;
      log_offset = 0;
    } else if (debug_view && key == KEY_UP) {
      ++log_offset;
    } else if (debug_view && key == KEY_DOWN && log_offset > 0) {
      --log_offset;
    } else if (!debug_view && key == KEY_UP && selected > 0)
      --selected;
    else if (!debug_view && key == KEY_DOWN &&
             selected + 1 < static_cast<int>(store.all().size()))
      ++selected;
    else if (!debug_view && (key == 'i' || key == 'I')) {
      status = edit_server_info(store, selected) ? "Server info saved."
                                                 : "Invalid server info.";
    } else if (key == '\n' || key == KEY_ENTER) {
      active = std::make_shared<Preset>(store.all()[selected]);
      proxy.set_active(active);
      store.set_active_name(active->name);
      store.save();
      status = "Active target: " + active->name + " (new connections)";
    } else if (key == 'a' || key == 'A') {
      status = add_preset(store) ? "Preset added."
                                 : "Invalid preset; nothing added.";
    } else if ((key == 'd' || key == 'D') && store.all().size() > 1) {
      const bool was_active = store.all()[selected].name == active->name;
      store.all().erase(store.all().begin() + selected);
      selected = std::min(selected, static_cast<int>(store.all().size()) - 1);
      store.save();
      if (was_active) {
        active = std::make_shared<Preset>(store.all()[selected]);
        proxy.set_active(active);
        store.set_active_name(active->name);
      }
      status = "Preset deleted.";
    }
  }
  endwin();
  proxy.stop();
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
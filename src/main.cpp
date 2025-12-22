#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <X11/extensions/randr.h>
#include <algorithm>
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <print>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define GL_SILENCE_DEPRECATION
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <GLES2/gl2.h>
#endif
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

static void glfw_error_callback(int error, const char *description) {
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

void limitFPS(int targetFPS) {
  static auto lastFrameTime = std::chrono::steady_clock::now();
  auto frameDuration = std::chrono::microseconds(1000000 / targetFPS);
  auto currentTime = std::chrono::steady_clock::now();
  auto elapsed = currentTime - lastFrameTime;
  if (elapsed < frameDuration) {
    std::this_thread::sleep_for(frameDuration - elapsed);
  }
  lastFrameTime = std::chrono::steady_clock::now();
}

constexpr int PORT = 10829;
constexpr int MAX_CONNECTIONS = 20;
constexpr size_t BUFFER_SIZE = 2048;

void set_to_non_blocking(int socket_fd) {
  int flags = fcntl(socket_fd, F_GETFL, 0);
  if (flags < 0) {
    perror("fcntl");
    throw std::runtime_error("Getting socket flags failed");
  }
  if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("fcntl");
    throw std::runtime_error("Setting non-blocking mode failed");
  }
}

class TcpStream {
  int socket_fd;
  sockaddr peer_addr;

public:
  TcpStream(int socket_fd, sockaddr_in addr)
      : socket_fd(socket_fd), peer_addr(*(sockaddr *)&addr) {}

  TcpStream(sockaddr_in addr) : peer_addr(*(sockaddr *)&addr) {
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
      perror("socket");
      throw std::runtime_error("Socket creation failed");
    }
    timeval timeout;
    timeout.tv_sec = 5; // Set timeout to 5 seconds
    timeout.tv_usec = 0;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
      perror("setsockopt");
      close(socket_fd);
      throw std::runtime_error("Setting socket timeout failed");
    }
    if (connect(socket_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
      perror("connect");
      close(socket_fd);
      throw std::runtime_error("Connection failed");
    }
    set_to_non_blocking(socket_fd);
  }
  TcpStream(const TcpStream &) = delete;
  TcpStream &operator=(const TcpStream &) = delete;
  TcpStream(TcpStream &&other) noexcept
      : socket_fd(other.socket_fd), peer_addr(other.peer_addr) {
    other.socket_fd = -1;
  }
  TcpStream &operator=(TcpStream &&other) noexcept {
    if (this != &other) {
      close(socket_fd);
      socket_fd = other.socket_fd;
      peer_addr = other.peer_addr;
      other.socket_fd = -1;
    }
    return *this;
  }

  ~TcpStream() { close(socket_fd); }

  std::vector<char> read(size_t size) {

    std::vector<char> buffer(size);
    ssize_t bytesRead = recv(socket_fd, buffer.data(), size, 0);
    if (bytesRead < 0 && errno != EWOULDBLOCK) {
      perror("recv");
      throw std::runtime_error("Read failed");
    }
    if (bytesRead == 0) {
      throw std::runtime_error("Connection closed by peer");
    }
    bytesRead = std::max(bytesRead, 0l);
    buffer.resize(bytesRead);
    return buffer;
  }
  void write(const std::vector<char> &data) {
    ssize_t total = data.size();
    ssize_t offset = 0;
    ssize_t bytesSent = send(socket_fd, data.data(), data.size(), 0);
    while (bytesSent < total) {
      if (bytesSent < 0 && errno != EWOULDBLOCK) {
        perror("send");
        throw std::runtime_error("Write failed");
      }

      offset += bytesSent;
      bytesSent =
          send(socket_fd, data.data() + offset, data.size() - offset, 0);
    }
  }

  friend TcpStream &operator<<(TcpStream &stream, const std::string &data) {
    stream.write(std::vector<char>(data.begin(), data.end()));
    return stream;
  }

  friend TcpStream &operator>>(TcpStream &stream, std::string &data) {
    auto buffer = stream.read(BUFFER_SIZE);
    data = std::string(buffer.begin(), buffer.end());
    return stream;
  }

  friend TcpStream &operator<<(TcpStream &stream, uint64_t number) {
    uint64_t net_number = htobe64(number);
    stream.write(std::vector<char>(reinterpret_cast<char *>(&net_number),
                                   reinterpret_cast<char *>(&net_number) +
                                       sizeof(net_number)));
    return stream;
  }

  friend TcpStream &operator>>(TcpStream &stream, uint64_t &number) {
    auto buffer = stream.read(sizeof(uint64_t));
    if (buffer.size() != sizeof(uint64_t)) {
      throw std::runtime_error("Failed to read uint64_t");
    }
    uint64_t net_number;
    std::memcpy(&net_number, buffer.data(), sizeof(uint64_t));
    number = be64toh(net_number);
    return stream;
  }

  auto get_peer_addr() const { return peer_addr; }
};

class TcpListener {
  int socket_fd;

public:
  TcpListener() {
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
      throw std::runtime_error("Socket creation failed");
    }
    int opt = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
        0) {
      perror("setsockopt");
      close(socket_fd);
      throw std::runtime_error("Setting socket options failed");
    }
    sockaddr_in addr{
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr = {.s_addr = INADDR_ANY},
    };
    if (::bind(socket_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
      close(socket_fd);
      perror("bind");
      throw std::runtime_error("Binding failed");
    }
    if (listen(socket_fd, MAX_CONNECTIONS) < 0) {
      close(socket_fd);
      perror("listen");
      throw std::runtime_error("Listening failed");
    }
  }

  ~TcpListener() { close(socket_fd); }
  TcpListener(const TcpListener &) = delete;
  TcpListener &operator=(const TcpListener &) = delete;
  TcpListener(TcpListener &&other) noexcept : socket_fd(other.socket_fd) {
    other.socket_fd = -1;
  }
  TcpListener &operator=(TcpListener &&other) noexcept {
    if (this != &other) {
      close(socket_fd);
      socket_fd = other.socket_fd;
      other.socket_fd = -1;
    }
    return *this;
  }
  TcpStream accept() {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_socket =
        ::accept(socket_fd, (sockaddr *)&client_addr, &client_len);
    if (client_socket < 0) {
      perror("accept");
      throw std::runtime_error("Accepting connection failed");
    }
    return TcpStream(client_socket, client_addr);
  }
};

struct Connections {
  uint32_t client_id;
  sockaddr_in addr;
  std::string get_ip() const { return std::string(inet_ntoa(addr.sin_addr)); }
  std::string get_port() const { return std::to_string(ntohs(addr.sin_port)); }
};

std::vector<char> as_bytes(const std::string &str) {
  return std::vector<char>(str.begin(), str.end());
}

enum class MessageType : uint8_t {
  TIME_REQUEST = 0x01,
  NAME_REQUEST = 0x02,
  ACTIVE_CONNECTIONS_REQUEST = 0x03,
  SEND_MESSAGE = 0x04,
  TIME_RESPONSE = 0x81,
  NAME_RESPONSE = 0x82,
  ACTIVE_CONNECTIONS_RESPONSE = 0x83,
  FORWARD_MESSAGE = 0x84
};

struct ReceivedMessage {
  MessageType type;
  uint8_t flags;
  std::vector<char> payload;
};

std::optional<ReceivedMessage> try_parse_message(std::vector<char> &data) {
  if (data.size() < 4) {
    return std::nullopt;
  }
  ReceivedMessage message;
  message.type = static_cast<MessageType>(data[0]);
  if (data[0] != static_cast<char>(message.type)) {
    // 别读了，整个流都脏了
    data.clear();
    return std::nullopt;
  }
  message.flags = data[1];
  uint16_t payload_length =
      (static_cast<uint8_t>(data[2]) << 8) | static_cast<uint8_t>(data[3]);
  if (data.size() < 4 + payload_length) {
    return std::nullopt;
  }
  message.payload =
      std::vector<char>(data.begin() + 4, data.begin() + 4 + payload_length);
  data.erase(data.begin(), data.begin() + 4 + payload_length);
  return message;
}

std::string interpret_message(const ReceivedMessage &message) {
  switch (message.type) {
  case MessageType::TIME_RESPONSE:
    return "Server Time Stamp: " +
           std::to_string(static_cast<uint64_t>(*message.payload.begin()));
  case MessageType::NAME_RESPONSE: {
    uint16_t name_length = be16toh(
        static_cast<uint16_t>((static_cast<uint8_t>(message.payload[0]) << 8) |
                              static_cast<uint8_t>(message.payload[1])));
    return "Server Name: " +
           std::string(message.payload.begin() + 2,
                       message.payload.begin() + 2 + name_length);
  }
  case MessageType::ACTIVE_CONNECTIONS_RESPONSE: {
    uint16_t conn_count = be16toh(
        static_cast<uint16_t>((static_cast<uint8_t>(message.payload[0]) << 8) |
                              static_cast<uint8_t>(message.payload[1])));
    size_t off = 2;
    int i = 0;

    std::stringstream ss;
    while (i < conn_count) {
      uint32_t id = be32toh(static_cast<uint32_t>(
          (static_cast<uint8_t>(message.payload[off]) << 24) |
          (static_cast<uint8_t>(message.payload[off + 1]) << 16) |
          (static_cast<uint8_t>(message.payload[off + 2]) << 8) |
          static_cast<uint8_t>(message.payload[off + 3])));
      off += 4;
      uint32_t ip = be32toh(static_cast<uint32_t>(
          (static_cast<uint8_t>(message.payload[off]) << 24) |
          (static_cast<uint8_t>(message.payload[off + 1]) << 16) |
          (static_cast<uint8_t>(message.payload[off + 2]) << 8) |
          static_cast<uint8_t>(message.payload[off + 3])));
      off += 4;
      uint16_t port = be16toh(static_cast<uint16_t>(
          (static_cast<uint8_t>(message.payload[off]) << 8) |
          static_cast<uint8_t>(message.payload[off + 1])));
      off += 2;
      std::println(ss, "Connection {}: {}:{}", id, inet_ntoa(*(in_addr *)&ip),
                   port);
      i++;
    }
    return ss.str();
  }
  case MessageType::FORWARD_MESSAGE: {
    uint32_t sender_id = be32toh(
        static_cast<uint32_t>((static_cast<uint8_t>(message.payload[0]) << 24) |
                              (static_cast<uint8_t>(message.payload[1]) << 16) |
                              (static_cast<uint8_t>(message.payload[2]) << 8) |
                              static_cast<uint8_t>(message.payload[3])));
    uint16_t msg_length = be16toh(
        static_cast<uint16_t>((static_cast<uint8_t>(message.payload[4]) << 8) |
                              static_cast<uint8_t>(message.payload[5])));
    std::string msg_content(message.payload.begin() + 6,
                            message.payload.begin() + 6 + msg_length);
    return std::format("Message from {}: {}", sender_id, msg_content);
  }
  default: {
    return "Unknown message type";
  }
  }
}

// Main code
int main(int, char **) {
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
  // GL ES 2.0 + GLSL 100 (WebGL 1.0)
  const char *glsl_version = "#version 100";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(IMGUI_IMPL_OPENGL_ES3)
  // GL ES 3.0 + GLSL 300 es (WebGL 2.0)
  const char *glsl_version = "#version 300 es";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
  // GL 3.2 + GLSL 150
  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); // 3.2+ only
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // Required on Mac
#else
  // GL 3.0 + GLSL 130
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  // glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+
  // only glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE); // 3.0+ only
#endif

  // Create window with graphics context
  float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(
      glfwGetPrimaryMonitor()); // Valid on GLFW 3.3+ only
  GLFWwindow *window = glfwCreateWindow(
      (int)(1280 * main_scale), (int)(800 * main_scale),
      "Lab6 Computer Network Client & Server", nullptr, nullptr);
  if (window == nullptr)
    return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.Fonts->AddFontFromFileTTF("../assets/font.ttf", 24.0);
  io.MouseDrawCursor = true;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  // io.ConfigViewportsNoAutoMerge = true;
  // io.ConfigViewportsNoTaskBarIcon = true;

  ImGui::StyleColorsDark();
  // ImGui::StyleColorsLight();

  ImGuiStyle &style = ImGui::GetStyle();
  style.ScaleAllSizes(
      main_scale); // Bake a fixed style scale. (until we have a solution for
                   // dynamic style scaling, changing this requires resetting
                   // Style + calling this again)
  style.FontScaleDpi =
      main_scale; // Set initial font scale. (using io.ConfigDpiScaleFonts=true
                  // makes this unnecessary. We leave both here for
                  // documentation purpose)
#if GLFW_VERSION_MAJOR >= 3 && GLFW_VERSION_MINOR >= 3
  io.ConfigDpiScaleFonts =
      true; // [Experimental] Automatically overwrite style.FontScaleDpi in
            // Begin() when Monitor DPI changes. This will scale fonts but _NOT_
            // scale sizes/padding for now.
  io.ConfigDpiScaleViewports =
      true; // [Experimental] Scale Dear ImGui and Platform Windows when Monitor
            // DPI changes.
#endif

  // When viewports are enabled we tweak WindowRounding/WindowBg so platform
  // windows can look identical to regular ones.
  if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  }

  // Setup Platform/Renderer backends
  ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif
  ImGui_ImplOpenGL3_Init(glsl_version);

  // Our state
  bool show_demo_window = true;
  bool show_another_window = false;
  ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

  // Main loop
#ifdef __EMSCRIPTEN__
  // For an Emscripten build we are disabling file-system access, so let's not
  // attempt to do a fopen() of the imgui.ini file. You may manually call
  // LoadIniSettingsFromMemory() to load settings from your own storage.
  io.IniFilename = nullptr;
  EMSCRIPTEN_MAINLOOP_BEGIN
#else
  while (!glfwWindowShouldClose(window))
#endif
  {
    // Poll and handle events (inputs, window resize, etc.)
    // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to
    // tell if dear imgui wants to use your inputs.
    // - When io.WantCaptureMouse is true, do not dispatch mouse input data to
    // your main application, or clear/overwrite your copy of the mouse data.
    // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input
    // data to your main application, or clear/overwrite your copy of the
    // keyboard data. Generally you may always pass all inputs to dear imgui,
    // and hide them from your application based on those two flags.
    glfwPollEvents();
    if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
      ImGui_ImplGlfw_Sleep(10);
      continue;
    }

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // 1. Show the big demo window (Most of the sample code is in
    // ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear
    // ImGui!).
    // if (show_demo_window)
    //   ImGui::ShowDemoWindow(&show_demo_window);

    // 2. Show a simple window that we create ourselves. We use a Begin/End pair
    // to create a named window.
    // {
    //   static float f = 0.0f;
    //   static int counter = 0;

    //   ImGui::Begin("Hello, world!"); // Create a window called "Hello,
    //   world!"
    //                                  // and append into it.

    //   ImGui::Text("This is some useful text."); // Display some text (you can
    //                                             // use a format strings too)
    //   ImGui::Checkbox(
    //       "Demo Window",
    //       &show_demo_window); // Edit bools storing our window open/close
    //       state
    //   ImGui::Checkbox("Another Window", &show_another_window);

    //   ImGui::SliderFloat("float", &f, 0.0f,
    //                      1.0f); // Edit 1 float using a slider from 0.0f
    //                      to 1.0f
    //   ImGui::ColorEdit3(
    //       "clear color",
    //       (float *)&clear_color); // Edit 3 floats representing a color

    //   if (ImGui::Button("Button")) // Buttons return true when clicked (most
    //                                // widgets return true when
    //                                edited/activated)
    //     counter++;
    //   ImGui::SameLine();
    //   ImGui::Text("counter = %d", counter);

    //   ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
    //               1000.0f / io.Framerate, io.Framerate);
    //   ImGui::End();
    // }

    // 3. Show another simple window.
    // if (show_another_window) {
    //   ImGui::Begin(
    //       "Another Window",
    //       &show_another_window); // Pass a pointer to our bool variable (the
    //                              // window will have a closing button that
    //                              will
    //                              // clear the bool when clicked)
    //   ImGui::Text("Hello from another window!");
    //   if (ImGui::Button("Close Me"))
    //     show_another_window = false;
    //   ImGui::End();
    // }

    // Client UI
    {
      static std::unique_ptr<TcpStream> client;
      static std::vector<char> total_received;
      static std::vector<ReceivedMessage> parsed_messages;
      static std::vector<Connections> connections = {
          {.client_id = 1,
           .addr = {.sin_family = AF_INET,
                    .sin_port = htons(10829),
                    .sin_addr = {.s_addr = inet_addr("114.115.116.1")}}},
          {.client_id = 2,
           .addr = {.sin_family = AF_INET,
                    .sin_port = htons(10829),
                    .sin_addr = {.s_addr = inet_addr("114.115.116.2")}}},
          {.client_id = 3,
           .addr = {.sin_family = AF_INET,
                    .sin_port = htons(10829),
                    .sin_addr = {.s_addr = inet_addr("114.115.116.3")}}},
      };
      static int selected_conn_index = -1;
      ImGui::Begin("Client Panel");
      static char server_ip[64] = "127.0.0.1";
      static char server_port[8] = "10829";
      ImGui::InputText("Server IP", server_ip, sizeof(server_ip));
      ImGui::InputText("Server Port", server_port, sizeof(server_port));
      if (ImGui::Button("Connect")) {
        try {
          sockaddr_in server_addr{
              .sin_family = AF_INET,
              .sin_port = htons(static_cast<uint16_t>(atoi(server_port))),
              .sin_addr = {.s_addr = inet_addr(server_ip)},
          };
          client = std::make_unique<TcpStream>(server_addr);
          std::println(std::cout, "Connected to server {}:{}", server_ip,
                       server_port);
        } catch (const std::exception &e) {
          std::println(std::cerr, "Connection failed: {}\n", e.what());
          client.reset(nullptr);
        }
      }
      if (client != nullptr) {
        auto peer_addr = client->get_peer_addr();
        auto peer_ipv4 =
            inet_ntoa(reinterpret_cast<sockaddr_in *>(&peer_addr)->sin_addr);
        auto peer_port =
            ntohs(reinterpret_cast<sockaddr_in *>(&peer_addr)->sin_port);
        ImGui::Text("Connected to server %s:%d", peer_ipv4, peer_port);
      }
      ImGui::Separator();
      if (client != nullptr) {
        static char message[1024] = "Hello, Server!";
        ImGui::InputTextMultiline("Message", message, sizeof(message));
        if (ImGui::Button("Send")) {
          try {
            *client << std::string(message);
            std::println(std::cout, "Sent: {}", message);
          } catch (const std::exception &e) {
            client.reset(nullptr);
            std::println(std::cerr, "Send failed: {}\n", e.what());
          }
        }
        ImGui::Separator();

        try {
          auto bytes = client->read(BUFFER_SIZE);
          total_received.insert(total_received.end(), bytes.begin(),
                                bytes.end());
          auto packet = try_parse_message(total_received);
          if (packet.has_value()) {
            if (packet.value().type ==
                MessageType::ACTIVE_CONNECTIONS_RESPONSE) {
              connections.clear();
              uint16_t conn_count = be16toh(static_cast<uint16_t>(
                  (static_cast<uint8_t>(packet.value().payload[0]) << 8) |
                  static_cast<uint8_t>(packet.value().payload[1])));
              size_t off = 2;
              int i = 0;
              while (i < conn_count) {
                uint32_t id = be32toh(static_cast<uint32_t>(
                    (static_cast<uint8_t>(packet.value().payload[off]) << 24) |
                    (static_cast<uint8_t>(packet.value().payload[off + 1])
                     << 16) |
                    (static_cast<uint8_t>(packet.value().payload[off + 2])
                     << 8) |
                    static_cast<uint8_t>(packet.value().payload[off + 3])));
                off += 4;
                uint32_t ip = be32toh(static_cast<uint32_t>(
                    (static_cast<uint8_t>(packet.value().payload[off]) << 24) |
                    (static_cast<uint8_t>(packet.value().payload[off + 1])
                     << 16) |
                    (static_cast<uint8_t>(packet.value().payload[off + 2])
                     << 8) |
                    static_cast<uint8_t>(packet.value().payload[off + 3])));
                off += 4;
                uint16_t port = be16toh(static_cast<uint16_t>(
                    (static_cast<uint8_t>(packet.value().payload[off]) << 8) |
                    static_cast<uint8_t>(packet.value().payload[off + 1])));
                off += 2;
                Connections conn;
                conn.client_id = id;
                conn.addr.sin_family = AF_INET;
                conn.addr.sin_addr.s_addr = ip;
                conn.addr.sin_port = htons(port);
                connections.push_back(conn);
                i++;
              }
            }
            auto interpreted = interpret_message(packet.value());
            std::println(std::cout, "Received: {}", interpreted);
            parsed_messages.push_back(packet.value());
          }
        } catch (const std::exception &e) {
          client.reset(nullptr);
          std::println(std::cerr, "Receive failed: {}\n", e.what());
        }
      }
      ImGui::End();
      // ImGui::Begin("Server messages");
      // ImGui::TextWrapped("%s", total_received.data());
      // ImGui::End();
      ImGui::Begin("Action Panel");

      if (client != nullptr && ImGui::Button("Disconnect")) {
        client.reset(nullptr);
        std::println(std::cout, "Disconnected from server.");
      }

      if (client != nullptr && ImGui::Button("Get Time")) {
        std::vector<char> packet{static_cast<char>(MessageType::TIME_REQUEST),
                                 0x00, 0x00, 0x00};
        client->write(packet);
      }

      if (client != nullptr && ImGui::Button("Get Name")) {
        std::vector<char> packet{static_cast<char>(MessageType::NAME_REQUEST),
                                 0x00, 0x00, 0x00};
        client->write(packet);
      }

      ImGui::Separator();
      if (client != nullptr && ImGui::Button("Get Active Connections")) {
        std::vector<char> packet{
            static_cast<char>(MessageType::ACTIVE_CONNECTIONS_REQUEST), 0x00,
            0x00, 0x00};
        client->write(packet);
      }
      if (ImGui::BeginListBox("Active Connections")) {
        for (const auto &conn : connections) {
          if (ImGui::Selectable(std::format("[{}]{}:{}", conn.client_id,
                                            conn.get_ip(), conn.get_port())
                                    .c_str(),
                                selected_conn_index ==
                                    &conn - &connections[0])) {
            selected_conn_index = &conn - &connections[0];
          }
        }
        ImGui::EndListBox();
      }

      if (selected_conn_index >= 0 &&
          selected_conn_index < static_cast<int>(connections.size())) {
        static char fwd_message[1024] = "Hello from client!";
        ImGui::InputTextMultiline("Forward Message", fwd_message,
                                  sizeof(fwd_message));
        if (ImGui::Button("Send to Selected")) {
          const auto &conn = connections[selected_conn_index];
          std::vector<char> payload;
          uint32_t net_id = htobe32(conn.client_id);
          payload.insert(payload.end(), reinterpret_cast<char *>(&net_id),
                         reinterpret_cast<char *>(&net_id) + sizeof(net_id));
          uint16_t msg_length =
              htobe16(static_cast<uint16_t>(std::strlen(fwd_message)));
          payload.insert(payload.end(), reinterpret_cast<char *>(&msg_length),
                         reinterpret_cast<char *>(&msg_length) +
                             sizeof(msg_length));
          payload.insert(payload.end(), fwd_message,
                         fwd_message + std::strlen(fwd_message));
          std::vector<char> packet;
          packet.push_back(static_cast<char>(MessageType::SEND_MESSAGE));
          packet.push_back(0x00);
          uint16_t payload_len = htobe16(static_cast<uint16_t>(payload.size()));
          packet.push_back(reinterpret_cast<char *>(&payload_len)[0]);
          packet.push_back(reinterpret_cast<char *>(&payload_len)[1]);
          packet.insert(packet.end(), payload.begin(), payload.end());
          client->write(packet);
        }
      }
      ImGui::Separator();
      if (ImGui::Button("Exit")) {
        client.reset(nullptr);
        glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
      ImGui::End();
    }

    // Rendering
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                 clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImDrawData *draw_data = ImGui::GetDrawData();
    ImGui_ImplOpenGL3_RenderDrawData(draw_data);

    // Update and Render additional Platform Windows
    // (Platform functions may change the current OpenGL context, so we
    // save/restore it to make it easier to paste this code elsewhere.
    //  For this specific demo app we could also call
    //  glfwMakeContextCurrent(window) directly)
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
      GLFWwindow *backup_current_context = glfwGetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      glfwMakeContextCurrent(backup_current_context);
    }

    glfwSwapBuffers(window);
    limitFPS(60);
  }
#ifdef __EMSCRIPTEN__
  EMSCRIPTEN_MAINLOOP_END;
#endif

  // Cleanup

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}

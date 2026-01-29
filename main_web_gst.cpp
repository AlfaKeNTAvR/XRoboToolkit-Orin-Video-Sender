#include <algorithm>
#include <cstring>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <signal.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "network_helper.hpp"

std::unique_ptr<TCPClient> sender_ptr;
std::unique_ptr<TCPServer> server_ptr;
std::unique_ptr<TCPServer> cmd_server_ptr;
std::mutex sender_mutex;
GMainLoop *loop = nullptr;
volatile sig_atomic_t stop_requested = 0;
bool send_enabled = false;   // Send as TCP client
bool listen_enabled = false; // Listen as TCP server
bool cmd_listen_enabled = false;
bool raw_h264_enabled = false;
bool len_le_enabled = false;
bool avc_stream_enabled = false;
bool zed_sbs_enabled = false;

static void signal_handler(int sig) {
  if (!stop_requested && loop) {
    stop_requested = 1;
    g_main_loop_quit(loop);
  }
}

void printErrorAndQuit(const std::string &error_msg) {
  std::cerr << "Error: " << error_msg << std::endl;
  if (loop) {
    g_main_loop_quit(loop);
  }
}

struct CameraRequestData {
  int width = 0;
  int height = 0;
  int fps = 0;
  int bitrate = 0;
  int enableMvHevc = 0;
  int renderMode = 0;
  int port = 0;
  std::string camera;
  std::string ip;
};

struct NetworkDataProtocol {
  std::string command;
  std::vector<uint8_t> data;
};

static int32_t readInt32LE(const std::vector<uint8_t> &data, size_t offset) {
  if (offset + 4 > data.size()) {
    throw std::out_of_range("Not enough data to read int32");
  }
  return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                              (data[offset + 2] << 16) |
                              (data[offset + 3] << 24));
}

static std::string readCompactString(const std::vector<uint8_t> &data,
                                     size_t &offset) {
  if (offset >= data.size()) {
    throw std::out_of_range("Not enough data to read string length");
  }
  uint8_t length = data[offset++];
  if (length == 0) {
    return std::string();
  }
  if (offset + length > data.size()) {
    throw std::out_of_range("Not enough data to read string content");
  }
  std::string result(reinterpret_cast<const char *>(&data[offset]), length);
  offset += length;
  return result;
}

static int32_t readInt32BE(const std::vector<uint8_t> &data, size_t offset) {
  if (offset + 4 > data.size()) {
    throw std::out_of_range("Not enough data to read int32");
  }
  return static_cast<int32_t>((data[offset] << 24) |
                              (data[offset + 1] << 16) |
                              (data[offset + 2] << 8) |
                              (data[offset + 3]));
}

static NetworkDataProtocol parseNetworkDataProtocol(
    const std::vector<uint8_t> &buffer) {
  if (buffer.size() < 8) {
    throw std::invalid_argument("Buffer too small for protocol");
  }

  // Format A: [cmd_len(le)][cmd][data_len(le)][data]
  auto tryFormatA = [&]() -> NetworkDataProtocol {
    size_t offset = 0;
    int32_t commandLength = readInt32LE(buffer, offset);
    offset += 4;
    if (commandLength < 0 ||
        offset + static_cast<size_t>(commandLength) > buffer.size()) {
      throw std::invalid_argument("Invalid command length");
    }
    std::string command;
    if (commandLength > 0) {
      command = std::string(reinterpret_cast<const char *>(&buffer[offset]),
                            commandLength);
      size_t nullPos = command.find('\0');
      if (nullPos != std::string::npos) {
        command = command.substr(0, nullPos);
      }
    }
    offset += commandLength;
    if (offset + 4 > buffer.size()) {
      throw std::invalid_argument("Buffer too small for data length");
    }
    int32_t dataLength = readInt32LE(buffer, offset);
    offset += 4;
    if (dataLength < 0 ||
        offset + static_cast<size_t>(dataLength) > buffer.size()) {
      throw std::invalid_argument("Invalid data length");
    }
    std::vector<uint8_t> data;
    if (dataLength > 0) {
      data.assign(buffer.begin() + offset,
                  buffer.begin() + offset + dataLength);
    }
    return NetworkDataProtocol{command, data};
  };

  // Format B: [total_len(be)][cmd_len(le)][cmd][data_len(le)][data]
  auto tryFormatB = [&]() -> NetworkDataProtocol {
    size_t offset = 0;
    int32_t totalLength = readInt32BE(buffer, offset);
    offset += 4;
    if (totalLength <= 0 ||
        static_cast<size_t>(totalLength) + 4 != buffer.size()) {
      throw std::invalid_argument("Invalid total length");
    }
    int32_t commandLength = readInt32LE(buffer, offset);
    offset += 4;
    if (commandLength < 0 ||
        offset + static_cast<size_t>(commandLength) > buffer.size()) {
      throw std::invalid_argument("Invalid command length");
    }
    std::string command;
    if (commandLength > 0) {
      command = std::string(reinterpret_cast<const char *>(&buffer[offset]),
                            commandLength);
      size_t nullPos = command.find('\0');
      if (nullPos != std::string::npos) {
        command = command.substr(0, nullPos);
      }
    }
    offset += commandLength;
    if (offset + 4 > buffer.size()) {
      throw std::invalid_argument("Buffer too small for data length");
    }
    int32_t dataLength = readInt32LE(buffer, offset);
    offset += 4;
    if (dataLength < 0 ||
        offset + static_cast<size_t>(dataLength) > buffer.size()) {
      throw std::invalid_argument("Invalid data length");
    }
    std::vector<uint8_t> data;
    if (dataLength > 0) {
      data.assign(buffer.begin() + offset,
                  buffer.begin() + offset + dataLength);
    }
    return NetworkDataProtocol{command, data};
  };

  try {
    return tryFormatA();
  } catch (...) {
    return tryFormatB();
  }
}

static CameraRequestData parseCameraRequest(
    const std::vector<uint8_t> &data) {
  if (data.size() < 10) {
    throw std::invalid_argument("Data too small for camera request");
  }
  size_t offset = 0;
  if (data[offset] != 0xCA || data[offset + 1] != 0xFE) {
    throw std::invalid_argument("Invalid magic bytes");
  }
  offset += 2;
  uint8_t version = data[offset++];
  if (version != 1) {
    throw std::invalid_argument("Unsupported protocol version");
  }
  CameraRequestData result;
  if (offset + 28 > data.size()) {
    throw std::invalid_argument("Data too small for integer fields");
  }
  result.width = readInt32LE(data, offset);
  result.height = readInt32LE(data, offset + 4);
  result.fps = readInt32LE(data, offset + 8);
  result.bitrate = readInt32LE(data, offset + 12);
  result.enableMvHevc = readInt32LE(data, offset + 16);
  result.renderMode = readInt32LE(data, offset + 20);
  result.port = readInt32LE(data, offset + 24);
  offset += 28;
  result.camera = readCompactString(data, offset);
  result.ip = readCompactString(data, offset);
  return result;
}

static std::string hexDump(const std::vector<uint8_t> &data, size_t max_len) {
  size_t n = std::min(max_len, data.size());
  std::string out;
  out.reserve(n * 3);
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = data[i];
    out.push_back(hex[(b >> 4) & 0xF]);
    out.push_back(hex[b & 0xF]);
    if (i + 1 < n) {
      out.push_back(' ');
    }
  }
  return out;
}

GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_ERROR;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const uint8_t *data = map.data;
    gsize size = map.size;
    if (data && size > 0) {
      try {
        if (send_enabled && sender_ptr && sender_ptr->isConnected()) {
          std::lock_guard<std::mutex> lock(sender_mutex);
          if (raw_h264_enabled) {
            sender_ptr->sendData(reinterpret_cast<const char *>(data),
                                 static_cast<uint32_t>(size));
          } else {
            std::vector<uint8_t> packet(4 + size);
            if (len_le_enabled) {
              packet[0] = (size)&0xFF;
              packet[1] = (size >> 8) & 0xFF;
              packet[2] = (size >> 16) & 0xFF;
              packet[3] = (size >> 24) & 0xFF;
            } else {
              packet[0] = (size >> 24) & 0xFF;
              packet[1] = (size >> 16) & 0xFF;
              packet[2] = (size >> 8) & 0xFF;
              packet[3] = (size)&0xFF;
            }
            std::copy(data, data + size, packet.begin() + 4);
            sender_ptr->sendData(packet);
          }
          std::cout << "Sent " << size << " bytes of H.264 data" << std::endl;
        } else if (listen_enabled && server_ptr &&
                   server_ptr->isClientConnected()) {
          if (raw_h264_enabled) {
            server_ptr->sendData(reinterpret_cast<const char *>(data),
                                 static_cast<uint32_t>(size));
          } else {
            std::vector<uint8_t> packet(4 + size);
            if (len_le_enabled) {
              packet[0] = (size)&0xFF;
              packet[1] = (size >> 8) & 0xFF;
              packet[2] = (size >> 16) & 0xFF;
              packet[3] = (size >> 24) & 0xFF;
            } else {
              packet[0] = (size >> 24) & 0xFF;
              packet[1] = (size >> 16) & 0xFF;
              packet[2] = (size >> 8) & 0xFF;
              packet[3] = (size)&0xFF;
            }
            std::copy(data, data + size, packet.begin() + 4);
            server_ptr->sendData(packet);
          }
          std::cout << "Sent " << size << " bytes of H.264 data" << std::endl;
        }
      } catch (const TCPException &e) {
        std::cerr << "Send error: " << e.what() << std::endl;
      } catch (const std::exception &e) {
        std::cerr << "Unexpected send error: " << e.what() << std::endl;
      }
    }

    gst_buffer_unmap(buffer, &map);
  }

  if (buffer) {
    GstClockTime timestamp = GST_BUFFER_PTS(buffer);
    std::cout << "Encoded frame at timestamp: "
              << GST_TIME_AS_MSECONDS(timestamp) << " ms" << std::endl;
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);
  g_unix_signal_add(
      SIGINT,
      [](gpointer user_data) -> gboolean {
        if (loop && !stop_requested) {
          stop_requested = 1;
          g_main_loop_quit(loop);
        }
        return G_SOURCE_REMOVE;
      },
      nullptr);

  bool preview_enabled = false;
  std::string server_ip = "127.0.0.1";
  int server_port = 12345;
  std::string listen_address;
  std::string cmd_listen_address;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--preview") {
      preview_enabled = true;
    } else if (arg == "--send") {
      send_enabled = true;
    } else if (arg == "--listen" && i + 1 < argc) {
      listen_enabled = true;
      listen_address = argv[++i];
    } else if (arg == "--cmd-listen" && i + 1 < argc) {
      cmd_listen_enabled = true;
      cmd_listen_address = argv[++i];
    } else if (arg == "--raw-h264") {
      raw_h264_enabled = true;
    } else if (arg == "--len-le") {
      len_le_enabled = true;
    } else if (arg == "--avc") {
      avc_stream_enabled = true;
    } else if (arg == "--zed-sbs") {
      zed_sbs_enabled = true;
    } else if (arg == "--server" && i + 1 < argc) {
      server_ip = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      server_port = std::stoi(argv[++i]);
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]\n";
      std::cout << "Options:\n";
      std::cout << "  --preview      Enable video preview\n";
      std::cout << "  --send         Enable sending encoded video over TCP\n";
      std::cout << "  --listen ADDR  Listen for TCP client on ip:port\n";
      std::cout << "  --cmd-listen ADDR  Listen for control commands on ip:port\n";
      std::cout << "  --raw-h264     Send raw Annex-B H.264 without length header\n";
      std::cout << "  --len-le       Use little-endian length prefix\n";
      std::cout << "  --avc          Output AVC stream format (length-prefixed NALs)\n";
      std::cout << "  --zed-sbs      Output 2560x720 side-by-side for ZEDMINI\n";
      std::cout << "  --server IP    Server IP address (default: 127.0.0.1)\n";
      std::cout << "  --port PORT    Server port (default: 12345)\n";
      std::cout << "  --help         Show this help message\n";
      return 0;
    }
  }

  if (!send_enabled && !listen_enabled && !cmd_listen_enabled) {
    std::cerr << "Error: --send, --listen, or --cmd-listen is required"
              << std::endl;
    return -1;
  }

  if (send_enabled) {
    try {
      sender_ptr =
          std::unique_ptr<TCPClient>(new TCPClient(server_ip, server_port));
      std::cout << "Attempting to connect to " << server_ip << ":"
                << server_port << std::endl;
      sender_ptr->connect();
    } catch (const TCPException &e) {
      std::cerr << "Failed to connect to server: " << e.what() << std::endl;
      return -1;
    }
  }

  if (listen_enabled) {
    try {
      server_ptr = std::unique_ptr<TCPServer>(new TCPServer(listen_address));
      server_ptr->setDataCallback([](const std::string &) {});
      server_ptr->setDisconnectCallback([]() {});
      server_ptr->start();
      std::cout << "Listening on " << listen_address << std::endl;
    } catch (const TCPException &e) {
      std::cerr << "Failed to start server: " << e.what() << std::endl;
      return -1;
    }
  }

  if (cmd_listen_enabled) {
    try {
      cmd_server_ptr =
          std::unique_ptr<TCPServer>(new TCPServer(cmd_listen_address));
      cmd_server_ptr->setDataCallback([](const std::string &data) {
        std::vector<uint8_t> buffer(data.begin(), data.end());
        try {
          NetworkDataProtocol pkt = parseNetworkDataProtocol(buffer);
          if (!pkt.command.empty()) {
            std::cout << "Received command: " << pkt.command << std::endl;
          } else {
            std::cout << "Received command payload (" << pkt.data.size()
                      << " bytes)" << std::endl;
          }

          if (pkt.command == "OPEN_CAMERA" && !pkt.data.empty()) {
            CameraRequestData req = parseCameraRequest(pkt.data);
            if (!req.ip.empty() && req.port > 0) {
              std::lock_guard<std::mutex> lock(sender_mutex);
              sender_ptr = std::unique_ptr<TCPClient>(
                  new TCPClient(req.ip, req.port));
              std::cout << "Attempting to connect to " << req.ip << ":"
                        << req.port << std::endl;
              sender_ptr->connect();
              send_enabled = true;
            }
          }
        } catch (const std::exception &e) {
          std::cout << "Failed to parse command payload: " << e.what()
                    << std::endl;
          std::cout << "Command raw bytes (" << buffer.size()
                    << "): " << hexDump(buffer, 64) << std::endl;
          try {
            CameraRequestData req = parseCameraRequest(buffer);
            std::cout << "Parsed raw camera request: " << req.camera << " "
                      << req.width << "x" << req.height << "@" << req.fps
                      << " bitrate=" << req.bitrate << " ip=" << req.ip
                      << " port=" << req.port << std::endl;
            if (!req.ip.empty() && req.port > 0) {
              std::lock_guard<std::mutex> lock(sender_mutex);
              sender_ptr = std::unique_ptr<TCPClient>(
                  new TCPClient(req.ip, req.port));
              std::cout << "Attempting to connect to " << req.ip << ":"
                        << req.port << std::endl;
              sender_ptr->connect();
              send_enabled = true;
            }
          } catch (const std::exception &e2) {
            std::cout << "Failed to parse raw camera request: " << e2.what()
                      << std::endl;
          }
        }
      });
      cmd_server_ptr->setDisconnectCallback([]() {});
      cmd_server_ptr->start();
      std::cout << "Command listen on " << cmd_listen_address << std::endl;
    } catch (const TCPException &e) {
      std::cerr << "Failed to start command server: " << e.what() << std::endl;
      return -1;
    }
  }

  std::string pipeline_desc;

  const char *stream_format = avc_stream_enabled ? "avc" : "byte-stream";
  const char *byte_stream_flag = avc_stream_enabled ? "false" : "true";
  if (zed_sbs_enabled) {
    pipeline_desc =
        "v4l2src device=/dev/video10 ! "
        "video/x-raw,width=640,height=480,framerate=30/1 ! "
        "videoconvert ! videoscale ! "
        "video/x-raw,width=1280,height=720 ! "
        "videorate ! video/x-raw,framerate=60/1 ! "
        "tee name=tsrc "
        "tsrc. ! queue ! comp. "
        "tsrc. ! queue ! comp. "
        "compositor name=comp sink_0::xpos=0 sink_1::xpos=1280 ! "
        "video/x-raw,width=2560,height=720,framerate=60/1 ! "
        "videoconvert ! "
        + std::string(preview_enabled ? "tee name=tout " : "") +
        (preview_enabled
             ? "tout. ! queue ! videoconvert ! autovideosink sync=false "
               "tout. ! queue ! "
             : "") +
        "videoconvert ! video/x-raw,format=I420 ! "
        "x264enc tune=zerolatency bitrate=4000 speed-preset=veryfast "
        "key-int-max=30 bframes=0 repeat-headers=1 aud=true byte-stream="
        + std::string(byte_stream_flag) + " ! "
        "h264parse config-interval=1 ! "
        "video/x-h264,profile=baseline,stream-format="
        + std::string(stream_format) + ",alignment=au ! "
        "appsink name=mysink emit-signals=true sync=false";
  } else if (preview_enabled) {
    pipeline_desc =
        "v4l2src device=/dev/video10 ! "
        "video/x-raw,width=640,height=480,framerate=30/1 ! "
        "videoconvert ! "
        "tee name=t "
        "t. ! queue ! "
        "x264enc tune=zerolatency bitrate=2000 speed-preset=veryfast "
        "key-int-max=30 bframes=0 repeat-headers=1 aud=true byte-stream="
        + std::string(byte_stream_flag) + " ! "
        "h264parse config-interval=1 ! "
        "video/x-h264,profile=baseline,stream-format="
        + std::string(stream_format) + ",alignment=au ! "
        "appsink name=mysink emit-signals=true sync=false "
        "t. ! queue ! videoconvert ! autovideosink sync=false";
  } else {
    pipeline_desc =
        "v4l2src device=/dev/video10 ! "
        "video/x-raw,width=640,height=480,framerate=30/1 ! "
        "videoconvert ! "
        "x264enc tune=zerolatency bitrate=2000 speed-preset=veryfast "
        "key-int-max=30 bframes=0 repeat-headers=1 aud=true byte-stream="
        + std::string(byte_stream_flag) + " ! "
        "h264parse config-interval=1 ! "
        "video/x-h264,profile=baseline,stream-format="
        + std::string(stream_format) + ",alignment=au ! "
        "appsink name=mysink emit-signals=true sync=false";
  }

  GError *error = nullptr;
  GstElement *pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
  if (!pipeline) {
    g_printerr("Failed to create pipeline: %s\n", error->message);
    g_clear_error(&error);
    return -1;
  }

  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");
  if (!sink) {
    g_printerr("Failed to get appsink element\n");
    gst_object_unref(pipeline);
    return -1;
  }

  g_signal_connect(sink, "new-sample", G_CALLBACK(on_new_sample), nullptr);

  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  g_print("Capturing and encoding... Press Ctrl+C to stop.\n");

  loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  g_print("\nStopping pipeline...\n");

  if (send_enabled && sender_ptr) {
    sender_ptr->disconnect();
    std::cout << "Disconnected from server" << std::endl;
  }
  if (listen_enabled && server_ptr) {
    server_ptr->stop();
  }
  if (cmd_listen_enabled && cmd_server_ptr) {
    cmd_server_ptr->stop();
  }

  gst_element_send_event(pipeline, gst_event_new_eos());
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  gst_object_unref(sink);
  g_main_loop_unref(loop);
  loop = nullptr;

  return 0;
}

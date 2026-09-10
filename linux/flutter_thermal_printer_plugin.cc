// FLUTTER_PLUGIN_IMPL is defined by CMake via target_compile_definitions.
// Do NOT redefine it here — that would cause a -Werror,-Wmacro-redefined error.
#include "flutter_thermal_printer_plugin.h"
#include "include/flutter_thermal_printer/flutter_thermal_printer_plugin.h"

#include <cups/cups.h>
#include <bluetooth/bluetooth.h>
#include <libusb-1.0/libusb.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using TransportDeadline = std::chrono::steady_clock::time_point;
static constexpr auto kTransportTimeout = std::chrono::seconds(30);

// ---------------------------------------------------------------------------
// GObject boilerplate
// ---------------------------------------------------------------------------

// Struct body — G_DECLARE_FINAL_TYPE in the header declared the opaque typedef.
struct _FlutterThermalPrinterPlugin {
  GObject parent_instance;
};

G_DEFINE_TYPE(FlutterThermalPrinterPlugin,
              flutter_thermal_printer_plugin,
              G_TYPE_OBJECT)

static void flutter_thermal_printer_plugin_dispose(GObject* object) {
  G_OBJECT_CLASS(flutter_thermal_printer_plugin_parent_class)->dispose(object);
}

static void flutter_thermal_printer_plugin_class_init(
    FlutterThermalPrinterPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = flutter_thermal_printer_plugin_dispose;
}

static void flutter_thermal_printer_plugin_init(
    FlutterThermalPrinterPlugin* self) {}

// ---------------------------------------------------------------------------
// CUPS helpers
// ---------------------------------------------------------------------------

// Returns an FlValue list of maps: [{name, vendorId, productId}, ...]
// Uses the CUPS queue name as both `name` and `vendorId` so the Dart
// side can identify the printer by address = vendorId = queue name.
static FlValue* cups_get_printers_list() {
  cups_dest_t* dests = nullptr;
  const int num_dests = cupsGetDests(&dests);

  FlValue* result = fl_value_new_list();

  for (int i = 0; i < num_dests; i++) {
    const char* queue_name = dests[i].name ? dests[i].name : "";

    FlValue* device = fl_value_new_map();
    fl_value_set_string_take(device, "name",
                             fl_value_new_string(queue_name));
    fl_value_set_string_take(device, "vendorId",
                             fl_value_new_string(queue_name));
    fl_value_set_string_take(device, "productId",
                             fl_value_new_string("CUPS"));

    fl_value_append_take(result, device);
  }

  cupsFreeDests(num_dests, dests);
  return result;
}

// Returns true if a CUPS queue with the given name exists.
static bool cups_printer_exists(const char* printer_name) {
  if (!printer_name || printer_name[0] == '\0') return false;
  cups_dest_t* dests = nullptr;
  const int num_dests = cupsGetDests(&dests);
  cups_dest_t* dest =
      cupsGetDest(printer_name, nullptr, num_dests, dests);
  const bool found = (dest != nullptr);
  cupsFreeDests(num_dests, dests);
  return found;
}

// Sends raw ESC/POS bytes using `lp -d <printer> -o raw <tmpfile>`.
//
// This mirrors the macOS implementation (tryPrintWithCups in Swift) and is
// critical for correctness: using the CUPS C API with CUPS_FORMAT_RAW
// ("application/octet-stream") still runs data through the printer driver
// filter chain, which corrupts ESC/POS raster image (GS v 0) commands.
// `lp -o raw` maps to "application/vnd.cups-raw" internally, which bypasses
// ALL driver filters and delivers bytes unmodified to the printer.
static bool lp_print_raw(const char* printer_name,
                         const uint8_t* data,
                         size_t data_len,
                         TransportDeadline deadline) {
  // Write data to a secure temp file.
  char tmp_path[] = "/tmp/flutter_thermal_XXXXXX";
  const int fd = mkstemp(tmp_path);
  if (fd < 0) return false;

  // Write all bytes, handling partial writes.
  size_t written = 0;
  while (written < data_len) {
    const ssize_t n =
        write(fd, data + written, data_len - written);
    if (n <= 0) {
      close(fd);
      unlink(tmp_path);
      return false;
    }
    written += static_cast<size_t>(n);
  }
  close(fd);

  // Fork and exec `lp -d <printer> -o raw <file>`.
  // Using execl avoids shell injection — printer_name is passed as a direct
  // argument, never interpolated into a shell command string.
  const pid_t pid = fork();
  if (pid < 0) {
    unlink(tmp_path);
    return false;
  }

  if (pid == 0) {
    // Child process: try standard lp locations.
    execl("/usr/bin/lp", "lp",
          "-d", printer_name, "-o", "raw", tmp_path,
          static_cast<char*>(nullptr));
    execl("/usr/local/bin/lp", "lp",
          "-d", printer_name, "-o", "raw", tmp_path,
          static_cast<char*>(nullptr));
    _exit(EXIT_FAILURE);  // execl failed.
  }

  // Parent: wait for child to finish without blocking beyond the operation deadline.
  int status = 0;
  while (true) {
    const pid_t wait_result = waitpid(pid, &status, WNOHANG);
    if (wait_result == pid) break;
    if (wait_result < 0) {
      unlink(tmp_path);
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGTERM);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
      }
      unlink(tmp_path);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  unlink(tmp_path);

  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Fallback: CUPS C API with CUPS_FORMAT_RAW.
// Used only when lp_print_raw fails (e.g. lp binary not found).
// NOTE: this may be filtered by the printer driver on some setups.
static bool cups_print_raw(const char* printer_name,
                           const uint8_t* data,
                           size_t data_len) {
  if (!printer_name || printer_name[0] == '\0' || !data || data_len == 0) {
    return false;
  }

  // CUPS_HTTP_DEFAULT (NULL) connects to the local CUPS daemon.
  const int job_id = cupsCreateJob(CUPS_HTTP_DEFAULT, printer_name,
                                   "ESC/POS Print", 0, nullptr);
  if (job_id <= 0) {
    return false;
  }

  const http_status_t status =
      cupsStartDocument(CUPS_HTTP_DEFAULT, printer_name, job_id,
                        "receipt.bin", CUPS_FORMAT_RAW, 1 /* last doc */);

  if (status != HTTP_STATUS_CONTINUE) {
    return false;
  }

  cupsWriteRequestData(CUPS_HTTP_DEFAULT,
                       reinterpret_cast<const char*>(data), data_len);
  cupsFinishDocument(CUPS_HTTP_DEFAULT, printer_name);
  return true;
}

// ---------------------------------------------------------------------------
// Utility: extract a string value from an FlValue map by key.
// Returns empty string if not found or not a string.
// ---------------------------------------------------------------------------
static std::string map_get_string(FlValue* map, const char* key) {
  if (!map || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) return {};
  FlValue* val = fl_value_lookup_string(map, key);
  if (!val || fl_value_get_type(val) != FL_VALUE_TYPE_STRING) return {};
  const char* str = fl_value_get_string(val);
  return str ? str : "";
}

static std::vector<uint8_t> bytes_from_value(FlValue* value) {
  std::vector<uint8_t> bytes;
  if (value == nullptr) return bytes;
  if (fl_value_get_type(value) == FL_VALUE_TYPE_UINT8_LIST) {
    const size_t length = fl_value_get_length(value);
    const auto* data = fl_value_get_uint8_list(value);
    bytes.assign(data, data + length);
    return bytes;
  }
  if (fl_value_get_type(value) == FL_VALUE_TYPE_LIST) {
    const size_t length = fl_value_get_length(value);
    bytes.reserve(length);
    for (size_t index = 0; index < length; ++index) {
      FlValue* item = fl_value_get_list_value(value, index);
      if (fl_value_get_type(item) == FL_VALUE_TYPE_INT) {
        bytes.push_back(static_cast<uint8_t>(fl_value_get_int(item)));
      }
    }
  }
  return bytes;
}

static void bluetooth_log(const std::string& message) {
  g_message("[FlutterThermalPrinterNative] platform=linux transport=bluetoothClassic %s", message.c_str());
}

static void usb_log(const std::string& message) {
  g_message("[FlutterThermalPrinterNative] platform=linux transport=usb %s", message.c_str());
}

static void cups_log(const std::string& message) {
  g_message("[FlutterThermalPrinterNative] platform=linux transport=cups %s", message.c_str());
}

static int remainingTransferTimeout(TransportDeadline deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
  if (remaining <= 0) return 0;
  return static_cast<int>(std::min<int64_t>(remaining, 10'000));
}

struct LibusbPrinterAddress {
  bool uses_serial = false;
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  std::string serial_number;
  uint8_t bus_number = 0;
  uint8_t device_address = 0;
  int interface_number = -1;
  uint8_t endpoint_address = 0;
};

static bool parse_libusb_printer_address(
    const std::string& address, LibusbPrinterAddress* result) {
  if (result == nullptr || address.rfind("libusb:", 0) != 0) return false;
  if (address.rfind("libusb:serial:", 0) == 0) {
    const std::string prefix = "libusb:serial:";
    const size_t vendor_end = address.find(':', prefix.size());
    const size_t product_end = vendor_end == std::string::npos
        ? std::string::npos : address.find(':', vendor_end + 1);
    if (vendor_end == std::string::npos || product_end == std::string::npos || product_end + 1 >= address.size()) {
      return false;
    }
    unsigned int vendor = 0;
    unsigned int product = 0;
    if (sscanf(address.substr(prefix.size(), vendor_end - prefix.size()).c_str(), "%x", &vendor) != 1 ||
        sscanf(address.substr(vendor_end + 1, product_end - vendor_end - 1).c_str(), "%x", &product) != 1 ||
        vendor > UINT16_MAX || product > UINT16_MAX) {
      return false;
    }
    result->uses_serial = true;
    result->vendor_id = static_cast<uint16_t>(vendor);
    result->product_id = static_cast<uint16_t>(product);
    result->serial_number = address.substr(product_end + 1);
    return !result->serial_number.empty();
  }
  if (address.rfind("libusb:port:", 0) != 0) return false;
  unsigned int bus = 0;
  unsigned int device = 0;
  unsigned int interface_number = 0;
  unsigned int endpoint = 0;
  if (sscanf(address.c_str(), "libusb:port:%u:%u:%u:%u", &bus, &device, &interface_number, &endpoint) != 4 ||
      bus > UINT8_MAX || device > UINT8_MAX || interface_number > INT_MAX || endpoint > UINT8_MAX) {
    return false;
  }
  result->bus_number = static_cast<uint8_t>(bus);
  result->device_address = static_cast<uint8_t>(device);
  result->interface_number = static_cast<int>(interface_number);
  result->endpoint_address = static_cast<uint8_t>(endpoint);
  return true;
}

static std::string libusb_device_serial(libusb_device* device, const libusb_device_descriptor& descriptor) {
  if (descriptor.iSerialNumber == 0) return {};
  libusb_device_handle* handle = nullptr;
  if (libusb_open(device, &handle) != 0 || handle == nullptr) return {};
  unsigned char buffer[256]{};
  const int length = libusb_get_string_descriptor_ascii(
      handle, descriptor.iSerialNumber, buffer, sizeof(buffer));
  libusb_close(handle);
  return length > 0 ? std::string(reinterpret_cast<char*>(buffer), length) : std::string();
}

static libusb_device* find_libusb_device(
    libusb_device** devices, ssize_t count, const LibusbPrinterAddress& address) {
  for (ssize_t index = 0; index < count; ++index) {
    auto* device = devices[index];
    if (address.uses_serial) {
      libusb_device_descriptor descriptor{};
      if (libusb_get_device_descriptor(device, &descriptor) != 0 ||
          descriptor.idVendor != address.vendor_id || descriptor.idProduct != address.product_id ||
          libusb_device_serial(device, descriptor) != address.serial_number) {
        continue;
      }
      return device;
    }
    if (libusb_get_bus_number(device) == address.bus_number &&
        libusb_get_device_address(device) == address.device_address) {
      return device;
    }
  }
  return nullptr;
}

static bool find_printer_endpoint(
    libusb_device* device, int* interface_number, uint8_t* endpoint_address) {
  libusb_config_descriptor* config = nullptr;
  if (libusb_get_active_config_descriptor(device, &config) != 0 || config == nullptr) return false;
  bool found = false;
  for (uint8_t interface_index = 0; interface_index < config->bNumInterfaces && !found; ++interface_index) {
    const auto& interface = config->interface[interface_index];
    for (int alternate_index = 0; alternate_index < interface.num_altsetting && !found; ++alternate_index) {
      const auto& alternate = interface.altsetting[alternate_index];
      if (alternate.bInterfaceClass != LIBUSB_CLASS_PRINTER) continue;
      for (uint8_t endpoint_index = 0; endpoint_index < alternate.bNumEndpoints; ++endpoint_index) {
        const auto& endpoint = alternate.endpoint[endpoint_index];
        if ((endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK &&
            (endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
          *interface_number = alternate.bInterfaceNumber;
          *endpoint_address = endpoint.bEndpointAddress;
          found = true;
          break;
        }
      }
    }
  }
  libusb_free_config_descriptor(config);
  return found;
}

static bool libusb_transfer_data(
    const std::string& address,
    const std::vector<uint8_t>* bytes,
    TransportDeadline deadline) {
  LibusbPrinterAddress parsed;
  if (!parse_libusb_printer_address(address, &parsed)) {
    usb_log("connection.failed reason=invalid_libusb_address address=" + address);
    return false;
  }

  libusb_context* context = nullptr;
  if (libusb_init(&context) != 0) return false;
  libusb_device** devices = nullptr;
  const ssize_t count = libusb_get_device_list(context, &devices);
  auto* device = find_libusb_device(devices, count, parsed);
  if (device != nullptr && parsed.uses_serial &&
      !find_printer_endpoint(device, &parsed.interface_number, &parsed.endpoint_address)) {
    device = nullptr;
  }
  libusb_device_handle* handle = nullptr;
  bool success = false;
  if (device != nullptr && libusb_open(device, &handle) == 0 && handle != nullptr) {
    libusb_set_auto_detach_kernel_driver(handle, 1);
    const int claim_result = libusb_claim_interface(handle, parsed.interface_number);
    if (claim_result == 0) {
      success = true;
      if (bytes != nullptr) {
        size_t offset = 0;
        while (offset < bytes->size()) {
          const int timeout = remainingTransferTimeout(deadline);
          if (timeout <= 0) {
            usb_log("print.timeout stage=bulk_transfer");
            success = false;
            break;
          }
          const int transfer_size = static_cast<int>(std::min<size_t>(16 * 1024, bytes->size() - offset));
          int transferred = 0;
          const int transfer_result = libusb_bulk_transfer(
              handle,
              parsed.endpoint_address,
              const_cast<unsigned char*>(bytes->data() + offset),
              transfer_size,
              &transferred,
              timeout);
          if (transfer_result != 0 || transferred <= 0) {
            usb_log("print.failed stage=bulk_transfer error=" +
                    std::string(libusb_error_name(transfer_result)));
            success = false;
            break;
          }
          offset += static_cast<size_t>(transferred);
        }
      }
      libusb_release_interface(handle, parsed.interface_number);
    } else {
      usb_log("connection.failed stage=claim_interface error=" +
              std::string(libusb_error_name(claim_result)));
    }
    libusb_close(handle);
  }
  if (!success && device == nullptr) usb_log("connection.failed reason=device_not_found address=" + address);
  if (count >= 0) libusb_free_device_list(devices, 1);
  libusb_exit(context);
  return success;
}

static bool is_usb_printer_device_path(const std::string& path) {
  constexpr char kUsbPrinterPrefix[] = "/dev/usb/lp";
  if (path.rfind(kUsbPrinterPrefix, 0) != 0 || path.size() == strlen(kUsbPrinterPrefix)) {
    return false;
  }
  return std::all_of(
      path.begin() + strlen(kUsbPrinterPrefix), path.end(),
      [](unsigned char character) { return std::isdigit(character) != 0; });
}

static bool can_access_usb_printer(const std::string& path, TransportDeadline deadline) {
  if (path.rfind("libusb:", 0) == 0) {
    const bool connected = libusb_transfer_data(path, nullptr, deadline);
    usb_log("connection.checked path=" + path + " connected=" + std::to_string(connected));
    return connected;
  }
  if (!is_usb_printer_device_path(path)) {
    usb_log("connection.failed reason=invalid_path path=" + path);
    return false;
  }
  const int file_descriptor = open(path.c_str(), O_WRONLY | O_NONBLOCK);
  if (file_descriptor < 0) {
    usb_log("connection.failed path=" + path + " errno=" + std::to_string(errno));
    return false;
  }
  close(file_descriptor);
  usb_log("connection.checked path=" + path + " connected=1");
  return true;
}

class ScopedFileDescriptor {
 public:
  explicit ScopedFileDescriptor(int descriptor) : descriptor_(descriptor) {}
  ~ScopedFileDescriptor() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  int get() const { return descriptor_; }
  bool valid() const { return descriptor_ >= 0; }

 private:
  int descriptor_;
};

static bool print_usb_printer(
    const std::string& path,
    const std::vector<uint8_t>& bytes,
    TransportDeadline deadline) {
  if (path.rfind("libusb:", 0) == 0) {
    const bool printed = libusb_transfer_data(path, &bytes, deadline);
    if (printed) usb_log("print.success path=" + path + " bytes=" + std::to_string(bytes.size()));
    return printed;
  }
  if (!can_access_usb_printer(path, deadline)) return false;
  if (bytes.empty()) return true;

  ScopedFileDescriptor file_descriptor(open(path.c_str(), O_WRONLY | O_NONBLOCK));
  if (!file_descriptor.valid()) {
    usb_log("print.failed stage=open path=" + path + " errno=" + std::to_string(errno));
    return false;
  }

  size_t offset = 0;
  while (offset < bytes.size()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      usb_log("print.timeout stage=write path=" + path);
      return false;
    }
    const ssize_t written = write(file_descriptor.get(), bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) {
      usb_log(
          "print.failed stage=write path=" + path + " offset=" + std::to_string(offset) +
          " errno=" + std::to_string(errno));
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  usb_log("print.success path=" + path + " bytes=" + std::to_string(bytes.size()));
  return true;
}

static bool bluetooth_address_from_string(const std::string& address, bdaddr_t* result) {
  if (result == nullptr || address.empty()) return false;
  return str2ba(address.c_str(), result) == 0;
}

// sdp_get_access_protos returns an outer list whose items are protocol
// descriptor lists. The descriptor data is owned by the SDP record, so only
// the list nodes themselves must be released.
static void free_protocol_lists(sdp_list_t* protocol_lists) {
  for (sdp_list_t* node = protocol_lists; node != nullptr; node = node->next) {
    sdp_list_free(static_cast<sdp_list_t*>(node->data), nullptr);
  }
  sdp_list_free(protocol_lists, nullptr);
}

static void free_service_records(sdp_list_t* records) {
  for (sdp_list_t* node = records; node != nullptr; node = node->next) {
    sdp_record_free(static_cast<sdp_record_t*>(node->data));
  }
  sdp_list_free(records, nullptr);
}

static int serial_port_channel(const bdaddr_t& target, TransportDeadline deadline) {
  char target_address[18]{};
  ba2str(&target, target_address);
  bluetooth_log("sdp.started address=" + std::string(target_address));

  uuid_t service_uuid{};
  sdp_uuid16_create(&service_uuid, SERIAL_PORT_SVCLASS_ID);
  sdp_list_t* search_list = sdp_list_append(nullptr, &service_uuid);
  uint32_t range = 0x0000ffff;
  sdp_list_t* attribute_list = sdp_list_append(nullptr, &range);
  sdp_list_t* response_list = nullptr;
  const bdaddr_t any = {{0, 0, 0, 0, 0, 0}};
  if (std::chrono::steady_clock::now() >= deadline) return -1;
  sdp_session_t* session = sdp_connect(&any, &target, SDP_RETRY_IF_BUSY);
  if (session == nullptr) {
    bluetooth_log("sdp.failed stage=connect errno=" + std::to_string(errno));
    sdp_list_free(search_list, nullptr);
    sdp_list_free(attribute_list, nullptr);
    return -1;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    sdp_close(session);
    sdp_list_free(search_list, nullptr);
    sdp_list_free(attribute_list, nullptr);
    bluetooth_log("sdp.timeout stage=connect address=" + std::string(target_address));
    return -1;
  }
  bluetooth_log("sdp.connected address=" + std::string(target_address));

  const int status = sdp_service_search_attr_req(
      session, search_list, SDP_ATTR_REQ_RANGE, attribute_list, &response_list);
  if (std::chrono::steady_clock::now() >= deadline) {
    free_service_records(response_list);
    sdp_list_free(search_list, nullptr);
    sdp_list_free(attribute_list, nullptr);
    sdp_close(session);
    bluetooth_log("sdp.timeout stage=search address=" + std::string(target_address));
    return -1;
  }
  int channel = -1;
  if (status == 0) {
    for (sdp_list_t* record_node = response_list; record_node != nullptr && channel < 0;
         record_node = record_node->next) {
      auto* record = static_cast<sdp_record_t*>(record_node->data);
      sdp_list_t* protocol_list = nullptr;
      if (sdp_get_access_protos(record, &protocol_list) == 0) {
        channel = sdp_get_proto_port(protocol_list, RFCOMM_UUID);
        free_protocol_lists(protocol_list);
      }
    }
    bluetooth_log(
        "sdp.completed address=" + std::string(target_address) +
        " channel=" + std::to_string(channel));
  } else {
    bluetooth_log("sdp.failed stage=search status=" + std::to_string(status));
  }

  free_service_records(response_list);
  sdp_list_free(search_list, nullptr);
  sdp_list_free(attribute_list, nullptr);
  sdp_close(session);
  return channel;
}

static bool connect_rfcomm_socket(
    int socket_fd,
    const sockaddr_rc& remote,
    TransportDeadline deadline) {
  const int previous_flags = fcntl(socket_fd, F_GETFL, 0);
  if (previous_flags < 0 || fcntl(socket_fd, F_SETFL, previous_flags | O_NONBLOCK) < 0) {
    bluetooth_log("connect.failed stage=nonblocking errno=" + std::to_string(errno));
    return false;
  }
  const int result = connect(socket_fd, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
  if (result == 0) {
    fcntl(socket_fd, F_SETFL, previous_flags);
    return true;
  }
  if (errno != EINPROGRESS) {
    bluetooth_log("connect.failed stage=connect errno=" + std::to_string(errno));
    fcntl(socket_fd, F_SETFL, previous_flags);
    return false;
  }

  fd_set write_set;
  FD_ZERO(&write_set);
  FD_SET(socket_fd, &write_set);
  const int timeout_ms = remainingTransferTimeout(deadline);
  if (timeout_ms <= 0) return false;
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  const int selected = select(socket_fd + 1, nullptr, &write_set, nullptr, &timeout);
  int socket_error = 0;
  socklen_t socket_error_size = sizeof(socket_error);
  const bool connected = selected == 1 &&
      getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 &&
      socket_error == 0;
  if (!connected) {
    bluetooth_log(
        "connect.failed stage=wait selected=" + std::to_string(selected) +
        " error=" + std::to_string(socket_error));
  }
  fcntl(socket_fd, F_SETFL, previous_flags);
  return connected;
}

static bool print_bluetooth_classic(
    const std::string& address,
    const std::vector<uint8_t>& bytes,
    TransportDeadline deadline) {
  if (bytes.empty()) return true;
  bdaddr_t target{};
  if (!bluetooth_address_from_string(address, &target)) {
    bluetooth_log("print.failed reason=invalid_address address=" + address);
    return false;
  }
  const int channel = serial_port_channel(target, deadline);
  if (channel <= 0) {
    bluetooth_log("print.failed reason=serial_port_profile_unavailable address=" + address);
    return false;
  }

  const int socket_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (socket_fd < 0) {
    bluetooth_log("print.failed stage=socket errno=" + std::to_string(errno));
    return false;
  }
  const int timeout_ms = remainingTransferTimeout(deadline);
  if (timeout_ms <= 0) {
    close(socket_fd);
    return false;
  }
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  sockaddr_rc remote{};
  remote.rc_family = AF_BLUETOOTH;
  remote.rc_bdaddr = target;
  remote.rc_channel = static_cast<uint8_t>(channel);
  bluetooth_log(
      "connect.started address=" + address + " channel=" + std::to_string(channel));
  if (!connect_rfcomm_socket(socket_fd, remote, deadline)) {
    close(socket_fd);
    return false;
  }

  constexpr size_t kChunkSize = 1'024;
  size_t offset = 0;
  while (offset < bytes.size()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      bluetooth_log("print.timeout stage=write address=" + address);
      close(socket_fd);
      return false;
    }
    const size_t size = std::min(kChunkSize, bytes.size() - offset);
    const ssize_t written = write(socket_fd, bytes.data() + offset, size);
    if (written <= 0) {
      bluetooth_log(
          "print.failed stage=write offset=" + std::to_string(offset) + " errno=" + std::to_string(errno));
      close(socket_fd);
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  close(socket_fd);
  bluetooth_log("print.success address=" + address + " bytes=" + std::to_string(bytes.size()));
  return true;
}

static bool can_connect_bluetooth_classic(
    const std::string& address,
    TransportDeadline deadline) {
  bdaddr_t target{};
  if (!bluetooth_address_from_string(address, &target)) return false;
  const int channel = serial_port_channel(target, deadline);
  if (channel <= 0) return false;

  const int socket_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (socket_fd < 0) return false;
  const int timeout_ms = remainingTransferTimeout(deadline);
  if (timeout_ms <= 0) {
    close(socket_fd);
    return false;
  }
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_rc remote{};
  remote.rc_family = AF_BLUETOOTH;
  remote.rc_bdaddr = target;
  remote.rc_channel = static_cast<uint8_t>(channel);
  const bool connected = connect_rfcomm_socket(socket_fd, remote, deadline);
  close(socket_fd);
  bluetooth_log(
      "connection.checked address=" + address + " channel=" + std::to_string(channel) +
      " connected=" + std::to_string(connected));
  return connected;
}

enum class BluetoothOperationKind { connect, print };

struct BluetoothOperation {
  FlMethodCall* method_call;
  BluetoothOperationKind kind;
  std::string address;
  std::vector<uint8_t> bytes;
  uint64_t operation_id;
  bool success = false;
  bool return_false_as_value = false;
};

static std::atomic<uint64_t> next_bluetooth_operation_id{0};

static gboolean complete_bluetooth_operation(gpointer user_data) {
  auto* operation = static_cast<BluetoothOperation*>(user_data);
  g_autoptr(FlMethodResponse) response = nullptr;
  if (operation->kind == BluetoothOperationKind::print && !operation->success) {
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "BLUETOOTH_WRITE_FAILED", "Unable to write data to the Bluetooth printer", nullptr));
  } else if (operation->kind == BluetoothOperationKind::connect &&
             !operation->success && !operation->return_false_as_value) {
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "BLUETOOTH_CONNECT_FAILED", "Unable to connect to the Bluetooth printer", nullptr));
  } else {
    g_autoptr(FlValue) result = fl_value_new_bool(operation->success ? TRUE : FALSE);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }
  fl_method_call_respond(operation->method_call, response, nullptr);
  g_object_unref(operation->method_call);
  delete operation;
  return G_SOURCE_REMOVE;
}

static gpointer run_bluetooth_operation(gpointer user_data) {
  auto* operation = static_cast<BluetoothOperation*>(user_data);
  const auto started_at = std::chrono::steady_clock::now();
  const auto deadline = started_at + kTransportTimeout;
  bluetooth_log(
      "operation.started id=" + std::to_string(operation->operation_id) + " kind=" +
      std::string(operation->kind == BluetoothOperationKind::print ? "print" : "connect") +
      " address=" + operation->address);
  operation->success = operation->kind == BluetoothOperationKind::print
      ? print_bluetooth_classic(operation->address, operation->bytes, deadline)
      : can_connect_bluetooth_classic(operation->address, deadline);
  const char* error_code = operation->success
      ? "SUCCESS"
      : (operation->return_false_as_value
             ? "NONE"
             : (operation->kind == BluetoothOperationKind::print
                    ? "BLUETOOTH_WRITE_FAILED"
                    : "BLUETOOTH_CONNECT_FAILED"));
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at).count();
  bluetooth_log(
      "operation.completed id=" + std::to_string(operation->operation_id) +
      " success=" + std::to_string(operation->success) +
      " errorCode=" + error_code +
      " durationMs=" + std::to_string(duration));
  g_main_context_invoke(nullptr, complete_bluetooth_operation, operation);
  return nullptr;
}

static void start_bluetooth_operation(BluetoothOperation* operation) {
  GThread* thread = g_thread_new("thermal-bluetooth", run_bluetooth_operation, operation);
  g_thread_unref(thread);
}

enum class UsbOperationKind { connect, print };

struct UsbOperation {
  FlMethodCall* method_call;
  UsbOperationKind kind;
  std::string path;
  std::vector<uint8_t> bytes;
  uint64_t operation_id;
  bool success = false;
  bool return_false_as_value = false;
};

static std::atomic<uint64_t> next_usb_operation_id{0};

static gboolean complete_usb_operation(gpointer user_data) {
  auto* operation = static_cast<UsbOperation*>(user_data);
  g_autoptr(FlMethodResponse) response = nullptr;
  if (operation->kind == UsbOperationKind::print && !operation->success) {
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "USB_WRITE_FAILED", "Unable to write data to the USB printer", nullptr));
  } else if (operation->kind == UsbOperationKind::connect &&
             !operation->success && !operation->return_false_as_value) {
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "USB_OPEN_FAILED", "Unable to open the USB printer", nullptr));
  } else {
    g_autoptr(FlValue) result = fl_value_new_bool(operation->success ? TRUE : FALSE);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }
  fl_method_call_respond(operation->method_call, response, nullptr);
  g_object_unref(operation->method_call);
  delete operation;
  return G_SOURCE_REMOVE;
}

static gpointer run_usb_operation(gpointer user_data) {
  auto* operation = static_cast<UsbOperation*>(user_data);
  const auto started_at = std::chrono::steady_clock::now();
  const auto deadline = started_at + kTransportTimeout;
  usb_log("operation.started id=" + std::to_string(operation->operation_id) +
          " kind=" + (operation->kind == UsbOperationKind::print ? "print" : "connect") +
          " path=" + operation->path);
  operation->success = operation->kind == UsbOperationKind::print
      ? print_usb_printer(operation->path, operation->bytes, deadline)
      : can_access_usb_printer(operation->path, deadline);
  const char* error_code = operation->success
      ? "SUCCESS"
      : (operation->return_false_as_value
             ? "NONE"
             : (operation->kind == UsbOperationKind::print
                    ? "USB_WRITE_FAILED"
                    : "USB_OPEN_FAILED"));
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at).count();
  usb_log("operation.completed id=" + std::to_string(operation->operation_id) +
          " success=" + std::to_string(operation->success) +
          " errorCode=" + error_code +
          " durationMs=" + std::to_string(duration));
  g_main_context_invoke(nullptr, complete_usb_operation, operation);
  return nullptr;
}

static void start_usb_operation(UsbOperation* operation) {
  GThread* thread = g_thread_new("thermal-usb", run_usb_operation, operation);
  g_thread_unref(thread);
}

struct CupsPrintOperation {
  FlMethodCall* method_call;
  std::string printer_name;
  std::vector<uint8_t> bytes;
  uint64_t operation_id;
  bool success = false;
};

static std::atomic<uint64_t> next_cups_operation_id{0};

static gboolean complete_cups_print_operation(gpointer user_data) {
  auto* operation = static_cast<CupsPrintOperation*>(user_data);
  g_autoptr(FlMethodResponse) response = nullptr;
  if (!operation->success) {
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "PRINT_ERROR", "Failed to submit CUPS print job", nullptr));
  } else {
    g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  }
  fl_method_call_respond(operation->method_call, response, nullptr);
  g_object_unref(operation->method_call);
  delete operation;
  return G_SOURCE_REMOVE;
}

static gpointer run_cups_print_operation(gpointer user_data) {
  auto* operation = static_cast<CupsPrintOperation*>(user_data);
  const auto started_at = std::chrono::steady_clock::now();
  const auto deadline = started_at + kTransportTimeout;
  cups_log("operation.started id=" + std::to_string(operation->operation_id) +
          " printer=" + operation->printer_name);
  operation->success = lp_print_raw(
      operation->printer_name.c_str(), operation->bytes.data(), operation->bytes.size(), deadline);
  if (!operation->success && std::chrono::steady_clock::now() < deadline) {
    operation->success = cups_print_raw(
        operation->printer_name.c_str(), operation->bytes.data(), operation->bytes.size());
  }
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at).count();
  cups_log("operation.completed id=" + std::to_string(operation->operation_id) +
          " success=" + std::to_string(operation->success) +
          " durationMs=" + std::to_string(duration));
  g_main_context_invoke(nullptr, complete_cups_print_operation, operation);
  return nullptr;
}

static void start_cups_print_operation(CupsPrintOperation* operation) {
  GThread* thread = g_thread_new("thermal-cups-print", run_cups_print_operation, operation);
  g_thread_unref(thread);
}

// ---------------------------------------------------------------------------
// Method call handler
// ---------------------------------------------------------------------------

static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer /*user_data*/) {
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  g_autoptr(FlMethodResponse) response = nullptr;

  // ── getPlatformVersion ────────────────────────────────────────────────────
  if (strcmp(method, "getPlatformVersion") == 0) {
    g_autoptr(FlValue) result = fl_value_new_string("Linux");
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));

  // ── getUsbDevicesList ─────────────────────────────────────────────────────
  // Lists all CUPS printer queues (USB, network, etc.).
  } else if (strcmp(method, "getUsbDevicesList") == 0) {
    g_autoptr(FlValue) result = cups_get_printers_list();
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));

  // ── connect ───────────────────────────────────────────────────────────────
  // For a direct Bluetooth Classic printer, this verifies that the paired
  // device exposes SPP/RFCOMM and accepts a socket connection. CUPS queues
  // remain the fallback for all other desktop printers.
  } else if (strcmp(method, "connect") == 0) {
    const std::string connection_type = map_get_string(args, "connectionType");
    if (connection_type == "BLUETOOTH_CLASSIC") {
      const auto address = map_get_string(args, "address");
      if (address.empty()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "DEVICE_UNAVAILABLE", "Bluetooth printer address is missing", nullptr));
        fl_method_call_respond(method_call, response, nullptr);
        return;
      }
      auto* operation = new BluetoothOperation{
          FL_METHOD_CALL(g_object_ref(method_call)),
          BluetoothOperationKind::connect,
          address,
          ++next_bluetooth_operation_id,
      };
      start_bluetooth_operation(operation);
      return;
    }
    if (connection_type == "USB") {
      const auto path = map_get_string(args, "address");
      if (path.empty()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "DEVICE_UNAVAILABLE", "USB printer path is missing", nullptr));
        fl_method_call_respond(method_call, response, nullptr);
        return;
      }
      auto* operation = new UsbOperation{
          FL_METHOD_CALL(g_object_ref(method_call)),
          UsbOperationKind::connect,
          path,
          {},
          ++next_usb_operation_id,
      };
      start_usb_operation(operation);
      return;
    }
    const std::string name = map_get_string(args, "name");
    const bool found = cups_printer_exists(name.c_str());
    g_autoptr(FlValue) result = fl_value_new_bool(found ? TRUE : FALSE);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));

  // ── disconnect ────────────────────────────────────────────────────────────
  // CUPS is stateless — no explicit disconnection needed.
  } else if (strcmp(method, "disconnect") == 0) {
    g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));

  // ── isConnected ───────────────────────────────────────────────────────────
  // Returns true if the CUPS queue still exists and is accepting jobs.
  } else if (strcmp(method, "isConnected") == 0) {
    const std::string connection_type = map_get_string(args, "connectionType");
    if (connection_type == "BLUETOOTH_CLASSIC") {
      const auto address = map_get_string(args, "address");
      if (address.empty()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "DEVICE_UNAVAILABLE", "Bluetooth printer address is missing", nullptr));
        fl_method_call_respond(method_call, response, nullptr);
        return;
      }
      auto* operation = new BluetoothOperation{
          FL_METHOD_CALL(g_object_ref(method_call)),
          BluetoothOperationKind::connect,
          address,
          ++next_bluetooth_operation_id,
      };
      operation->return_false_as_value = true;
      start_bluetooth_operation(operation);
      return;
    }
    if (connection_type == "USB") {
      const auto path = map_get_string(args, "address");
      if (path.empty()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "DEVICE_UNAVAILABLE", "USB printer path is missing", nullptr));
        fl_method_call_respond(method_call, response, nullptr);
        return;
      }
      auto* operation = new UsbOperation{
          FL_METHOD_CALL(g_object_ref(method_call)),
          UsbOperationKind::connect,
          path,
          {},
          ++next_usb_operation_id,
      };
      operation->return_false_as_value = true;
      start_usb_operation(operation);
      return;
    }
    // The Dart layer sends vendorId = CUPS queue name (set in getUsbDevicesList).
    std::string printer_name = map_get_string(args, "vendorId");
    if (printer_name.empty()) {
      printer_name = map_get_string(args, "name");
    }
    const bool found = cups_printer_exists(printer_name.c_str());
    g_autoptr(FlValue) result = fl_value_new_bool(found ? TRUE : FALSE);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));

  // ── printText ─────────────────────────────────────────────────────────────
  // Sends raw ESC/POS bytes to the CUPS queue as a RAW print job.
  } else if (strcmp(method, "printText") == 0) {
    if (fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "INVALID_ARGS", "Expected map argument", nullptr));
    } else {
      // Resolve printer name: prefer 'name', fall back to 'vendorId'.
      std::string printer_name = map_get_string(args, "name");
      const std::string connection_type = map_get_string(args, "connectionType");
      if (connection_type == "BLUETOOTH_CLASSIC") {
        const auto address = map_get_string(args, "address");
        FlValue* data_val = fl_value_lookup_string(args, "data");
        const auto data_bytes = bytes_from_value(data_val);
        if (address.empty() || data_bytes.empty()) {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              address.empty() ? "DEVICE_UNAVAILABLE" : "BLUETOOTH_WRITE_FAILED",
              address.empty() ? "Bluetooth printer address is missing"
                              : "Bluetooth print payload is empty",
              nullptr));
        } else {
          auto* operation = new BluetoothOperation{
              FL_METHOD_CALL(g_object_ref(method_call)),
              BluetoothOperationKind::print,
              address,
              data_bytes,
              ++next_bluetooth_operation_id,
          };
          start_bluetooth_operation(operation);
          return;
        }
      } else if (connection_type == "USB") {
        const auto path = map_get_string(args, "address");
        FlValue* data_val = fl_value_lookup_string(args, "data");
        const auto data_bytes = bytes_from_value(data_val);
        if (path.empty() || data_bytes.empty()) {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              path.empty() ? "DEVICE_UNAVAILABLE" : "USB_WRITE_FAILED",
              path.empty() ? "USB printer path is missing" : "USB print payload is empty",
              nullptr));
        } else {
          auto* operation = new UsbOperation{
              FL_METHOD_CALL(g_object_ref(method_call)),
              UsbOperationKind::print,
              path,
              data_bytes,
              ++next_usb_operation_id,
          };
          start_usb_operation(operation);
          return;
        }
        fl_method_call_respond(method_call, response, nullptr);
        return;
      } else if (printer_name.empty()) {
        printer_name = map_get_string(args, "vendorId");
      }
      if (printer_name.empty()) {
        printer_name = map_get_string(args, "path");
      }

      if (printer_name.empty()) {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new(
            "INVALID_ARGS", "Printer name not provided", nullptr));
      } else {
        FlValue* data_val = fl_value_lookup_string(args, "data");
        if (!data_val) {
          response = FL_METHOD_RESPONSE(fl_method_error_response_new(
              "INVALID_ARGS", "No data provided", nullptr));
        } else {
          // Build byte buffer — handle both List<int> and Uint8List.
          std::vector<uint8_t> data_bytes;
          const FlValueType data_type = fl_value_get_type(data_val);

          if (data_type == FL_VALUE_TYPE_UINT8_LIST) {
            const size_t len = fl_value_get_length(data_val);
            const uint8_t* raw = fl_value_get_uint8_list(data_val);
            data_bytes.assign(raw, raw + len);

          } else if (data_type == FL_VALUE_TYPE_LIST) {
            const size_t len = fl_value_get_length(data_val);
            data_bytes.resize(len);
            for (size_t i = 0; i < len; i++) {
              FlValue* byte_val = fl_value_get_list_value(data_val, i);
              data_bytes[i] =
                  static_cast<uint8_t>(fl_value_get_int(byte_val));
            }

          } else if (data_type == FL_VALUE_TYPE_INT32_LIST) {
            const size_t len = fl_value_get_length(data_val);
            const int32_t* raw = fl_value_get_int32_list(data_val);
            data_bytes.resize(len);
            for (size_t i = 0; i < len; i++) {
              data_bytes[i] = static_cast<uint8_t>(raw[i]);
            }
          }

          if (data_bytes.empty()) {
            response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                "INVALID_ARGS", "Data is empty or unsupported type",
                nullptr));
          } else {
            auto* operation = new CupsPrintOperation{
                FL_METHOD_CALL(g_object_ref(method_call)),
                printer_name,
                data_bytes,
                ++next_cups_operation_id,
            };
            start_cups_print_operation(operation);
            return;
          }
        }
      }
    }

  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

// ---------------------------------------------------------------------------
// Plugin registration
// ---------------------------------------------------------------------------

void flutter_thermal_printer_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  FlutterThermalPrinterPlugin* plugin = FLUTTER_THERMAL_PRINTER_PLUGIN(
      g_object_new(flutter_thermal_printer_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "flutter_thermal_printer",
      FL_METHOD_CODEC(codec));

  fl_method_channel_set_method_call_handler(
      channel, method_call_cb, g_object_ref(plugin), g_object_unref);

  g_object_unref(plugin);
}

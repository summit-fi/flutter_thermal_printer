#include "flutter_thermal_printer_plugin.h"

#include <ws2bth.h>
#include <bthsdpdef.h>
#include <windows.h>
#include <VersionHelpers.h>

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace flutter_thermal_printer {

namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

constexpr auto kBluetoothConnectTimeout = std::chrono::seconds(10);
constexpr size_t kBluetoothWriteChunkSize = 1'024;
constexpr DWORD kUsbWriteTimeoutMs = 10'000;

void BluetoothLog(const std::wstring& message) {
  OutputDebugStringW((L"[FlutterThermalPrinterNative] windows.bluetooth " + message + L"\n").c_str());
}

void UsbLog(const std::wstring& message) {
  OutputDebugStringW((L"[FlutterThermalPrinterNative] windows.usb " + message + L"\n").c_str());
}

std::wstring WideFromUtf8(const std::string& value) {
  if (value.empty()) return L"";
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (size <= 1) return L"";
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size) == 0) return L"";
  result.pop_back();
  return result;
}

std::string StringValue(const EncodableMap& map, const char* key) {
  const auto iterator = map.find(EncodableValue(key));
  if (iterator == map.end()) return "";
  const auto value = std::get_if<std::string>(&iterator->second);
  return value == nullptr ? "" : *value;
}

std::vector<uint8_t> BytesValue(const EncodableMap& map, const char* key) {
  const auto iterator = map.find(EncodableValue(key));
  if (iterator == map.end()) return {};
  const auto list = std::get_if<EncodableList>(&iterator->second);
  if (list == nullptr) return {};

  std::vector<uint8_t> bytes;
  bytes.reserve(list->size());
  for (const auto& value : *list) {
    if (const auto integer = std::get_if<int32_t>(&value)) {
      bytes.push_back(static_cast<uint8_t>(*integer));
    } else if (const auto long_integer = std::get_if<int64_t>(&value)) {
      bytes.push_back(static_cast<uint8_t>(*long_integer));
    }
  }
  return bytes;
}

std::string NormalizeAddress(const std::string& address) {
  std::string normalized;
  normalized.reserve(12);
  for (const unsigned char character : address) {
    if (std::isxdigit(character)) normalized.push_back(static_cast<char>(std::toupper(character)));
  }
  return normalized.size() == 12 ? normalized : "";
}

bool BluetoothAddressFromString(const std::string& address, BTH_ADDR* result) {
  const auto normalized = NormalizeAddress(address);
  if (normalized.empty() || result == nullptr) return false;
  try {
    *result = static_cast<BTH_ADDR>(std::stoull(normalized, nullptr, 16));
    return true;
  } catch (...) {
    return false;
  }
}

bool WaitForConnect(
    SOCKET socket_handle,
    const std::shared_ptr<std::atomic_bool>& cancellation) {
  const auto deadline = std::chrono::steady_clock::now() + kBluetoothConnectTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cancellation != nullptr && cancellation->load()) return false;

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket_handle, &write_set);

    timeval timeout{};
    timeout.tv_usec = 100'000;
    const auto select_result = select(0, nullptr, &write_set, nullptr, &timeout);
    if (select_result == SOCKET_ERROR) return false;
    if (select_result == 0) continue;

    int socket_error = 0;
    int socket_error_size = sizeof(socket_error);
    return getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                      reinterpret_cast<char*>(&socket_error),
                      &socket_error_size) == 0 &&
        socket_error == 0;
  }
  return false;
}

}  // namespace

// static
void FlutterThermalPrinterPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "flutter_thermal_printer",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<FlutterThermalPrinterPlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

FlutterThermalPrinterPlugin::FlutterThermalPrinterPlugin() {
  WSADATA wsa_data{};
  const int status = WSAStartup(MAKEWORD(2, 2), &wsa_data);
  BluetoothLog(L"winsock.start status=" + std::to_wstring(status));
}

FlutterThermalPrinterPlugin::~FlutterThermalPrinterPlugin() {
  CancelPrint();
  {
    std::lock_guard lock(print_worker_mutex_);
    if (print_worker_.joinable()) {
      print_worker_.join();
    }
  }
  std::lock_guard lock(bluetooth_sockets_mutex_);
  for (const auto& [address, socket_handle] : bluetooth_sockets_) {
    BluetoothLog(L"disconnect.cleanup address=" + WideFromUtf8(address));
    shutdown(socket_handle, SD_BOTH);
    closesocket(socket_handle);
  }
  bluetooth_sockets_.clear();
  WSACleanup();
}

void FlutterThermalPrinterPlugin::StartPrintWorker(
    PrintOperation operation,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  std::lock_guard lock(print_worker_mutex_);
  if (print_worker_.joinable()) {
    if (print_worker_done_ == nullptr || !print_worker_done_->load()) {
      result->Error("PRINT_BUSY", "Another Windows print operation is still running.");
      return;
    }
    print_worker_.join();
  }

  const auto done = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = std::make_shared<std::atomic_bool>(false);
  print_worker_done_ = done;
  print_worker_cancellation_ = cancellation;
  print_worker_ = std::thread([
      operation = std::move(operation), result = std::move(result), done,
      cancellation]() mutable {
        bool success = false;
        try {
          success = operation(cancellation);
          if (cancellation->load()) {
            result->Error("PRINT_CANCELLED", "Windows print operation was cancelled.");
          } else {
            result->Success(EncodableValue(success));
          }
        } catch (...) {
          result->Error("PRINT_FAILED", "Windows print operation failed unexpectedly.");
        }
        done->store(true);
      });
}

void FlutterThermalPrinterPlugin::CancelPrint() {
  std::shared_ptr<std::atomic_bool> cancellation;
  {
    std::lock_guard lock(print_worker_mutex_);
    cancellation = print_worker_cancellation_;
  }
  if (cancellation != nullptr) cancellation->store(true);
}

bool FlutterThermalPrinterPlugin::ConnectBluetoothClassic(
    const std::string& address,
    const CancellationToken& cancellation) {
  if (cancellation != nullptr && cancellation->load()) return false;
  const auto normalized_address = NormalizeAddress(address);
  BTH_ADDR bluetooth_address = 0;
  if (!BluetoothAddressFromString(address, &bluetooth_address)) {
    BluetoothLog(L"connect.failed reason=invalid_address address=" + WideFromUtf8(address));
    return false;
  }

  {
    std::lock_guard lock(bluetooth_sockets_mutex_);
    const auto socket_iterator = bluetooth_sockets_.find(normalized_address);
    if (socket_iterator != bluetooth_sockets_.end()) {
      int socket_error = 0;
      int socket_error_size = sizeof(socket_error);
      const auto status = getsockopt(
          socket_iterator->second,
          SOL_SOCKET,
          SO_ERROR,
          reinterpret_cast<char*>(&socket_error),
          &socket_error_size);
      if (status == 0 && socket_error == 0) {
        BluetoothLog(L"connect.reused address=" + WideFromUtf8(normalized_address));
        return true;
      }
      BluetoothLog(L"connect.stale address=" + WideFromUtf8(normalized_address) +
                   L" error=" + std::to_wstring(socket_error));
      shutdown(socket_iterator->second, SD_BOTH);
      closesocket(socket_iterator->second);
      bluetooth_sockets_.erase(socket_iterator);
    }
  }

  SOCKET socket_handle = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
  if (socket_handle == INVALID_SOCKET) {
    BluetoothLog(L"connect.failed reason=socket error=" + std::to_wstring(WSAGetLastError()));
    return false;
  }

  u_long non_blocking = 1;
  if (ioctlsocket(socket_handle, FIONBIO, &non_blocking) != 0) {
    BluetoothLog(L"connect.failed reason=nonblocking error=" + std::to_wstring(WSAGetLastError()));
    closesocket(socket_handle);
    return false;
  }

  SOCKADDR_BTH remote{};
  remote.addressFamily = AF_BTH;
  remote.btAddr = bluetooth_address;
  remote.serviceClassId = SerialPortServiceClass_UUID;
  remote.port = 0;

  BluetoothLog(L"connect.started address=" + WideFromUtf8(normalized_address));
  const int connect_result = connect(
      socket_handle, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));
  const int connect_error = connect_result == SOCKET_ERROR ? WSAGetLastError() : 0;
  if (connect_result == SOCKET_ERROR && connect_error != WSAEWOULDBLOCK &&
      connect_error != WSAEINPROGRESS) {
    BluetoothLog(L"connect.failed reason=connect error=" + std::to_wstring(connect_error));
    closesocket(socket_handle);
    return false;
  }
  if (connect_result == SOCKET_ERROR && !WaitForConnect(socket_handle, cancellation)) {
    BluetoothLog(L"connect.failed reason=timeout_or_socket_error error=" +
                 std::to_wstring(WSAGetLastError()));
    closesocket(socket_handle);
    return false;
  }

  {
    std::lock_guard lock(bluetooth_sockets_mutex_);
    bluetooth_sockets_.emplace(normalized_address, socket_handle);
  }
  BluetoothLog(L"connect.success address=" + WideFromUtf8(normalized_address));
  return true;
}

bool FlutterThermalPrinterPlugin::PrintBluetoothClassic(
    const std::string& address,
    const std::vector<uint8_t>& bytes,
    const CancellationToken& cancellation) {
  if (cancellation != nullptr && cancellation->load()) return false;
  const auto normalized_address = NormalizeAddress(address);
  if (normalized_address.empty()) {
    BluetoothLog(L"print.failed reason=invalid_address");
    return false;
  }
  if (bytes.empty()) {
    BluetoothLog(L"print.skipped reason=empty_data address=" + WideFromUtf8(normalized_address));
    return true;
  }
  if (!ConnectBluetoothClassic(normalized_address, cancellation)) return false;

  std::lock_guard lock(bluetooth_sockets_mutex_);
  const auto socket_iterator = bluetooth_sockets_.find(normalized_address);
  if (socket_iterator == bluetooth_sockets_.end()) return false;

  const auto socket_handle = socket_iterator->second;
  size_t offset = 0;
  BluetoothLog(L"print.started address=" + WideFromUtf8(normalized_address) +
               L" bytes=" + std::to_wstring(bytes.size()));
  while (offset < bytes.size()) {
    if (cancellation != nullptr && cancellation->load()) {
      shutdown(socket_handle, SD_BOTH);
      closesocket(socket_handle);
      bluetooth_sockets_.erase(socket_iterator);
      BluetoothLog(L"print.cancelled address=" + WideFromUtf8(normalized_address));
      return false;
    }
    const auto remaining = bytes.size() - offset;
    const auto chunk_size = static_cast<int>(std::min(remaining, kBluetoothWriteChunkSize));
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(socket_handle, &write_set);
    timeval wait_time{};
    wait_time.tv_usec = 100'000;
    const auto writable = select(0, nullptr, &write_set, nullptr, &wait_time);
    if (writable == SOCKET_ERROR) {
      BluetoothLog(L"print.failed address=" + WideFromUtf8(normalized_address) +
                   L" stage=select error=" + std::to_wstring(WSAGetLastError()));
      shutdown(socket_handle, SD_BOTH);
      closesocket(socket_handle);
      bluetooth_sockets_.erase(socket_iterator);
      return false;
    }
    if (writable == 0) continue;
    const int sent = send(socket_handle, reinterpret_cast<const char*>(bytes.data() + offset), chunk_size, 0);
    if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
    if (sent <= 0) {
      BluetoothLog(L"print.failed address=" + WideFromUtf8(normalized_address) +
                   L" offset=" + std::to_wstring(offset) +
                   L" error=" + std::to_wstring(WSAGetLastError()));
      shutdown(socket_handle, SD_BOTH);
      closesocket(socket_handle);
      bluetooth_sockets_.erase(socket_iterator);
      return false;
    }
    offset += static_cast<size_t>(sent);
  }
  BluetoothLog(L"print.success address=" + WideFromUtf8(normalized_address) +
               L" bytes=" + std::to_wstring(bytes.size()));
  return true;
}

bool FlutterThermalPrinterPlugin::IsBluetoothClassicConnected(const std::string& address) {
  const auto normalized_address = NormalizeAddress(address);
  std::lock_guard lock(bluetooth_sockets_mutex_);
  const auto socket_iterator = bluetooth_sockets_.find(normalized_address);
  if (socket_iterator == bluetooth_sockets_.end()) {
    BluetoothLog(L"connection.checked address=" + WideFromUtf8(normalized_address) + L" connected=0");
    return false;
  }

  int socket_error = 0;
  int socket_error_size = sizeof(socket_error);
  const auto status = getsockopt(
      socket_iterator->second,
      SOL_SOCKET,
      SO_ERROR,
      reinterpret_cast<char*>(&socket_error),
      &socket_error_size);
  const bool connected = status == 0 && socket_error == 0;
  if (!connected) {
    shutdown(socket_iterator->second, SD_BOTH);
    closesocket(socket_iterator->second);
    bluetooth_sockets_.erase(socket_iterator);
  }
  BluetoothLog(L"connection.checked address=" + WideFromUtf8(normalized_address) +
               L" connected=" + std::to_wstring(connected));
  return connected;
}

bool FlutterThermalPrinterPlugin::DisconnectBluetoothClassic(const std::string& address) {
  const auto normalized_address = NormalizeAddress(address);
  std::lock_guard lock(bluetooth_sockets_mutex_);
  const auto socket_iterator = bluetooth_sockets_.find(normalized_address);
  if (socket_iterator == bluetooth_sockets_.end()) return true;
  shutdown(socket_iterator->second, SD_BOTH);
  closesocket(socket_iterator->second);
  bluetooth_sockets_.erase(socket_iterator);
  BluetoothLog(L"disconnect.success address=" + WideFromUtf8(normalized_address));
  return true;
}

bool FlutterThermalPrinterPlugin::CanOpenUsbPrinter(const std::string& device_path) {
  const auto wide_path = WideFromUtf8(device_path);
  if (wide_path.empty()) {
    UsbLog(L"open.failed reason=empty_path");
    return false;
  }

  const auto handle = CreateFileW(
      wide_path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    UsbLog(L"open.failed error=" + std::to_wstring(GetLastError()));
    return false;
  }

  CloseHandle(handle);
  UsbLog(L"open.success");
  return true;
}

bool FlutterThermalPrinterPlugin::PrintUsbPrinter(
    const std::string& device_path,
    const std::vector<uint8_t>& bytes,
    const CancellationToken& cancellation) {
  if (cancellation != nullptr && cancellation->load()) return false;
  if (bytes.empty()) {
    UsbLog(L"print.skipped reason=empty_data");
    return true;
  }

  const auto wide_path = WideFromUtf8(device_path);
  if (wide_path.empty()) {
    UsbLog(L"print.failed reason=empty_path");
    return false;
  }

  const auto handle = CreateFileW(
      wide_path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    UsbLog(L"print.failed stage=open error=" + std::to_wstring(GetLastError()));
    return false;
  }

  HANDLE completion_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (completion_event == nullptr) {
    UsbLog(L"print.failed stage=create_event error=" + std::to_wstring(GetLastError()));
    CloseHandle(handle);
    return false;
  }

  constexpr size_t kUsbWriteChunkSize = 4 * 1024;
  size_t offset = 0;
  bool success = true;
  UsbLog(L"print.started bytes=" + std::to_wstring(bytes.size()));
  while (offset < bytes.size()) {
    if (cancellation != nullptr && cancellation->load()) {
      UsbLog(L"print.cancelled offset=" + std::to_wstring(offset));
      success = false;
      break;
    }
    const auto remaining = bytes.size() - offset;
    const auto chunk_size = static_cast<DWORD>(std::min(remaining, kUsbWriteChunkSize));
    OVERLAPPED overlapped{};
    overlapped.hEvent = completion_event;
    ResetEvent(completion_event);

    DWORD written = 0;
    const auto write_started = WriteFile(
        handle,
        bytes.data() + offset,
        chunk_size,
        &written,
        &overlapped);
    if (write_started == FALSE) {
      const auto write_error = GetLastError();
      if (write_error != ERROR_IO_PENDING) {
        UsbLog(L"print.failed stage=write offset=" + std::to_wstring(offset) +
               L" error=" + std::to_wstring(write_error));
        success = false;
        break;
      }

      DWORD wait_result = WAIT_TIMEOUT;
      const auto deadline = std::chrono::steady_clock::now() +
          std::chrono::milliseconds(kUsbWriteTimeoutMs);
      while (std::chrono::steady_clock::now() < deadline) {
        if (cancellation != nullptr && cancellation->load()) {
          CancelIoEx(handle, &overlapped);
          UsbLog(L"print.cancelled offset=" + std::to_wstring(offset));
          success = false;
          break;
        }
        wait_result = WaitForSingleObject(completion_event, 100);
        if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) break;
      }
      if (success && wait_result == WAIT_TIMEOUT) {
        CancelIoEx(handle, &overlapped);
      }
      if (wait_result != WAIT_OBJECT_0 ||
          GetOverlappedResult(handle, &overlapped, &written, FALSE) == FALSE) {
        if (!success && cancellation != nullptr && cancellation->load()) break;
        const auto completion_error = GetLastError();
        CancelIoEx(handle, &overlapped);
        UsbLog(L"print.failed stage=completion offset=" + std::to_wstring(offset) +
               L" wait=" + std::to_wstring(wait_result) +
               L" error=" + std::to_wstring(completion_error));
        success = false;
        break;
      }
    }

    if (written != chunk_size) {
      UsbLog(L"print.failed stage=partial_write offset=" + std::to_wstring(offset) +
             L" expected=" + std::to_wstring(chunk_size) +
             L" actual=" + std::to_wstring(written));
      success = false;
      break;
    }
    offset += written;
  }

  CloseHandle(completion_event);
  CloseHandle(handle);
  UsbLog(success ? L"print.success bytes=" + std::to_wstring(bytes.size()) : L"print.finished success=0");
  return success;
}

void FlutterThermalPrinterPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto arguments = std::get_if<EncodableMap>(method_call.arguments());
  const auto is_bluetooth_classic = arguments != nullptr &&
      StringValue(*arguments, "connectionType") == "BLUETOOTH_CLASSIC";
  const auto is_usb = arguments != nullptr && StringValue(*arguments, "connectionType") == "USB";

  if (method_call.method_name().compare("getPlatformVersion") == 0) {
    std::ostringstream version_stream;
    version_stream << "Windows ";
    if (IsWindows10OrGreater()) {
      version_stream << "10+";
    } else if (IsWindows8OrGreater()) {
      version_stream << "8";
    } else if (IsWindows7OrGreater()) {
      version_stream << "7";
    }
    result->Success(flutter::EncodableValue(version_stream.str()));
  } else if (method_call.method_name().compare("connect") == 0 && is_bluetooth_classic) {
    result->Success(EncodableValue(
        ConnectBluetoothClassic(StringValue(*arguments, "address"), nullptr)));
  } else if (method_call.method_name().compare("printText") == 0 && is_bluetooth_classic) {
    const auto address = StringValue(*arguments, "address");
    const auto bytes = BytesValue(*arguments, "data");
    StartPrintWorker(
        [this, address, bytes](const CancellationToken& cancellation) {
          return PrintBluetoothClassic(address, bytes, cancellation);
        },
        std::move(result));
  } else if (method_call.method_name().compare("cancelPrint") == 0) {
    CancelPrint();
    result->Success();
  } else if (method_call.method_name().compare("isConnected") == 0 && is_bluetooth_classic) {
    result->Success(EncodableValue(IsBluetoothClassicConnected(StringValue(*arguments, "address"))));
  } else if (method_call.method_name().compare("disconnect") == 0 && is_bluetooth_classic) {
    result->Success(EncodableValue(DisconnectBluetoothClassic(StringValue(*arguments, "address"))));
  } else if (method_call.method_name().compare("connect") == 0 && is_usb) {
    result->Success(EncodableValue(CanOpenUsbPrinter(StringValue(*arguments, "address"))));
  } else if (method_call.method_name().compare("printText") == 0 && is_usb) {
    const auto device_path = StringValue(*arguments, "address");
    const auto bytes = BytesValue(*arguments, "data");
    StartPrintWorker(
        [device_path, bytes](const CancellationToken& cancellation) {
          return PrintUsbPrinter(device_path, bytes, cancellation);
        },
        std::move(result));
  } else if (method_call.method_name().compare("isConnected") == 0 && is_usb) {
    result->Success(EncodableValue(CanOpenUsbPrinter(StringValue(*arguments, "address"))));
  } else if (method_call.method_name().compare("disconnect") == 0 && is_usb) {
    result->Success(EncodableValue(true));
  } else {
    result->NotImplemented();
  }
}

}  // namespace flutter_thermal_printer

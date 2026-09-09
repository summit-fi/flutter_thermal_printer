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
using MethodResult = flutter::MethodResult<EncodableValue>;
using ResultHolder = std::shared_ptr<std::unique_ptr<MethodResult>>;

constexpr auto kBluetoothConnectTimeout = std::chrono::seconds(10);
constexpr auto kTransportOperationTimeout = std::chrono::seconds(30);
constexpr size_t kBluetoothWriteChunkSize = 1'024;
constexpr DWORD kUsbWriteTimeoutMs = 10'000;

/// Writes diagnostic messages to the Windows debugger output.
void BluetoothLog(const std::wstring& message) {
  OutputDebugStringW((L"[FlutterThermalPrinterNative] windows.bluetooth " + message + L"\n").c_str());
}

/// Writes USB transport diagnostics to the Windows debugger output.
void UsbLog(const std::wstring& message) {
  OutputDebugStringW((L"[FlutterThermalPrinterNative] windows.usb " + message + L"\n").c_str());
}

/// Writes print lifecycle diagnostics to the Windows debugger output.
void PrintLog(const std::wstring& message) {
  OutputDebugStringW((L"[FlutterThermalPrinterNative] windows.print " + message + L"\n").c_str());
}

/// Writes connection lifecycle diagnostics to the Windows debugger output.
void TransportLog(const std::wstring& message) {
  OutputDebugStringW((L"[FlutterThermalPrinterNative] windows.transport " + message + L"\n").c_str());
}

int64_t DurationMilliseconds(const std::chrono::steady_clock::time_point& started_at) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - started_at)
      .count();
}

bool TakeResult(const ResultHolder& holder,
                const std::shared_ptr<std::mutex>& result_mutex,
                const std::shared_ptr<std::atomic_bool>& completed,
                std::unique_ptr<MethodResult>* result) {
  if (completed->exchange(true)) return false;
  std::lock_guard lock(*result_mutex);
  *result = std::move(*holder);
  return *result != nullptr;
}

void CompleteResultError(const ResultHolder& holder,
                         const std::shared_ptr<std::mutex>& result_mutex,
                         const std::shared_ptr<std::atomic_bool>& completed,
                         const char* code,
                         const char* message) {
  std::unique_ptr<MethodResult> result;
  if (TakeResult(holder, result_mutex, completed, &result)) {
    result->Error(code, message);
  }
}

void CompleteResultBool(const ResultHolder& holder,
                        const std::shared_ptr<std::mutex>& result_mutex,
                        const std::shared_ptr<std::atomic_bool>& completed,
                        bool value) {
  std::unique_ptr<MethodResult> result;
  if (TakeResult(holder, result_mutex, completed, &result)) {
    result->Success(EncodableValue(value));
  }
}

/// Converts UTF-8 input received from Dart to a Windows string.
std::wstring WideFromUtf8(const std::string& value) {
  if (value.empty()) return L"";
  const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (size <= 1) return L"";
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size) == 0) return L"";
  result.pop_back();
  return result;
}

/// Reads a string argument from a Flutter method-call map.
std::string StringValue(const EncodableMap& map, const char* key) {
  const auto iterator = map.find(EncodableValue(key));
  if (iterator == map.end()) return "";
  const auto value = std::get_if<std::string>(&iterator->second);
  return value == nullptr ? "" : *value;
}

/// Reads a byte list argument from a Flutter method-call map.
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

/// Removes separators and validates a Bluetooth address for Win32 APIs.
std::string NormalizeAddress(const std::string& address) {
  std::string normalized;
  normalized.reserve(12);
  for (const unsigned char character : address) {
    if (std::isxdigit(character)) normalized.push_back(static_cast<char>(std::toupper(character)));
  }
  return normalized.size() == 12 ? normalized : "";
}

/// Parses a normalized Bluetooth address into the Win32 address representation.
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

/// Waits for a non-blocking RFCOMM connect without blocking the worker indefinitely.
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
  CancelConnection();
  {
    std::lock_guard lock(connection_worker_mutex_);
    if (connection_worker_.joinable()) {
      connection_worker_.join();
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
    const std::string& transport,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto operation_id = ++print_operation_id_;
  if (connection_active_.load()) {
    PrintLog(L"rejected id=" + std::to_wstring(operation_id) +
             L" transport=" + WideFromUtf8(transport) + L" reason=connection_busy");
    result->Error("TRANSPORT_BUSY", "A Windows transport operation is still running.");
    return;
  }
  std::lock_guard lock(print_worker_mutex_);
  if (print_worker_.joinable()) {
    if (print_worker_done_ == nullptr || !print_worker_done_->load()) {
      PrintLog(L"rejected id=" + std::to_wstring(operation_id) +
               L" reason=busy");
      result->Error("PRINT_BUSY", "Another Windows print operation is still running.");
      return;
    }
    print_worker_.join();
  }

  const auto done = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = std::make_shared<std::atomic_bool>(false);
  const auto timed_out = std::make_shared<std::atomic_bool>(false);
  const auto completed = std::make_shared<std::atomic_bool>(false);
  const auto result_mutex = std::make_shared<std::mutex>();
  const auto result_holder = std::make_shared<std::unique_ptr<MethodResult>>(
      std::move(result));
  const auto started_at = std::chrono::steady_clock::now();
  print_worker_done_ = done;
  print_worker_cancellation_ = cancellation;
  print_cancel_completion_ = [completed, result_mutex, result_holder] {
    CompleteResultError(result_holder, result_mutex, completed,
                        "TRANSPORT_CANCELLED",
                        "Windows print operation was cancelled.");
  };
  print_active_.store(true);
  print_worker_ = std::thread([
      operation = std::move(operation), done, cancellation, timed_out,
      completed, result_mutex, result_holder, started_at, transport,
      operation_id,
      this]() mutable {
        PrintLog(L"started id=" + std::to_wstring(operation_id) +
                 L" transport=" + WideFromUtf8(transport));
        bool success = false;
        try {
          success = operation(cancellation);
          done->store(true);
          if (timed_out->load()) {
            PrintLog(L"timeout id=" + std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     L" errorCode=TRANSPORT_TIMEOUT" +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
            CompleteResultError(result_holder, result_mutex, completed,
                                "TRANSPORT_TIMEOUT",
                                "Windows print operation timed out.");
          } else if (cancellation->load()) {
            PrintLog(L"cancelled id=" + std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     L" errorCode=TRANSPORT_CANCELLED" +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
            CompleteResultError(result_holder, result_mutex, completed,
                                "TRANSPORT_CANCELLED",
                                "Windows print operation was cancelled.");
          } else {
            PrintLog((success ? L"completed id=" : L"failed id=") +
                     std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     (success ? L" errorCode=SUCCESS" : L" errorCode=TRANSPORT_FAILED") +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
            if (success) {
              CompleteResultBool(result_holder, result_mutex, completed, true);
            } else {
              CompleteResultError(result_holder, result_mutex, completed,
                                  "TRANSPORT_FAILED",
                                  "Windows print operation failed.");
            }
        }
        print_active_.store(false);
      } catch (...) {
          done->store(true);
          PrintLog(L"failed id=" + std::to_wstring(operation_id) +
                   L" transport=" + WideFromUtf8(transport) +
                   L" errorCode=TRANSPORT_FAILED" +
                   L" reason=exception durationMs=" +
                   std::to_wstring(DurationMilliseconds(started_at)));
          CompleteResultError(result_holder, result_mutex, completed,
                              "TRANSPORT_FAILED",
                              "Windows print operation failed unexpectedly.");
          print_active_.store(false);
        }
      });
  std::thread([done, cancellation, timed_out, completed, result_mutex,
               result_holder, started_at, transport, operation_id] {
    std::this_thread::sleep_for(kTransportOperationTimeout);
    if (done->load() || completed->load()) return;
    timed_out->store(true);
    cancellation->store(true);
    PrintLog(L"timeout.requested id=" + std::to_wstring(operation_id) +
             L" transport=" + WideFromUtf8(transport) +
             L" errorCode=TRANSPORT_TIMEOUT" +
             L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
    CompleteResultError(result_holder, result_mutex, completed,
                        "TRANSPORT_TIMEOUT",
                        "Windows print operation timed out.");
  }).detach();
}

void FlutterThermalPrinterPlugin::CancelPrint() {
  std::shared_ptr<std::atomic_bool> cancellation;
  std::function<void()> complete;
  {
    std::lock_guard lock(print_worker_mutex_);
    cancellation = print_worker_cancellation_;
    complete = print_cancel_completion_;
  }
  if (cancellation != nullptr) cancellation->store(true);
  if (complete) complete();
}

void FlutterThermalPrinterPlugin::StartConnectionWorker(
    PrintOperation operation,
    const std::string& transport,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result,
    bool return_false_as_value) {
  const auto operation_id = ++connection_operation_id_;
  if (print_active_.load()) {
    TransportLog(L"connection.rejected id=" + std::to_wstring(operation_id) +
                 L" transport=" + WideFromUtf8(transport) + L" reason=print_busy");
    result->Error("TRANSPORT_BUSY", "A Windows print operation is still running.");
    return;
  }
  CancelConnection();

  std::lock_guard lock(connection_worker_mutex_);
  if (connection_worker_.joinable()) connection_worker_.join();

  const auto done = std::make_shared<std::atomic_bool>(false);
  const auto cancellation = std::make_shared<std::atomic_bool>(false);
  const auto timed_out = std::make_shared<std::atomic_bool>(false);
  const auto completed = std::make_shared<std::atomic_bool>(false);
  const auto result_mutex = std::make_shared<std::mutex>();
  const auto result_holder = std::make_shared<std::unique_ptr<MethodResult>>(
      std::move(result));
  const auto started_at = std::chrono::steady_clock::now();
  connection_worker_done_ = done;
  connection_worker_cancellation_ = cancellation;
  connection_cancel_completion_ = [completed, result_mutex, result_holder] {
    CompleteResultError(result_holder, result_mutex, completed,
                        "TRANSPORT_CANCELLED",
                        "Windows transport connection was cancelled.");
  };
  connection_active_.store(true);
  connection_worker_ = std::thread([
      operation = std::move(operation), done, cancellation, timed_out,
      completed, result_mutex, result_holder, started_at, transport,
      operation_id, return_false_as_value, this]() mutable {
    TransportLog(L"connection.started id=" + std::to_wstring(operation_id) +
                 L" transport=" + WideFromUtf8(transport));
    bool success = false;
    try {
      success = operation(cancellation);
      done->store(true);
      if (timed_out->load()) {
        TransportLog(L"connection.timeout id=" + std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     L" errorCode=TRANSPORT_TIMEOUT" +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
        CompleteResultError(result_holder, result_mutex, completed,
                            "TRANSPORT_TIMEOUT",
                            "Windows transport connection timed out.");
      } else if (cancellation->load()) {
        TransportLog(L"connection.cancelled id=" + std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     L" errorCode=TRANSPORT_CANCELLED" +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
        CompleteResultError(result_holder, result_mutex, completed,
                            "TRANSPORT_CANCELLED",
                            "Windows transport connection was cancelled.");
      } else if (success) {
        TransportLog(L"connection.completed id=" + std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     L" errorCode=SUCCESS" +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
        CompleteResultBool(result_holder, result_mutex, completed, true);
      } else if (return_false_as_value) {
        TransportLog(L"connection.checked id=" + std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     L" errorCode=DEVICE_UNAVAILABLE" +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
        CompleteResultBool(result_holder, result_mutex, completed, false);
      } else {
        TransportLog(L"connection.failed id=" + std::to_wstring(operation_id) +
                     L" transport=" + WideFromUtf8(transport) +
                     L" errorCode=" +
                     WideFromUtf8(transport == "bluetoothClassic"
                                      ? "BLUETOOTH_CONNECT_FAILED"
                                      : "USB_OPEN_FAILED") +
                     L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
        CompleteResultError(
            result_holder, result_mutex, completed,
            transport == "bluetoothClassic" ? "BLUETOOTH_CONNECT_FAILED" : "USB_OPEN_FAILED",
            transport == "bluetoothClassic"
                ? "Windows could not connect to the Bluetooth printer."
                : "Windows could not open the USB printer.");
      }
      connection_active_.store(false);
    } catch (...) {
      done->store(true);
      TransportLog(L"connection.failed id=" + std::to_wstring(operation_id) +
                   L" transport=" + WideFromUtf8(transport) +
                   L" errorCode=TRANSPORT_CONNECT_FAILED" +
                   L" reason=exception durationMs=" +
                   std::to_wstring(DurationMilliseconds(started_at)));
      CompleteResultError(result_holder, result_mutex, completed,
                          "TRANSPORT_CONNECT_FAILED",
                          "Windows transport connection failed unexpectedly.");
      connection_active_.store(false);
    }
  });

  std::thread([done, cancellation, timed_out, completed, result_mutex,
               result_holder, started_at, transport, operation_id] {
    std::this_thread::sleep_for(kTransportOperationTimeout);
    if (done->load() || completed->load()) return;
    timed_out->store(true);
    cancellation->store(true);
    TransportLog(L"connection.timeout.requested id=" + std::to_wstring(operation_id) +
                 L" transport=" + WideFromUtf8(transport) +
             L" errorCode=TRANSPORT_TIMEOUT" +
             L" durationMs=" + std::to_wstring(DurationMilliseconds(started_at)));
    CompleteResultError(result_holder, result_mutex, completed,
                        "TRANSPORT_TIMEOUT",
                        "Windows transport connection timed out.");
  }).detach();
}

void FlutterThermalPrinterPlugin::CancelConnection() {
  std::shared_ptr<std::atomic_bool> cancellation;
  std::function<void()> complete;
  {
    std::lock_guard lock(connection_worker_mutex_);
    cancellation = connection_worker_cancellation_;
    complete = connection_cancel_completion_;
  }
  if (cancellation != nullptr) cancellation->store(true);
  if (complete) complete();
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
    BluetoothLog(L"print.failed code=BLUETOOTH_INVALID_ADDRESS");
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
      BluetoothLog(L"print.cancelled code=PRINT_CANCELLED address=" +
                   WideFromUtf8(normalized_address));
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
      BluetoothLog(L"print.failed code=BLUETOOTH_WRITE_WAIT_FAILED address=" +
                   WideFromUtf8(normalized_address) +
                   L" error=" + std::to_wstring(WSAGetLastError()));
      shutdown(socket_handle, SD_BOTH);
      closesocket(socket_handle);
      bluetooth_sockets_.erase(socket_iterator);
      return false;
    }
    if (writable == 0) continue;
    const int sent = send(socket_handle, reinterpret_cast<const char*>(bytes.data() + offset), chunk_size, 0);
    if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
    if (sent <= 0) {
      BluetoothLog(L"print.failed code=BLUETOOTH_WRITE_FAILED address=" +
                   WideFromUtf8(normalized_address) +
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
    UsbLog(L"print.failed code=USB_EMPTY_DEVICE_PATH");
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
    UsbLog(L"print.failed code=USB_OPEN_FAILED error=" + std::to_wstring(GetLastError()));
    return false;
  }

  HANDLE completion_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (completion_event == nullptr) {
    UsbLog(L"print.failed code=USB_COMPLETION_EVENT_FAILED error=" +
           std::to_wstring(GetLastError()));
    CloseHandle(handle);
    return false;
  }

  constexpr size_t kUsbWriteChunkSize = 4 * 1024;
  size_t offset = 0;
  bool success = true;
  UsbLog(L"print.started bytes=" + std::to_wstring(bytes.size()));
  while (offset < bytes.size()) {
    if (cancellation != nullptr && cancellation->load()) {
      UsbLog(L"print.cancelled code=PRINT_CANCELLED offset=" + std::to_wstring(offset));
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
        UsbLog(L"print.failed code=USB_WRITE_FAILED offset=" + std::to_wstring(offset) +
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
          UsbLog(L"print.cancelled code=PRINT_CANCELLED offset=" +
                 std::to_wstring(offset));
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
        UsbLog(L"print.failed code=USB_WRITE_COMPLETION_FAILED offset=" +
               std::to_wstring(offset) +
               L" wait=" + std::to_wstring(wait_result) +
               L" error=" + std::to_wstring(completion_error));
        success = false;
        break;
      }
    }

    if (written != chunk_size) {
      UsbLog(L"print.failed code=USB_PARTIAL_WRITE offset=" + std::to_wstring(offset) +
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
    const auto address = StringValue(*arguments, "address");
    if (NormalizeAddress(address).empty()) {
      result->Error("DEVICE_UNAVAILABLE", "The Bluetooth printer address is missing or invalid.");
      return;
    }
    StartConnectionWorker(
        [this, address](const CancellationToken& cancellation) {
          return ConnectBluetoothClassic(address, cancellation);
        },
        "bluetoothClassic",
        std::move(result));
  } else if (method_call.method_name().compare("printText") == 0 && is_bluetooth_classic) {
    const auto address = StringValue(*arguments, "address");
    const auto bytes = BytesValue(*arguments, "data");
    StartPrintWorker(
        [this, address, bytes](const CancellationToken& cancellation) {
          return PrintBluetoothClassic(address, bytes, cancellation);
        },
        "bluetoothClassic",
        std::move(result));
  } else if (method_call.method_name().compare("cancelPrint") == 0) {
    CancelPrint();
    result->Success();
  } else if (method_call.method_name().compare("cancelConnect") == 0) {
    CancelConnection();
    result->Success();
  } else if (method_call.method_name().compare("isConnected") == 0 && is_bluetooth_classic) {
    const auto address = StringValue(*arguments, "address");
    StartConnectionWorker(
        [this, address](const CancellationToken&) {
          return IsBluetoothClassicConnected(address);
        },
        "bluetoothClassic.status",
        std::move(result), true);
  } else if (method_call.method_name().compare("disconnect") == 0 && is_bluetooth_classic) {
    const auto address = StringValue(*arguments, "address");
    StartConnectionWorker(
        [this, address](const CancellationToken&) {
          return DisconnectBluetoothClassic(address);
        },
        "bluetoothClassic.disconnect",
        std::move(result), true);
  } else if (method_call.method_name().compare("connect") == 0 && is_usb) {
    const auto device_path = StringValue(*arguments, "address");
    if (device_path.empty()) {
      result->Error("DEVICE_UNAVAILABLE", "The USB printer device path is missing.");
      return;
    }
    StartConnectionWorker(
        [this, device_path](const CancellationToken&) {
          return CanOpenUsbPrinter(device_path);
        },
        "usb",
        std::move(result));
  } else if (method_call.method_name().compare("printText") == 0 && is_usb) {
    const auto device_path = StringValue(*arguments, "address");
    const auto bytes = BytesValue(*arguments, "data");
    StartPrintWorker(
        [device_path, bytes](const CancellationToken& cancellation) {
          return PrintUsbPrinter(device_path, bytes, cancellation);
        },
        "usb",
        std::move(result));
  } else if (method_call.method_name().compare("isConnected") == 0 && is_usb) {
    const auto device_path = StringValue(*arguments, "address");
    StartConnectionWorker(
        [this, device_path](const CancellationToken&) {
          return CanOpenUsbPrinter(device_path);
        },
        "usb.status",
        std::move(result), true);
  } else if (method_call.method_name().compare("disconnect") == 0 && is_usb) {
    StartConnectionWorker(
        [](const CancellationToken&) { return true; },
        "usb.disconnect",
        std::move(result));
  } else {
    result->NotImplemented();
  }
}

}  // namespace flutter_thermal_printer

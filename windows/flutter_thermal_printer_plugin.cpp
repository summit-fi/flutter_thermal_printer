#include "flutter_thermal_printer_plugin.h"

#include <ws2bth.h>
#include <bthsdpdef.h>
#include <windows.h>
#include <VersionHelpers.h>

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
#include <vector>

namespace flutter_thermal_printer {

namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

constexpr auto kBluetoothConnectTimeout = std::chrono::seconds(10);
constexpr int kBluetoothWriteTimeoutMs = 10'000;
constexpr size_t kBluetoothWriteChunkSize = 1'024;

void BluetoothLog(const std::wstring& message) {
  OutputDebugStringW((L"[FlutterThermalPrinterNative] windows.bluetooth " + message + L"\n").c_str());
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

bool WaitForConnect(SOCKET socket_handle) {
  fd_set write_set;
  FD_ZERO(&write_set);
  FD_SET(socket_handle, &write_set);

  timeval timeout{};
  timeout.tv_sec = static_cast<long>(kBluetoothConnectTimeout.count());
  const auto select_result = select(0, nullptr, &write_set, nullptr, &timeout);
  if (select_result != 1) return false;

  int socket_error = 0;
  int socket_error_size = sizeof(socket_error);
  return getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error),
                    &socket_error_size) == 0 &&
      socket_error == 0;
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
  std::lock_guard lock(bluetooth_sockets_mutex_);
  for (const auto& [address, socket_handle] : bluetooth_sockets_) {
    BluetoothLog(L"disconnect.cleanup address=" + WideFromUtf8(address));
    shutdown(socket_handle, SD_BOTH);
    closesocket(socket_handle);
  }
  bluetooth_sockets_.clear();
  WSACleanup();
}

bool FlutterThermalPrinterPlugin::ConnectBluetoothClassic(const std::string& address) {
  const auto normalized_address = NormalizeAddress(address);
  BTH_ADDR bluetooth_address = 0;
  if (!BluetoothAddressFromString(address, &bluetooth_address)) {
    BluetoothLog(L"connect.failed reason=invalid_address address=" + WideFromUtf8(address));
    return false;
  }

  {
    std::lock_guard lock(bluetooth_sockets_mutex_);
    if (bluetooth_sockets_.find(normalized_address) != bluetooth_sockets_.end()) {
      BluetoothLog(L"connect.reused address=" + WideFromUtf8(normalized_address));
      return true;
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
  if (connect_result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK &&
      WSAGetLastError() != WSAEINPROGRESS) {
    BluetoothLog(L"connect.failed reason=connect error=" + std::to_wstring(WSAGetLastError()));
    closesocket(socket_handle);
    return false;
  }
  if (connect_result == SOCKET_ERROR && !WaitForConnect(socket_handle)) {
    BluetoothLog(L"connect.failed reason=timeout_or_socket_error error=" +
                 std::to_wstring(WSAGetLastError()));
    closesocket(socket_handle);
    return false;
  }

  non_blocking = 0;
  ioctlsocket(socket_handle, FIONBIO, &non_blocking);
  setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char*>(&kBluetoothWriteTimeoutMs), sizeof(kBluetoothWriteTimeoutMs));

  {
    std::lock_guard lock(bluetooth_sockets_mutex_);
    bluetooth_sockets_.emplace(normalized_address, socket_handle);
  }
  BluetoothLog(L"connect.success address=" + WideFromUtf8(normalized_address));
  return true;
}

bool FlutterThermalPrinterPlugin::PrintBluetoothClassic(
    const std::string& address, const std::vector<uint8_t>& bytes) {
  const auto normalized_address = NormalizeAddress(address);
  if (normalized_address.empty()) {
    BluetoothLog(L"print.failed reason=invalid_address");
    return false;
  }
  if (bytes.empty()) {
    BluetoothLog(L"print.skipped reason=empty_data address=" + WideFromUtf8(normalized_address));
    return true;
  }
  if (!ConnectBluetoothClassic(normalized_address)) return false;

  std::lock_guard lock(bluetooth_sockets_mutex_);
  const auto socket_iterator = bluetooth_sockets_.find(normalized_address);
  if (socket_iterator == bluetooth_sockets_.end()) return false;

  const auto socket_handle = socket_iterator->second;
  size_t offset = 0;
  BluetoothLog(L"print.started address=" + WideFromUtf8(normalized_address) +
               L" bytes=" + std::to_wstring(bytes.size()));
  while (offset < bytes.size()) {
    const auto remaining = bytes.size() - offset;
    const auto chunk_size = static_cast<int>(std::min(remaining, kBluetoothWriteChunkSize));
    const int sent = send(socket_handle, reinterpret_cast<const char*>(bytes.data() + offset), chunk_size, 0);
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
  const bool connected = bluetooth_sockets_.find(normalized_address) != bluetooth_sockets_.end();
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

void FlutterThermalPrinterPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto arguments = std::get_if<EncodableMap>(method_call.arguments());
  const auto is_bluetooth_classic = arguments != nullptr &&
      StringValue(*arguments, "connectionType") == "BLUETOOTH_CLASSIC";

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
    result->Success(EncodableValue(ConnectBluetoothClassic(StringValue(*arguments, "address"))));
  } else if (method_call.method_name().compare("printText") == 0 && is_bluetooth_classic) {
    result->Success(EncodableValue(PrintBluetoothClassic(
        StringValue(*arguments, "address"), BytesValue(*arguments, "data"))));
  } else if (method_call.method_name().compare("isConnected") == 0 && is_bluetooth_classic) {
    result->Success(EncodableValue(IsBluetoothClassicConnected(StringValue(*arguments, "address"))));
  } else if (method_call.method_name().compare("disconnect") == 0 && is_bluetooth_classic) {
    result->Success(EncodableValue(DisconnectBluetoothClassic(StringValue(*arguments, "address"))));
  } else {
    result->NotImplemented();
  }
}

}  // namespace flutter_thermal_printer

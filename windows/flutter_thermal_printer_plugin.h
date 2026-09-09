#ifndef FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flutter_thermal_printer {

class FlutterThermalPrinterPlugin : public flutter::Plugin {
 public:
  /// Registers the method channel and transfers plugin ownership to Flutter.
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  /// Initializes the Windows transport layer and Winsock.
  FlutterThermalPrinterPlugin();

  /// Cancels active output and closes every retained Bluetooth socket.
  virtual ~FlutterThermalPrinterPlugin();

  /// The plugin owns worker state and native handles, so copying is unsafe.
  FlutterThermalPrinterPlugin(const FlutterThermalPrinterPlugin&) = delete;
  FlutterThermalPrinterPlugin& operator=(const FlutterThermalPrinterPlugin&) = delete;

  /// Handles transport and print commands from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

 private:
  /// Shared cancellation flag observed by the active print worker.
  using CancellationToken = std::shared_ptr<std::atomic_bool>;
  /// Native print operation executed outside the Flutter method-channel thread.
  using PrintOperation = std::function<bool(const CancellationToken&)>;

  /// Opens or reuses an RFCOMM socket, waiting asynchronously for connection completion.
  bool ConnectBluetoothClassic(const std::string& address, const CancellationToken& cancellation);
  /// Writes printer bytes in bounded chunks while honoring cancellation.
  bool PrintBluetoothClassic(const std::string& address,
                             const std::vector<uint8_t>& bytes,
                             const CancellationToken& cancellation);
  /// Checks the retained RFCOMM socket and removes stale handles.
  bool IsBluetoothClassicConnected(const std::string& address);
  /// Closes and removes the retained RFCOMM socket for an address.
  bool DisconnectBluetoothClassic(const std::string& address);
  /// Verifies that a raw USB printer path can be opened for writing.
  bool CanOpenUsbPrinter(const std::string& device_path);
  /// Writes USB printer bytes with overlapped I/O and a bounded wait.
  static bool PrintUsbPrinter(const std::string& device_path,
                              const std::vector<uint8_t>& bytes,
                              const CancellationToken& cancellation);
  /// Requests cancellation of the current print worker.
  void CancelPrint();
  /// Runs one print operation off the method-channel thread and completes its result.
  void StartPrintWorker(
      PrintOperation operation,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  std::unordered_map<std::string, SOCKET> bluetooth_sockets_;
  std::mutex bluetooth_sockets_mutex_;
  std::mutex print_worker_mutex_;
  std::thread print_worker_;
  std::shared_ptr<std::atomic_bool> print_worker_done_;
  CancellationToken print_worker_cancellation_;
  std::atomic<int64_t> print_operation_id_ = 0;
};

}  // namespace flutter_thermal_printer

#endif  // FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_

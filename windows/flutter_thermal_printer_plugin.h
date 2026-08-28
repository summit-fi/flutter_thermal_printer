#ifndef FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flutter_thermal_printer {

class FlutterThermalPrinterPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  FlutterThermalPrinterPlugin();

  virtual ~FlutterThermalPrinterPlugin();

  // Disallow copy and assign.
  FlutterThermalPrinterPlugin(const FlutterThermalPrinterPlugin&) = delete;
  FlutterThermalPrinterPlugin& operator=(const FlutterThermalPrinterPlugin&) = delete;

  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

 private:
  bool ConnectBluetoothClassic(const std::string& address);
  bool PrintBluetoothClassic(const std::string& address, const std::vector<uint8_t>& bytes);
  bool IsBluetoothClassicConnected(const std::string& address);
  bool DisconnectBluetoothClassic(const std::string& address);
  bool CanOpenUsbPrinter(const std::string& device_path);
  bool PrintUsbPrinter(const std::string& device_path, const std::vector<uint8_t>& bytes);

  std::unordered_map<std::string, SOCKET> bluetooth_sockets_;
  std::mutex bluetooth_sockets_mutex_;
};

}  // namespace flutter_thermal_printer

#endif  // FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_

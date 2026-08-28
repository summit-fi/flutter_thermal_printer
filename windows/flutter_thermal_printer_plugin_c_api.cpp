#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>

#include "include/flutter_thermal_printer/flutter_thermal_printer_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "flutter_thermal_printer_plugin.h"

void FlutterThermalPrinterPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  flutter_thermal_printer::FlutterThermalPrinterPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}

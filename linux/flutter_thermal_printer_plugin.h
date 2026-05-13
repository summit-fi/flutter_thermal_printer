#ifndef FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define FLUTTER_PLUGIN_EXPORT
#endif

// GObject type declaration — required before G_DEFINE_TYPE in the .cc file.
#define FLUTTER_THERMAL_PRINTER_TYPE_PLUGIN \
  (flutter_thermal_printer_plugin_get_type())

G_DECLARE_FINAL_TYPE(FlutterThermalPrinterPlugin,
                     flutter_thermal_printer_plugin,
                     FLUTTER_THERMAL_PRINTER,
                     PLUGIN,
                     GObject)

FLUTTER_PLUGIN_EXPORT void flutter_thermal_printer_plugin_register_with_registrar(
    FlPluginRegistrar* registrar);

G_END_DECLS

#endif  // FLUTTER_PLUGIN_FLUTTER_THERMAL_PRINTER_PLUGIN_H_

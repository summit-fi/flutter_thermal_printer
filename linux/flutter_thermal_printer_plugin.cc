// FLUTTER_PLUGIN_IMPL is defined by CMake via target_compile_definitions.
// Do NOT redefine it here — that would cause a -Werror,-Wmacro-redefined error.
#include "flutter_thermal_printer_plugin.h"
#include "include/flutter_thermal_printer/flutter_thermal_printer_plugin.h"

#include <cups/cups.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

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
                         size_t data_len) {
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

  // Parent: wait for child to finish.
  int status = 0;
  waitpid(pid, &status, 0);
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
  // For CUPS, "connecting" just means checking that the queue exists.
  } else if (strcmp(method, "connect") == 0) {
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
      if (printer_name.empty()) {
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
            // Primary: lp -o raw (bypasses all CUPS driver filters).
            // Fallback: CUPS C API (may be filtered by printer driver).
            bool ok = lp_print_raw(printer_name.c_str(),
                                   data_bytes.data(),
                                   data_bytes.size());
            if (!ok) {
              ok = cups_print_raw(printer_name.c_str(),
                                  data_bytes.data(),
                                  data_bytes.size());
            }
            if (ok) {
              g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
              response =
                  FL_METHOD_RESPONSE(fl_method_success_response_new(result));
            } else {
              const char* err = cupsLastErrorString();
              response = FL_METHOD_RESPONSE(fl_method_error_response_new(
                  "PRINT_ERROR",
                  err ? err : "Failed to submit CUPS print job", nullptr));
            }
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

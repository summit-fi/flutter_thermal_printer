package com.example.flutter_thermal_printer;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import androidx.annotation.NonNull;

import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import io.flutter.embedding.engine.plugins.FlutterPlugin;
import io.flutter.plugin.common.MethodCall;
import io.flutter.plugin.common.MethodChannel;
import io.flutter.plugin.common.MethodChannel.MethodCallHandler;
import io.flutter.plugin.common.MethodChannel.Result;
import io.flutter.plugin.common.EventChannel;

/** FlutterThermalPrinterPlugin */
public class FlutterThermalPrinterPlugin implements FlutterPlugin, MethodCallHandler {
  /// The MethodChannel that will the communication between Flutter and native Android
  ///
  /// This local reference serves to register the plugin with the Flutter Engine and unregister it
  /// when the Flutter Engine is detached from the Activity
  private MethodChannel channel;
  private EventChannel eventChannel;
  private Context context;
  private UsbPrinter usbPrinter;
  private BluetoothClassicPrinter bluetoothClassicPrinter;
  private final ExecutorService executor = Executors.newSingleThreadExecutor();
  private final Handler mainHandler = new Handler(Looper.getMainLooper());

  @Override
  public void onAttachedToEngine(@NonNull FlutterPluginBinding flutterPluginBinding) {
    channel = new MethodChannel(flutterPluginBinding.getBinaryMessenger(), "flutter_thermal_printer");
    eventChannel = new EventChannel(flutterPluginBinding.getBinaryMessenger(), "flutter_thermal_printer/events");
    channel.setMethodCallHandler(this);
    context = flutterPluginBinding.getApplicationContext();
    usbPrinter = new UsbPrinter(context); 
    bluetoothClassicPrinter = new BluetoothClassicPrinter(context);
    eventChannel.setStreamHandler(usbPrinter);
  }

  @Override
  public void onMethodCall(@NonNull MethodCall call, @NonNull Result result) {
      switch (call.method) {
          case "getPlatformVersion":
              result.success("Android " + android.os.Build.VERSION.RELEASE);
              break;
          case "getUsbDevicesList":
              result.success(usbPrinter.getUsbDevicesList());
              break;
          case "connect": {
              if (isBluetoothClassic(call)) {
                  String address = call.argument("address");
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.connect(address));
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  result.success(usbPrinter.connect(vendorId, productId));
              }
              break;
          }
          case "disconnect": {
              if (isBluetoothClassic(call)) {
                  String address = call.argument("address");
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.disconnect(address));
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  result.success(usbPrinter.disconnect(vendorId, productId));
              }
              break;
          }
          case "printText": {
              List<Integer> data = call.argument("data");
              if (isBluetoothClassic(call)) {
                  String address = call.argument("address");
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.printText(address, data));
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  result.success(usbPrinter.printText(vendorId, productId, data));
              }
              break;
          }
          case "isConnected": {
              if (isBluetoothClassic(call)) {
                  String address = call.argument("address");
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.isConnected(address));
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  result.success(usbPrinter.isConnected(vendorId, productId));
              }
              break;
          }
          default:
              result.notImplemented();
              break;
      }
  }

  @Override
  public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
    channel.setMethodCallHandler(null);
    eventChannel.setStreamHandler(null);
    executor.shutdownNow();
  }

  private boolean isBluetoothClassic(MethodCall call) {
      String connectionType = call.argument("connectionType");
      return "BLUETOOTH_CLASSIC".equals(connectionType);
  }

  private void runBooleanAsync(Result result, Callable<Boolean> task) {
      executor.execute(() -> {
          try {
              Boolean success = task.call();
              mainHandler.post(() -> result.success(success));
          } catch (Exception error) {
              mainHandler.post(() -> result.error("BLUETOOTH_CLASSIC_ERROR", error.getMessage(), null));
          }
      });
  }
}

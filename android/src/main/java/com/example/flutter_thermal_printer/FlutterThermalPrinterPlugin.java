package com.example.flutter_thermal_printer;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import androidx.annotation.NonNull;

import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

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
    private final ScheduledExecutorService timeoutExecutor = Executors.newSingleThreadScheduledExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final AtomicLong operationIds = new AtomicLong();
    private static final long OPERATION_TIMEOUT_MS = 30_000L;

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
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.connect(address), "BLUETOOTH_CONNECT_FAILED", "bluetoothClassic.connect", bluetoothClassicPrinter::cancelActiveOperation, true);
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  runBooleanAsync(result, () -> usbPrinter.connect(vendorId, productId), "USB_OPEN_FAILED", "usb.connect", usbPrinter::cancelActiveOperation, true);
              }
              break;
          }
          case "disconnect": {
              if (isBluetoothClassic(call)) {
                  String address = call.argument("address");
                  bluetoothClassicPrinter.cancelActiveOperation();
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.disconnect(address), "TRANSPORT_DISCONNECT_FAILED", "bluetoothClassic.disconnect", bluetoothClassicPrinter::cancelActiveOperation, true);
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  runBooleanAsync(result, () -> usbPrinter.disconnect(vendorId, productId), "TRANSPORT_DISCONNECT_FAILED", "usb.disconnect", usbPrinter::cancelActiveOperation, true);
              }
              break;
          }
          case "printText": {
              List<Integer> data = call.argument("data");
              if (isBluetoothClassic(call)) {
                  String address = call.argument("address");
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.printText(address, data), "BLUETOOTH_WRITE_FAILED", "bluetoothClassic.print", bluetoothClassicPrinter::cancelActiveOperation, true);
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  runBooleanAsync(result, () -> usbPrinter.printText(vendorId, productId, data), "USB_WRITE_FAILED", "usb.print", usbPrinter::cancelActiveOperation, true);
              }
              break;
          }
          case "isConnected": {
              if (isBluetoothClassic(call)) {
                  String address = call.argument("address");
                  runBooleanAsync(result, () -> bluetoothClassicPrinter.isConnected(address), "BLUETOOTH_CONNECT_FAILED", "bluetoothClassic.status", bluetoothClassicPrinter::cancelActiveOperation, false);
              } else {
                  String vendorId = call.argument("vendorId");
                  String productId = call.argument("productId");
                  runBooleanAsync(result, () -> usbPrinter.isConnected(vendorId, productId), "USB_OPEN_FAILED", "usb.status", usbPrinter::cancelActiveOperation, false);
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
    bluetoothClassicPrinter.closeAll();
    usbPrinter.cancelActiveOperation();
    executor.shutdownNow();
    timeoutExecutor.shutdownNow();
  }

  private boolean isBluetoothClassic(MethodCall call) {
      String connectionType = call.argument("connectionType");
      return "BLUETOOTH_CLASSIC".equals(connectionType);
  }

  private void runBooleanAsync(Result result, Callable<Boolean> task, String errorCode, String operation, Runnable cancel, boolean errorOnFalse) {
      long operationId = operationIds.incrementAndGet();
      long startedAt = System.nanoTime();
      AtomicBoolean completed = new AtomicBoolean(false);
      AtomicReference<Future<?>> workerReference = new AtomicReference<>();
      android.util.Log.d("FPP", "operation.started id=" + operationId
              + " platform=android transport=" + transportFor(operation)
              + " operation=" + operation + " stage=started");
      ScheduledFuture<?> timeout = timeoutExecutor.schedule(() -> {
          if (!completed.compareAndSet(false, true)) return;
          Future<?> worker = workerReference.get();
          if (worker != null) worker.cancel(true);
          cancel.run();
          android.util.Log.e("FPP", "operation.timeout id=" + operationId
              + " platform=android transport=" + transportFor(operation)
                      + " operation=" + operation + " stage=timedOut errorCode=TRANSPORT_TIMEOUT"
                  + " durationMs=" + ((System.nanoTime() - startedAt) / 1_000_000L));
          mainHandler.post(() -> result.error("TRANSPORT_TIMEOUT", "Transport operation timed out", null));
      }, OPERATION_TIMEOUT_MS, TimeUnit.MILLISECONDS);
      Future<?> worker = executor.submit(() -> {
          if (completed.get()) {
              android.util.Log.d("FPP", "operation.cancelled_before_start id=" + operationId
                      + " platform=android transport=" + transportFor(operation)
                      + " operation=" + operation + " stage=cancelled");
              return;
          }
          try {
              Boolean success = task.call();
              if (!completed.compareAndSet(false, true)) {
                  android.util.Log.d("FPP", "operation.late_result_ignored id=" + operationId
                          + " platform=android transport=" + transportFor(operation)
                          + " operation=" + operation + " stage=stale");
                  return;
              }
              long durationMs = (System.nanoTime() - startedAt) / 1_000_000L;
              String completionCode = success
                      ? "SUCCESS"
                      : (errorOnFalse ? errorCode : "NONE");
              android.util.Log.d("FPP", "operation.completed id=" + operationId
                      + " platform=android transport=" + transportFor(operation)
                      + " operation=" + operation + " stage=" + (success ? "completed" : "failed")
                      + " success=" + success
                      + " errorCode=" + completionCode
                      + " durationMs=" + durationMs);
              if (!success && errorOnFalse) {
                  mainHandler.post(() -> result.error(errorCode, "Native printer transport operation failed", null));
              } else {
                  mainHandler.post(() -> result.success(success));
              }
          } catch (Exception error) {
              if (!completed.compareAndSet(false, true)) {
                  android.util.Log.d("FPP", "operation.late_error_ignored id=" + operationId
                          + " platform=android transport=" + transportFor(operation)
                          + " operation=" + operation + " stage=stale");
                  return;
              }
              long durationMs = (System.nanoTime() - startedAt) / 1_000_000L;
              android.util.Log.e("FPP", "operation.failed id=" + operationId
                      + " platform=android transport=" + transportFor(operation)
                      + " operation=" + operation + " stage=failed errorCode=" + errorCode
                      + " durationMs=" + durationMs, error);
              mainHandler.post(() -> result.error(errorCode, error.getMessage(), null));
          } finally {
              timeout.cancel(false);
          }
      });
      workerReference.set(worker);
      if (completed.get()) worker.cancel(true);
  }

  private String transportFor(String operation) {
      return operation.startsWith("usb.") ? "usb" : "bluetoothClassic";
  }
}

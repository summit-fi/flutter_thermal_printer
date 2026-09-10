import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_thermal_printer/flutter_thermal_printer_platform_interface.dart';
import 'package:flutter_thermal_printer/printer_manager.dart';
import 'package:flutter_thermal_printer/utils/ble_config.dart';
import 'package:flutter_thermal_printer/utils/printer.dart';

import '../mocks/mock_platform.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('PrinterManager', () {
    late MockFlutterThermalPrinterPlatform mockPlatform;
    late FlutterThermalPrinterPlatform originalPlatform;

    setUp(() {
      originalPlatform = FlutterThermalPrinterPlatform.instance;
      mockPlatform = MockFlutterThermalPrinterPlatform();
      FlutterThermalPrinterPlatform.instance = mockPlatform;
    });

    tearDown(() {
      FlutterThermalPrinterPlatform.instance = originalPlatform;
      mockPlatform.reset();
    });

    group('singleton', () {
      test('instance returns same object', () {
        final instance1 = PrinterManager.instance;
        final instance2 = PrinterManager.instance;

        expect(identical(instance1, instance2), true);
      });

      test('instance is PrinterManager type', () {
        expect(PrinterManager.instance, isA<PrinterManager>());
      });
    });

    group('devicesStream', () {
      test('is a broadcast stream', () {
        final stream = PrinterManager.instance.devicesStream;
        expect(stream.isBroadcast, true);
      });

      test('can have multiple listeners', () {
        final stream = PrinterManager.instance.devicesStream;

        final sub1 = stream.listen((_) {});
        final sub2 = stream.listen((_) {});

        expect(sub1, isNotNull);
        expect(sub2, isNotNull);

        sub1.cancel();
        sub2.cancel();
      });
    });

    group('isBleTurnedOnStream', () {
      test('returns a stream', () {
        expect(
          PrinterManager.instance.isBleTurnedOnStream,
          isA<Stream<bool>>(),
        );
      });
    });

    group('getPrinters', () {
      test('throws exception when connectionTypes is empty', () async {
        expect(
          () => PrinterManager.instance.getPrinters(connectionTypes: []),
          throwsException,
        );
      });

      test('throws with message about no connection type', () async {
        try {
          await PrinterManager.instance.getPrinters(connectionTypes: []);
          fail('Should have thrown');
        } catch (e) {
          expect(e.toString(), contains('No connection type provided'));
        }
      });
    });

    group('connect', () {
      test('returns false for BLE device with null address', () async {
        final printer = Printer(
          connectionType: ConnectionType.BLE,
        );

        final result = await PrinterManager.instance.connect(printer);
        expect(result, false);
      });

      test('returns false for unknown connection type', () async {
        final printer = Printer();

        final result = await PrinterManager.instance.connect(printer);
        expect(result, false);
      });
    });

    group('isConnected', () {
      test('returns false for BLE device with null address', () async {
        final printer = Printer(
          connectionType: ConnectionType.BLE,
        );

        final result = await PrinterManager.instance.isConnected(printer);
        expect(result, false);
      });

      test('returns false for unknown connection type', () async {
        final printer = Printer();

        final result = await PrinterManager.instance.isConnected(printer);
        expect(result, false);
      });
    });

    group('disconnect', () {
      test('handles BLE device with null address gracefully', () async {
        final printer = Printer(
          connectionType: ConnectionType.BLE,
        );

        await PrinterManager.instance.disconnect(printer);
      });

      test('does nothing for USB devices', () async {
        final printer = Printer(
          connectionType: ConnectionType.USB,
          vendorId: '1234',
          productId: '5678',
        );

        await PrinterManager.instance.disconnect(printer);
      });

      test('does nothing for unknown connection type', () async {
        final printer = Printer();

        await PrinterManager.instance.disconnect(printer);
      });
    });

    group('stopScan', () {
      test('can be called without error', () async {
        await PrinterManager.instance.stopScan();
      });

      test('can be called multiple times', () async {
        await PrinterManager.instance.stopScan();
        await PrinterManager.instance.stopScan();
        await PrinterManager.instance.stopScan();
      });

      test('can stop only BLE', () async {
        await PrinterManager.instance.stopScan(stopUsb: false);
      });

      test('can stop only USB', () async {
        await PrinterManager.instance.stopScan(stopBle: false);
      });
    });

    group('bleConfig', () {
      test('has default config with zero stabilization delay', () {
        final config = PrinterManager.instance.bleConfig;
        expect(
          config.connectionStabilizationDelay,
          Duration.zero,
        );
      });

      test('bleConfig setter updates the configuration', () {
        final originalConfig = PrinterManager.instance.bleConfig;

        PrinterManager.instance.bleConfig =
            const BleConfig(connectionStabilizationDelay: Duration(seconds: 3));

        final newConfig = PrinterManager.instance.bleConfig;
        expect(
          newConfig.connectionStabilizationDelay,
          const Duration(seconds: 3),
        );

        PrinterManager.instance.bleConfig = originalConfig;
      });

      test('bleConfig getter returns current config', () {
        final config1 = PrinterManager.instance.bleConfig;
        final config2 = PrinterManager.instance.bleConfig;
        expect(
          config1.connectionStabilizationDelay,
          config2.connectionStabilizationDelay,
        );
      });

      test('config changes persist across accesses', () {
        final originalConfig = PrinterManager.instance.bleConfig;

        PrinterManager.instance.bleConfig =
            const BleConfig(connectionStabilizationDelay: Duration(seconds: 7));

        expect(
          PrinterManager.instance.bleConfig.connectionStabilizationDelay,
          const Duration(seconds: 7),
        );
        expect(
          PrinterManager.instance.bleConfig.connectionStabilizationDelay,
          const Duration(seconds: 7),
        );

        PrinterManager.instance.bleConfig = originalConfig;
      });
    });
  });
}

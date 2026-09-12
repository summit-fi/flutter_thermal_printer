import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_thermal_printer/flutter_thermal_printer_method_channel.dart';
import 'package:flutter_thermal_printer/flutter_thermal_printer_platform_interface.dart';
import 'package:flutter_thermal_printer/utils/printer.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockFlutterThermalPrinterPlatform extends FlutterThermalPrinterPlatform
    with MockPlatformInterfaceMixin {
  @override
  Future<String?> getPlatformVersion() async => 'Mock Platform';

  @override
  Future<void> cancelConnect() async {}

  @override
  Future<void> cancelPrint() async {}
}

class InvalidPlatform extends FlutterThermalPrinterPlatform {}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('FlutterThermalPrinterPlatform', () {
    late FlutterThermalPrinterPlatform originalInstance;

    setUp(() {
      originalInstance = FlutterThermalPrinterPlatform.instance;
    });

    tearDown(() {
      FlutterThermalPrinterPlatform.instance = originalInstance;
    });

    test('default instance is MethodChannelFlutterThermalPrinter', () {
      expect(
        FlutterThermalPrinterPlatform.instance,
        isA<MethodChannelFlutterThermalPrinter>(),
      );
    });

    test('can set custom instance with valid token', () {
      final mockPlatform = MockFlutterThermalPrinterPlatform();
      FlutterThermalPrinterPlatform.instance = mockPlatform;

      expect(FlutterThermalPrinterPlatform.instance, mockPlatform);
    });

    test('InvalidPlatform can be created but lacks MockPlatformInterfaceMixin',
        () {
      final invalidPlatform = InvalidPlatform();
      expect(invalidPlatform, isA<FlutterThermalPrinterPlatform>());
    });

    group('unimplemented methods throw UnimplementedError', () {
      late FlutterThermalPrinterPlatform basePlatform;

      setUp(() {
        basePlatform = MockFlutterThermalPrinterPlatform();
      });

      test('startUsbScan throws UnimplementedError', () async {
        expect(
          () => basePlatform.startUsbScan(),
          throwsA(isA<UnimplementedError>()),
        );
      });

      test('connect throws UnimplementedError', () async {
        final printer = Printer();
        expect(
          () => basePlatform.connect(printer),
          throwsA(isA<UnimplementedError>()),
        );
      });

      test('printText throws UnimplementedError', () async {
        final printer = Printer();
        final data = Uint8List.fromList([1, 2, 3]);
        expect(
          () => basePlatform.printText(printer, data),
          throwsA(isA<UnimplementedError>()),
        );
      });

      test('isConnected throws UnimplementedError', () async {
        final printer = Printer();
        expect(
          () => basePlatform.isConnected(printer),
          throwsA(isA<UnimplementedError>()),
        );
      });

      test('convertImageToGrayscale throws UnimplementedError', () async {
        final data = Uint8List.fromList([1, 2, 3]);
        expect(
          () => basePlatform.convertImageToGrayscale(data),
          throwsA(isA<UnimplementedError>()),
        );
      });

      test('disconnect throws UnimplementedError', () async {
        final printer = Printer();
        expect(
          () => basePlatform.disconnect(printer),
          throwsA(isA<UnimplementedError>()),
        );
      });

      test('stopScan throws UnimplementedError', () async {
        expect(
          () => basePlatform.stopScan(),
          throwsA(isA<UnimplementedError>()),
        );
      });

      test('getPrinters throws UnimplementedError', () async {
        expect(
          () => basePlatform.getPrinters(),
          throwsA(isA<UnimplementedError>()),
        );
      });
    });

    group('base class getPlatformVersion', () {
      test('throws UnimplementedError by default', () {
        final basePlatform = _BasePlatformForTest();
        expect(
          basePlatform.getPlatformVersion,
          throwsA(isA<UnimplementedError>()),
        );
      });
    });
  });
}

class _BasePlatformForTest extends FlutterThermalPrinterPlatform
    with MockPlatformInterfaceMixin {}

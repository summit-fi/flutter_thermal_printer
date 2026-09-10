import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_thermal_printer/utils/ble_config.dart';

void main() {
  group('BleConfig', () {
    group('constructor', () {
      test('uses zero connectionStabilizationDelay by default', () {
        const config = BleConfig();
        expect(
          config.connectionStabilizationDelay,
          Duration.zero,
        );
      });

      test('accepts custom connectionStabilizationDelay', () {
        const config = BleConfig(
          connectionStabilizationDelay: Duration(seconds: 5),
        );
        expect(config.connectionStabilizationDelay, const Duration(seconds: 5));
      });

      test('accepts zero duration', () {
        const config = BleConfig(
          connectionStabilizationDelay: Duration.zero,
        );
        expect(config.connectionStabilizationDelay, Duration.zero);
      });

      test('accepts milliseconds precision', () {
        const config = BleConfig(
          connectionStabilizationDelay: Duration(milliseconds: 500),
        );
        expect(
          config.connectionStabilizationDelay,
          const Duration(milliseconds: 500),
        );
      });

      test('is const constructible', () {
        const config1 = BleConfig();
        const config2 = BleConfig();
        expect(identical(config1, config2), isTrue);
      });
    });

    group('copyWith', () {
      test('returns new instance with updated connectionStabilizationDelay',
          () {
        const original = BleConfig(
          // ignore: avoid_redundant_argument_values
          connectionStabilizationDelay: Duration(seconds: 10),
        );
        final copied = original.copyWith(
          connectionStabilizationDelay: const Duration(seconds: 3),
        );

        expect(
          copied.connectionStabilizationDelay,
          const Duration(seconds: 3),
        );
        expect(
          original.connectionStabilizationDelay,
          const Duration(seconds: 10),
        );
      });

      test('preserves value when null passed', () {
        const original = BleConfig(
          connectionStabilizationDelay: Duration(seconds: 7),
        );
        final copied = original.copyWith();

        expect(
          copied.connectionStabilizationDelay,
          const Duration(seconds: 7),
        );
      });

      test('returns different instance', () {
        const original = BleConfig();
        final copied = original.copyWith();

        expect(identical(original, copied), isFalse);
      });
    });

    group('toString', () {
      test('includes connectionStabilizationDelay', () {
        const config = BleConfig(
          connectionStabilizationDelay: Duration(seconds: 5),
        );
        final result = config.toString();

        expect(result, contains('BleConfig'));
        expect(result, contains('connectionStabilizationDelay'));
        expect(result, contains('0:00:05'));
      });

      test('formats default duration correctly', () {
        const config = BleConfig();
        final result = config.toString();

        expect(result, contains('0:00:00'));
      });
    });

    group('equality', () {
      test('const instances with same values are identical', () {
        const config1 = BleConfig(
          connectionStabilizationDelay: Duration(seconds: 5),
        );
        const config2 = BleConfig(
          connectionStabilizationDelay: Duration(seconds: 5),
        );

        expect(identical(config1, config2), isTrue);
      });

      test('default const instances are identical', () {
        const config1 = BleConfig();
        const config2 = BleConfig();

        expect(identical(config1, config2), isTrue);
      });
    });

    group('edge cases', () {
      test('handles very long duration', () {
        const config = BleConfig(
          connectionStabilizationDelay: Duration(hours: 1),
        );
        expect(config.connectionStabilizationDelay, const Duration(hours: 1));
      });

      test('handles microseconds', () {
        const config = BleConfig(
          connectionStabilizationDelay: Duration(microseconds: 100),
        );
        expect(
          config.connectionStabilizationDelay,
          const Duration(microseconds: 100),
        );
      });
    });
  });
}

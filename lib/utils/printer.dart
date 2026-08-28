// ignore_for_file: constant_identifier_names

import 'package:universal_ble/universal_ble.dart';

/// Optimized printer model with better data validation and serialization
class Printer extends BleDevice {
  Printer({
    this.address,
    this.name,
    this.connectionType,
    this.isConnected,
    this.vendorId,
    this.productId,
  }) : super(deviceId: address ?? '', name: name ?? '');

  /// Create Printer from JSON with validation
  factory Printer.fromJson(Map<String, dynamic> json) {
    try {
      return Printer(
        address: json['address'] as String?,
        name: json['name'] as String?,
        connectionType:
            _getConnectionTypeFromString(json['connectionType'] as String?),
        isConnected: json['isConnected'] as bool?,
        vendorId: json['vendorId']?.toString(),
        productId: json['productId']?.toString(),
      );
    } catch (e) {
      throw FormatException('Invalid Printer JSON format: $e');
    }
  }

  @override
  // ignore: overridden_fields
  final String? name;
  final String? address;
  final ConnectionType? connectionType;
  final bool? isConnected;
  final String? vendorId;
  final String? productId;

  /// Convert to JSON with proper formatting
  Map<String, dynamic> toJson() {
    final data = <String, dynamic>{};

    data['address'] = address;
    data['name'] = name;
    data['connectionType'] = connectionType?.name;
    data['isConnected'] = isConnected;
    data['vendorId'] = vendorId;
    data['productId'] = productId;

    return data;
  }

  /// Get human-readable connection type string
  String get connectionTypeString {
    switch (connectionType) {
      case ConnectionType.BLE:
        return 'BLE';
      case ConnectionType.USB:
        return 'USB';
      case ConnectionType.NETWORK:
        return 'NETWORK';
      case ConnectionType.BLUETOOTH_CLASSIC:
        return 'BLUETOOTH_CLASSIC';
      default:
        return 'UNKNOWN';
    }
  }

  /// Create a copy with updated fields
  Printer copyWith({
    String? address,
    String? name,
    ConnectionType? connectionType,
    bool? isConnected,
    String? vendorId,
    String? productId,
  }) =>
      Printer(
        address: address ?? this.address,
        name: name ?? this.name,
        connectionType: connectionType ?? this.connectionType,
        isConnected: isConnected ?? this.isConnected,
        vendorId: vendorId ?? this.vendorId,
        productId: productId ?? this.productId,
      );

  /// Generate unique identifier for the printer
  String get uniqueId {
    final buffer = StringBuffer();
    if (vendorId != null) {
      buffer.write(vendorId);
    }
    buffer.write('_');
    if (address != null) {
      buffer.write(address);
    }
    return buffer.toString();
  }

  /// Check if printer has valid connection data
  bool get hasValidConnectionData {
    switch (connectionType) {
      case ConnectionType.USB:
        return (vendorId != null && productId != null) ||
            (address?.startsWith(r'\\?\') ?? false);
      case ConnectionType.BLE:
        return address != null;
      case ConnectionType.NETWORK:
        return address != null;
      case ConnectionType.BLUETOOTH_CLASSIC:
        return address != null;
      default:
        return false;
    }
  }

  @override
  String toString() =>
      'Printer(name: $name, connectionType: ${connectionType?.name}, '
      'address: $address, isConnected: $isConnected)';

  /// Convert string to ConnectionType enum
  static ConnectionType? _getConnectionTypeFromString(String? type) {
    if (type == null) {
      return null;
    }

    switch (type.toUpperCase()) {
      case 'BLE':
        return ConnectionType.BLE;
      case 'USB':
        return ConnectionType.USB;
      case 'NETWORK':
        return ConnectionType.NETWORK;
      case 'BLUETOOTH_CLASSIC':
      case 'BT_CLASSIC':
      case 'CLASSIC':
        return ConnectionType.BLUETOOTH_CLASSIC;
      default:
        return null;
    }
  }
}

/// Enhanced connection type enum
enum ConnectionType {
  BLE,
  USB,
  NETWORK,
  BLUETOOTH_CLASSIC,
}

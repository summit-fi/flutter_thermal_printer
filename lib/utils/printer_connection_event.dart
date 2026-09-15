import 'printer.dart';

/// A native connection-state notification for a printer transport.
class PrinterConnectionEvent {
  const PrinterConnectionEvent({
    required this.connectionType,
    required this.address,
    required this.state,
  });

  factory PrinterConnectionEvent.fromJson(Map<String, dynamic> json) {
    final address = json['address'] as String?;
    if (address == null || address.trim().isEmpty) {
      throw const FormatException('Printer connection event has no address');
    }

    return PrinterConnectionEvent(
      connectionType: _connectionTypeFromWireValue(json['connectionType']),
      address: address,
      state: _stateFromWireValue(json['state']),
    );
  }

  final ConnectionType connectionType;
  final String address;
  final PrinterConnectionState state;

  static ConnectionType _connectionTypeFromWireValue(Object? value) {
    switch (value?.toString().toUpperCase()) {
      case 'BLUETOOTH_CLASSIC':
        return ConnectionType.BLUETOOTH_CLASSIC;
      case 'BLE':
        return ConnectionType.BLE;
      case 'USB':
        return ConnectionType.USB;
      case 'NETWORK':
        return ConnectionType.NETWORK;
      default:
        throw FormatException('Unsupported printer connection type: $value');
    }
  }

  static PrinterConnectionState _stateFromWireValue(Object? value) {
    switch (value?.toString().toLowerCase()) {
      case 'connected':
        return PrinterConnectionState.connected;
      case 'connecting':
        return PrinterConnectionState.connecting;
      case 'disconnected':
        return PrinterConnectionState.disconnected;
      case 'failed':
        return PrinterConnectionState.failed;
      default:
        return PrinterConnectionState.unknown;
    }
  }
}

enum PrinterConnectionState {
  connecting,
  connected,
  disconnected,
  failed,
  unknown
}

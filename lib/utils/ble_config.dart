class BleConfig {
  const BleConfig({
    this.connectionStabilizationDelay = const Duration(seconds: 10),
    this.connectionTimeout = const Duration(seconds: 30),
    this.printTimeout = const Duration(seconds: 30),
  });

  final Duration connectionStabilizationDelay;
  final Duration connectionTimeout;
  final Duration printTimeout;

  BleConfig copyWith({
    Duration? connectionStabilizationDelay,
    Duration? connectionTimeout,
    Duration? printTimeout,
  }) =>
      BleConfig(
        connectionStabilizationDelay:
            connectionStabilizationDelay ?? this.connectionStabilizationDelay,
        connectionTimeout: connectionTimeout ?? this.connectionTimeout,
        printTimeout: printTimeout ?? this.printTimeout,
      );

  @override
  String toString() =>
      'BleConfig(connectionStabilizationDelay: $connectionStabilizationDelay, '
      'connectionTimeout: $connectionTimeout, printTimeout: $printTimeout)';
}

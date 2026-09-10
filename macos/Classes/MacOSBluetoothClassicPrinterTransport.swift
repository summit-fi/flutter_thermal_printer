import Foundation
import IOBluetooth

final class MacOSBluetoothClassicPrinterTransport: NSObject, IOBluetoothRFCOMMChannelDelegate {
  private static let writeTimeout: TimeInterval = 30
  private static let connectionTimeout: TimeInterval = 30
  private var channelsByAddress = [String: IOBluetoothRFCOMMChannel]()
  private var pendingConnections = [String: MacOSBluetoothSdpQuery]()
  // Keep cancelled queries alive until IOBluetooth finishes its native
  // callback. Otherwise the Objective-C delegate can become dangling.
  private var cancelledConnections = [UUID: MacOSBluetoothSdpQuery]()
  private var pendingWrites = [String: PendingWrite]()

  func connect(address: String, completion: @escaping (Result<Void, Error>) -> Void) {
    let normalizedAddress = normalize(address)
    guard !normalizedAddress.isEmpty else {
      completion(.failure(MacOSBluetoothTransportError.invalidAddress))
      return
    }
    if let channel = channelsByAddress[normalizedAddress], channel.isOpen() {
      log("connect.reused address=\(normalizedAddress) mtu=\(channel.getMTU())")
      completion(.success(()))
      return
    }
    guard let device = IOBluetoothDevice(addressString: normalizedAddress) else {
      completion(.failure(MacOSBluetoothTransportError.deviceUnavailable(normalizedAddress)))
      return
    }
    guard device.isPaired() else {
      completion(.failure(MacOSBluetoothTransportError.deviceNotPaired(normalizedAddress)))
      return
    }

    disconnect(address: normalizedAddress)
    let queryID = UUID()
    let query = MacOSBluetoothSdpQuery(
      id: queryID,
      device: device,
      onCompleted: { [weak self] result in
        guard let self else { return }
        if self.pendingConnections[normalizedAddress]?.id == queryID {
          self.pendingConnections[normalizedAddress] = nil
        }
        switch result {
        case .failure(let error):
          self.log("sdp.failed address=\(normalizedAddress) error=\(error.localizedDescription)")
          completion(.failure(error))
        case .success(let channelId):
          self.openChannel(
            device: device,
            address: normalizedAddress,
            channelId: channelId,
            completion: completion
          )
        }
      },
      onNativeOperationFinished: { [weak self] in
        self?.cancelledConnections.removeValue(forKey: queryID)
      }
    )
    pendingConnections[normalizedAddress] = query
    query.start(timeout: Self.connectionTimeout)
    log("sdp.started address=\(normalizedAddress) timeoutSeconds=\(Int(Self.connectionTimeout))")
  }

  func isConnected(address: String) -> Bool {
    guard let channel = channelsByAddress[normalize(address)] else { return false }
    return channel.isOpen()
  }

  func disconnect(address: String) {
    let normalizedAddress = normalize(address)
    if let query = pendingConnections.removeValue(forKey: normalizedAddress) {
      cancelledConnections[query.id] = query
      query.cancel()
    }
    if let pending = pendingWrites.removeValue(forKey: normalizedAddress) {
      pending.timeoutWorkItem?.cancel()
      log("write.cancelled address=\(normalizedAddress) id=\(pending.id.uuidString) reason=disconnect")
      pending.completion(.failure(MacOSBluetoothTransportError.writeCancelled))
    }
    guard let channel = channelsByAddress.removeValue(forKey: normalizedAddress) else { return }
    let status = channel.close()
    log("disconnect.completed address=\(normalizedAddress) status=\(status)")
  }

  // Sends data asynchronously chunk-by-chunk using writeAsync + delegate callback.
  // Each chunk is sent only after the previous write completes, providing flow control
  // and avoiding RFCOMM buffer overflow that occurs with writeSync loops.
  func write(data: Data, address: String, completion: @escaping (Result<Void, Error>) -> Void) {
    let normalizedAddress = normalize(address)
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      guard let channel = self.channelsByAddress[normalizedAddress], channel.isOpen() else {
        completion(.failure(MacOSBluetoothTransportError.channelUnavailable(normalizedAddress)))
        return
      }
      guard self.pendingWrites[normalizedAddress] == nil else {
        completion(.failure(MacOSBluetoothTransportError.writeAlreadyInProgress))
        return
      }
      let mtu = max(1, Int(channel.getMTU()))
      let writeID = UUID()
      self.log("write.started address=\(normalizedAddress) bytes=\(data.count) mtu=\(mtu)")
      self.pendingWrites[normalizedAddress] = PendingWrite(
        id: writeID,
        nsData: data as NSData,
        chunkSize: min(mtu, 512),
        offset: 0,
        completion: completion,
        timeoutWorkItem: nil
      )
      let timeoutWorkItem = DispatchWorkItem { [weak self] in
        guard let self,
              let pending = self.pendingWrites[normalizedAddress],
              pending.id == writeID
        else { return }
        self.pendingWrites.removeValue(forKey: normalizedAddress)
        if let channel = self.channelsByAddress.removeValue(forKey: normalizedAddress) {
          _ = channel.close()
        }
        self.log("write.timeout address=\(normalizedAddress) id=\(writeID.uuidString)")
        pending.completion(.failure(MacOSBluetoothTransportError.writeTimeout))
      }
      self.pendingWrites[normalizedAddress]?.timeoutWorkItem = timeoutWorkItem
      DispatchQueue.main.asyncAfter(deadline: .now() + Self.writeTimeout, execute: timeoutWorkItem)
      self.sendNextChunk(address: normalizedAddress, channel: channel)
    }
  }

  // MARK: - Private

  private func sendNextChunk(address: String, channel: IOBluetoothRFCOMMChannel) {
    guard let pending = pendingWrites[address] else { return }

    if pending.offset >= pending.totalBytes {
      pendingWrites.removeValue(forKey: address)
      pending.timeoutWorkItem?.cancel()
      log("write.completed address=\(address) id=\(pending.id.uuidString) bytes=\(pending.totalBytes)")
      pending.completion(.success(()))
      return
    }

    let chunkLength = min(pending.chunkSize, pending.totalBytes - pending.offset)
    let ptr = pending.nsData.bytes.advanced(by: pending.offset)
    let status = channel.writeAsync(UnsafeMutableRawPointer(mutating: ptr), length: UInt16(chunkLength), refcon: nil)

    if status != kIOReturnSuccess {
      let failed = pendingWrites.removeValue(forKey: address)
      failed?.timeoutWorkItem?.cancel()
      log("write.failed address=\(address) id=\(failed?.id.uuidString ?? "unknown") offset=\(pending.offset) status=\(status)")
      failed?.completion(.failure(MacOSBluetoothTransportError.writeFailed(status)))
      return
    }

    // Advance offset now so the delegate callback sees the updated position.
    pendingWrites[address]?.offset += chunkLength
  }

  private func openChannel(
    device: IOBluetoothDevice,
    address: String,
    channelId: BluetoothRFCOMMChannelID,
    completion: @escaping (Result<Void, Error>) -> Void
  ) {
    var channel: IOBluetoothRFCOMMChannel?
    let status = device.openRFCOMMChannelSync(&channel, withChannelID: channelId, delegate: self)
    guard status == kIOReturnSuccess, let channel else {
      completion(.failure(MacOSBluetoothTransportError.channelOpenFailed(status)))
      return
    }
    channelsByAddress[address] = channel
    log("connect.completed address=\(address) channel=\(channelId) mtu=\(channel.getMTU())")
    completion(.success(()))
  }

  // MARK: - IOBluetoothRFCOMMChannelDelegate

  func rfcommChannelData(
    _ rfcommChannel: IOBluetoothRFCOMMChannel!,
    data dataPointer: UnsafeMutableRawPointer!,
    length dataLength: Int
  ) {
    log("channel.data_received channel=\(rfcommChannel.getID()) bytes=\(dataLength)")
  }

  func rfcommChannelWriteComplete(
    _ rfcommChannel: IOBluetoothRFCOMMChannel!,
    refcon: UnsafeMutableRawPointer!,
    status: IOReturn
  ) {
    DispatchQueue.main.async { [weak self] in
      guard let self,
            let entry = self.channelsByAddress.first(where: { $0.value === rfcommChannel })
      else { return }
      let address = entry.key

      if status != kIOReturnSuccess {
        if let failed = self.pendingWrites.removeValue(forKey: address) {
          failed.timeoutWorkItem?.cancel()
          self.log("write.failed address=\(address) id=\(failed.id.uuidString) status=\(status)")
          failed.completion(.failure(MacOSBluetoothTransportError.writeFailed(status)))
        }
        return
      }

      self.sendNextChunk(address: address, channel: rfcommChannel)
    }
  }

  func rfcommChannelClosed(_ rfcommChannel: IOBluetoothRFCOMMChannel!) {
    guard let entry = channelsByAddress.first(where: { $0.value === rfcommChannel }) else { return }
    channelsByAddress.removeValue(forKey: entry.key)
    if let failed = pendingWrites.removeValue(forKey: entry.key) {
      failed.timeoutWorkItem?.cancel()
      log("channel.closed address=\(entry.key) id=\(failed.id.uuidString) channel=\(rfcommChannel.getID()) write_interrupted=true")
      failed.completion(.failure(MacOSBluetoothTransportError.channelUnavailable(entry.key)))
    } else {
      log("channel.closed address=\(entry.key) channel=\(rfcommChannel.getID())")
    }
  }

  private func normalize(_ address: String) -> String {
    address.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
  }

  private func log(_ message: String) {
    NSLog("[FlutterThermalPrinterNative] platform=macos transport=bluetoothClassic \(message)")
  }
}

private struct PendingWrite {
  let id: UUID
  let nsData: NSData
  let chunkSize: Int
  var offset: Int
  let completion: (Result<Void, Error>) -> Void
  var timeoutWorkItem: DispatchWorkItem?
  var totalBytes: Int { nsData.length }
}

private final class MacOSBluetoothSdpQuery: NSObject {
  let id: UUID
  private let device: IOBluetoothDevice
  private let onCompleted: (Result<BluetoothRFCOMMChannelID, Error>) -> Void
  private let onNativeOperationFinished: () -> Void
  private var completed = false
  private var cancelled = false
  private var timeoutWorkItem: DispatchWorkItem?

  init(
    id: UUID,
    device: IOBluetoothDevice,
    onCompleted: @escaping (Result<BluetoothRFCOMMChannelID, Error>) -> Void,
    onNativeOperationFinished: @escaping () -> Void
  ) {
    self.id = id
    self.device = device
    self.onCompleted = onCompleted
    self.onNativeOperationFinished = onNativeOperationFinished
  }

  func start(timeout: TimeInterval) {
    timeoutWorkItem = DispatchWorkItem { [weak self] in
      self?.timeout()
    }
    if let timeoutWorkItem {
      DispatchQueue.main.asyncAfter(deadline: .now() + timeout, execute: timeoutWorkItem)
    }

    if device.isConnected() {
      beginSdpQuery()
      return
    }

    let connectionStatus = device.openConnection(self)
    NSLog(
      "[FlutterThermalPrinterNative] macos.bluetooth baseband.started address=\(device.addressString ?? "unknown") " +
        "status=\(connectionStatus)"
    )
    if connectionStatus != kIOReturnSuccess {
      complete(.failure(MacOSBluetoothTransportError.basebandStartFailed(connectionStatus)))
    }
  }

  @objc(connectionComplete:status:)
  func connectionComplete(_ device: IOBluetoothDevice!, status: IOReturn) {
    NSLog("[FlutterThermalPrinterNative] macos.bluetooth baseband.completed status=\(status)")
    defer { onNativeOperationFinished() }
    guard !cancelled else { return }
    guard status == kIOReturnSuccess else {
      complete(.failure(MacOSBluetoothTransportError.basebandConnectionFailed(status)))
      return
    }
    beginSdpQuery()
  }

  private func beginSdpQuery() {
    let status = device.performSDPQuery(self)
    NSLog("[FlutterThermalPrinterNative] macos.bluetooth sdp.requested status=\(status)")
    if status != kIOReturnSuccess {
      complete(.failure(MacOSBluetoothTransportError.sdpStartFailed(status)))
    }
  }

  func cancel() {
    finishCancellation(
      error: MacOSBluetoothTransportError.connectionCancelled,
      reason: "cancelled"
    )
  }

  private func timeout() {
    finishCancellation(
      error: MacOSBluetoothTransportError.connectionTimeout,
      reason: "timeout"
    )
  }

  private func finishCancellation(error: Error, reason: String) {
    guard !cancelled, !completed else { return }
    cancelled = true
    // Abort the native baseband/SDP operation. The transport retains this
    // query until the late native callback arrives.
    _ = device.closeConnection()
    NSLog(
      "[FlutterThermalPrinterNative] macos.bluetooth query.cancelled " +
        "address=\(device.addressString ?? "unknown") reason=\(reason)"
    )
    complete(.failure(error))
  }

  @objc(sdpQueryComplete:status:)
  func sdpQueryComplete(_ sender: IOBluetoothDevice!, status: IOReturn) {
    NSLog("[FlutterThermalPrinterNative] macos.bluetooth sdp.callback status=\(status)")
    defer { onNativeOperationFinished() }
    guard !cancelled else { return }
    guard status == kIOReturnSuccess else {
      complete(.failure(MacOSBluetoothTransportError.sdpQueryFailed(status)))
      return
    }
    let records = (device.services as? [IOBluetoothSDPServiceRecord]) ?? []
    var channels = [(id: BluetoothRFCOMMChannelID, isSerialPortProfile: Bool)]()
    NSLog("[FlutterThermalPrinterNative] macos.bluetooth sdp.completed services=\(records.count)")
    for record in records {
      var channelId: BluetoothRFCOMMChannelID = 0
      if record.getRFCOMMChannelID(&channelId) == kIOReturnSuccess {
        let isSerialPortProfile = record.matchesUUID16(0x1101)
        let serviceName = record.getServiceName() ?? "unknown"
        NSLog(
          "[FlutterThermalPrinterNative] macos.bluetooth sdp.rfcomm_service " +
            "name=\(serviceName) channel=\(channelId) serial_port_profile=\(isSerialPortProfile)"
        )
        channels.append((id: channelId, isSerialPortProfile: isSerialPortProfile))
      }
    }
    guard let channel = channels.first(where: \.isSerialPortProfile) ?? channels.first else {
      complete(.failure(MacOSBluetoothTransportError.serialPortProfileUnavailable))
      return
    }
    complete(.success(channel.id))
  }

  private func complete(_ result: Result<BluetoothRFCOMMChannelID, Error>) {
    guard !completed else { return }
    completed = true
    timeoutWorkItem?.cancel()
    timeoutWorkItem = nil
    onCompleted(result)
  }
}

private enum MacOSBluetoothTransportError: LocalizedError {
  case invalidAddress
  case deviceUnavailable(String)
  case deviceNotPaired(String)
  case basebandStartFailed(IOReturn)
  case basebandConnectionFailed(IOReturn)
  case sdpStartFailed(IOReturn)
  case sdpQueryFailed(IOReturn)
  case connectionTimeout
  case serialPortProfileUnavailable
  case channelOpenFailed(IOReturn)
  case channelUnavailable(String)
  case writeFailed(IOReturn)
  case writeTimeout
  case writeCancelled
  case writeAlreadyInProgress
  case connectionCancelled

  var errorDescription: String? {
    switch self {
    case .invalidAddress:
      return "Bluetooth printer address is missing."
    case .deviceUnavailable(let address):
      return "Bluetooth printer \(address) is unavailable."
    case .deviceNotPaired(let address):
      return "Bluetooth printer \(address) is not paired."
    case .basebandStartFailed(let status):
      return "Unable to start the Bluetooth connection: \(status)."
    case .basebandConnectionFailed(let status):
      return "Unable to connect to the Bluetooth printer: \(status)."
    case .sdpStartFailed(let status):
      return "Unable to start Bluetooth service discovery: \(status)."
    case .sdpQueryFailed(let status):
      return "Bluetooth service discovery failed: \(status)."
    case .serialPortProfileUnavailable:
      return "The printer does not expose a Bluetooth serial port service."
    case .channelOpenFailed(let status):
      return "Unable to open the Bluetooth serial channel: \(status)."
    case .channelUnavailable(let address):
      return "Bluetooth printer \(address) is not connected."
    case .writeFailed(let status):
      return "Unable to send data to the Bluetooth printer: \(status)."
    case .writeTimeout:
      return "Bluetooth printer write timed out."
    case .writeCancelled:
      return "Bluetooth printer write was cancelled."
    case .writeAlreadyInProgress:
      return "A Bluetooth write operation is already in progress."
    case .connectionCancelled:
      return "Bluetooth connection was cancelled."
    case .connectionTimeout:
      return "Bluetooth connection timed out."
    }
  }
}

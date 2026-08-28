import Foundation
import IOBluetooth

final class MacOSBluetoothClassicPrinterTransport: NSObject {
  private var channelsByAddress = [String: IOBluetoothRFCOMMChannel]()
  private var pendingConnections = [String: MacOSBluetoothSdpQuery]()

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
    let query = MacOSBluetoothSdpQuery(
      device: device,
      onCompleted: { [weak self] result in
        guard let self else { return }
        self.pendingConnections[normalizedAddress] = nil
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
      }
    )
    pendingConnections[normalizedAddress] = query
    query.start()
    log("sdp.started address=\(normalizedAddress)")
  }

  func isConnected(address: String) -> Bool {
    guard let channel = channelsByAddress[normalize(address)] else { return false }
    return channel.isOpen()
  }

  func disconnect(address: String) {
    let normalizedAddress = normalize(address)
    pendingConnections.removeValue(forKey: normalizedAddress)?.cancel()
    guard let channel = channelsByAddress.removeValue(forKey: normalizedAddress) else { return }
    let status = channel.close()
    log("disconnect.completed address=\(normalizedAddress) status=\(status)")
  }

  func write(data: Data, address: String) -> Result<Void, Error> {
    let normalizedAddress = normalize(address)
    guard let channel = channelsByAddress[normalizedAddress], channel.isOpen() else {
      return .failure(MacOSBluetoothTransportError.channelUnavailable(normalizedAddress))
    }

    let mtu = max(1, Int(channel.getMTU()))
    let chunkSize = min(mtu, 512)
    log("write.started address=\(normalizedAddress) bytes=\(data.count) mtu=\(mtu) chunkSize=\(chunkSize)")
    var offset = 0
    while offset < data.count {
      let end = min(offset + chunkSize, data.count)
      let chunk = data.subdata(in: offset..<end)
      let status = chunk.withUnsafeBytes { buffer in
        channel.writeSync(UnsafeMutableRawPointer(mutating: buffer.baseAddress), length: UInt16(chunk.count))
      }
      guard status == kIOReturnSuccess else {
        log("write.failed address=\(normalizedAddress) offset=\(offset) status=\(status)")
        return .failure(MacOSBluetoothTransportError.writeFailed(status))
      }
      offset = end
    }
    log("write.completed address=\(normalizedAddress) bytes=\(data.count)")
    return .success(())
  }

  private func openChannel(
    device: IOBluetoothDevice,
    address: String,
    channelId: BluetoothRFCOMMChannelID,
    completion: @escaping (Result<Void, Error>) -> Void
  ) {
    var channel: IOBluetoothRFCOMMChannel?
    let status = device.openRFCOMMChannelSync(&channel, withChannelID: channelId, delegate: nil)
    guard status == kIOReturnSuccess, let channel else {
      completion(.failure(MacOSBluetoothTransportError.channelOpenFailed(status)))
      return
    }
    channelsByAddress[address] = channel
    log("connect.completed address=\(address) channel=\(channelId) mtu=\(channel.getMTU())")
    completion(.success(()))
  }

  private func normalize(_ address: String) -> String {
    address.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
  }

  private func log(_ message: String) {
    NSLog("[FlutterThermalPrinterNative] macos.bluetooth \(message)")
  }
}

private final class MacOSBluetoothSdpQuery: NSObject {
  private let device: IOBluetoothDevice
  private let onCompleted: (Result<BluetoothRFCOMMChannelID, Error>) -> Void
  private var completed = false

  init(
    device: IOBluetoothDevice,
    onCompleted: @escaping (Result<BluetoothRFCOMMChannelID, Error>) -> Void
  ) {
    self.device = device
    self.onCompleted = onCompleted
  }

  func start() {
    let status = device.performSDPQuery(self)
    if status != kIOReturnSuccess {
      complete(.failure(MacOSBluetoothTransportError.sdpStartFailed(status)))
    }
  }

  func cancel() {
    complete(.failure(MacOSBluetoothTransportError.connectionCancelled))
  }

  @objc func sdpQueryComplete(_ sender: IOBluetoothDevice!, status: IOReturn) {
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
    onCompleted(result)
  }
}

private enum MacOSBluetoothTransportError: LocalizedError {
  case invalidAddress
  case deviceUnavailable(String)
  case deviceNotPaired(String)
  case sdpStartFailed(IOReturn)
  case sdpQueryFailed(IOReturn)
  case serialPortProfileUnavailable
  case channelOpenFailed(IOReturn)
  case channelUnavailable(String)
  case writeFailed(IOReturn)
  case connectionCancelled

  var errorDescription: String? {
    switch self {
    case .invalidAddress:
      return "Bluetooth printer address is missing."
    case .deviceUnavailable(let address):
      return "Bluetooth printer \(address) is unavailable."
    case .deviceNotPaired(let address):
      return "Bluetooth printer \(address) is not paired."
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
    case .connectionCancelled:
      return "Bluetooth connection was cancelled."
    }
  }
}

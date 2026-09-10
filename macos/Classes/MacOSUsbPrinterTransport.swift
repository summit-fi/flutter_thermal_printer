import Foundation
import IOKit
import IOKit.usb
import IOKit.usb.IOUSBLib

final class MacOSUsbPrinterTransport {
  private static let operationTimeoutNanoseconds: UInt64 = 30 * 1_000_000_000
  private static let pipeTimeoutMilliseconds: UInt32 = 10_000

  func canOpen(address: String) -> Bool {
    perform(address: address, data: nil)
  }

  func write(data: Data, address: String) -> Bool {
    guard !data.isEmpty else { return true }
    return perform(address: address, data: data)
  }

  private func perform(address: String, data: Data?) -> Bool {
    guard let locationId = locationId(from: address) else {
      log("operation.failed reason=invalid_address address=\(address)")
      return false
    }
    guard let service = findDevice(locationId: locationId) else {
      log("operation.failed reason=device_not_found locationId=\(format(locationId))")
      return false
    }
    defer { IOObjectRelease(service) }

    guard let device = openDevice(service: service) else { return false }
    defer { device.close() }

    guard let interface = device.openPrinterInterface() else { return false }
    defer { interface.close() }

    guard data != nil else {
      log("open.success locationId=\(format(locationId))")
      return true
    }

    return interface.write(data!)
  }

  private func findDevice(locationId: UInt32) -> io_service_t? {
    guard let matching = IOServiceMatching(kIOUSBDeviceClassName) else { return nil }
    var iterator: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iterator) == KERN_SUCCESS else {
      return nil
    }
    defer { IOObjectRelease(iterator) }

    var service = IOIteratorNext(iterator)
    while service != 0 {
      if deviceLocationId(service) == locationId {
        return service
      }
      IOObjectRelease(service)
      service = IOIteratorNext(iterator)
    }
    return nil
  }

  private func deviceLocationId(_ service: io_service_t) -> UInt32? {
    var properties: Unmanaged<CFMutableDictionary>?
    guard IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0) == KERN_SUCCESS,
          let dictionary = properties?.takeRetainedValue() as? [String: Any],
          let number = dictionary[kUSBDevicePropertyLocationID as String] as? NSNumber
    else {
      return nil
    }
    return UInt32(exactly: number.uint64Value)
  }

  private func openDevice(service: io_service_t) -> OpenUsbDevice? {
    var pluginPointer: UnsafeMutablePointer<UnsafeMutablePointer<IOCFPlugInInterface>?>?
    var score: Int32 = 0
    let pluginResult = IOCreatePlugInInterfaceForService(
      service,
      MacUsbIokitIds.deviceUserClientType,
      MacUsbIokitIds.pluginInterface,
      &pluginPointer,
      &score
    )
    guard pluginResult == KERN_SUCCESS,
          let pluginPointer,
          let plugin = pluginPointer.pointee?.pointee
    else {
      log("device.open.failed stage=plugin result=\(pluginResult)")
      return nil
    }

    var devicePointer: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBDeviceInterface>?>?
    let queryResult = withUnsafeMutablePointer(to: &devicePointer) { output in
      output.withMemoryRebound(to: Optional<LPVOID>.self, capacity: 1) {
        plugin.QueryInterface(pluginPointer, CFUUIDGetUUIDBytes(MacUsbIokitIds.deviceInterface), $0)
      }
    }
    guard queryResult == S_OK,
          let devicePointer,
          let device = devicePointer.pointee?.pointee
    else {
      _ = plugin.Release(pluginPointer)
      log("device.open.failed stage=query result=\(queryResult)")
      return nil
    }

    let openResult = device.USBDeviceOpen(devicePointer)
    guard openResult == KERN_SUCCESS else {
      _ = device.Release(devicePointer)
      _ = plugin.Release(pluginPointer)
      log("device.open.failed stage=open result=\(openResult)")
      return nil
    }

    log("device.open.success")
    return OpenUsbDevice(devicePointer: devicePointer, pluginPointer: pluginPointer)
  }

  private func locationId(from address: String) -> UInt32? {
    let normalized = address.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
    guard normalized.hasPrefix("usb:"), let value = UInt32(normalized.dropFirst(4), radix: 16) else {
      return nil
    }
    return value
  }

  private func format(_ value: UInt32) -> String {
    String(format: "%08X", value)
  }

  private func log(_ message: String) {
    NSLog("[FlutterThermalPrinterNative] platform=macos transport=usb \(message)")
  }
}

private final class OpenUsbDevice {
  private let devicePointer: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBDeviceInterface>?>
  private let pluginPointer: UnsafeMutablePointer<UnsafeMutablePointer<IOCFPlugInInterface>?>
  private var isClosed = false

  init(
    devicePointer: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBDeviceInterface>?>,
    pluginPointer: UnsafeMutablePointer<UnsafeMutablePointer<IOCFPlugInInterface>?>
  ) {
    self.devicePointer = devicePointer
    self.pluginPointer = pluginPointer
  }

  deinit { close() }

  func openPrinterInterface() -> OpenUsbPrinterInterface? {
    guard let device = devicePointer.pointee?.pointee else { return nil }
    var request = IOUSBFindInterfaceRequest(
      bInterfaceClass: UInt16(kUSBPrintingInterfaceClass),
      bInterfaceSubClass: UInt16(kIOUSBFindInterfaceDontCare),
      bInterfaceProtocol: UInt16(kIOUSBFindInterfaceDontCare),
      bAlternateSetting: UInt16(kIOUSBFindInterfaceDontCare)
    )
    var iterator: io_iterator_t = 0
    guard device.CreateInterfaceIterator(devicePointer, &request, &iterator) == KERN_SUCCESS else {
      return nil
    }
    defer { IOObjectRelease(iterator) }
    let service = IOIteratorNext(iterator)
    guard service != 0 else { return nil }
    defer { IOObjectRelease(service) }

    var pluginPointer: UnsafeMutablePointer<UnsafeMutablePointer<IOCFPlugInInterface>?>?
    var score: Int32 = 0
    let pluginResult = IOCreatePlugInInterfaceForService(
      service,
      MacUsbIokitIds.interfaceUserClientType,
      MacUsbIokitIds.pluginInterface,
      &pluginPointer,
      &score
    )
    guard pluginResult == KERN_SUCCESS,
          let pluginPointer,
          let plugin = pluginPointer.pointee?.pointee
    else {
      return nil
    }

    var interfacePointer: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBInterfaceInterface>?>?
    let queryResult = withUnsafeMutablePointer(to: &interfacePointer) { output in
      output.withMemoryRebound(to: Optional<LPVOID>.self, capacity: 1) {
        plugin.QueryInterface(pluginPointer, CFUUIDGetUUIDBytes(MacUsbIokitIds.interfaceInterface), $0)
      }
    }
    guard queryResult == S_OK,
          let interfacePointer,
          let interface = interfacePointer.pointee?.pointee
    else {
      _ = plugin.Release(pluginPointer)
      return nil
    }

    let openResult = interface.USBInterfaceOpen(interfacePointer)
    guard openResult == KERN_SUCCESS else {
      _ = interface.Release(interfacePointer)
      _ = plugin.Release(pluginPointer)
      NSLog("[FlutterThermalPrinterNative] macos.usb interface.open.failed result=\(openResult)")
      return nil
    }

    return OpenUsbPrinterInterface(interfacePointer: interfacePointer, pluginPointer: pluginPointer)
  }

  func close() {
    guard !isClosed else { return }
    isClosed = true
    guard let device = devicePointer.pointee?.pointee else { return }
    _ = device.USBDeviceClose(devicePointer)
    _ = device.Release(devicePointer)
    if let plugin = pluginPointer.pointee?.pointee {
      _ = plugin.Release(pluginPointer)
    }
  }
}

private final class OpenUsbPrinterInterface {
  private let interfacePointer: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBInterfaceInterface>?>
  private let pluginPointer: UnsafeMutablePointer<UnsafeMutablePointer<IOCFPlugInInterface>?>
  private var isClosed = false

  init(
    interfacePointer: UnsafeMutablePointer<UnsafeMutablePointer<IOUSBInterfaceInterface>?>,
    pluginPointer: UnsafeMutablePointer<UnsafeMutablePointer<IOCFPlugInInterface>?>
  ) {
    self.interfacePointer = interfacePointer
    self.pluginPointer = pluginPointer
  }

  deinit { close() }

  func write(_ data: Data) -> Bool {
    guard let interface = interfacePointer.pointee?.pointee, let pipe = bulkOutPipe() else {
      NSLog("[FlutterThermalPrinterNative] macos.usb write.failed reason=bulk_out_pipe_missing")
      return false
    }

    let chunkSize = 4_096
    var offset = 0
    let startedAt = DispatchTime.now().uptimeNanoseconds
    while offset < data.count {
      let elapsed = DispatchTime.now().uptimeNanoseconds - startedAt
      guard elapsed < Self.operationTimeoutNanoseconds else {
        NSLog("[FlutterThermalPrinterNative] macos.usb write.timeout bytes=\(data.count) offset=\(offset)")
        return false
      }

      let length = min(chunkSize, data.count - offset)
      let remainingMilliseconds = UInt32(
        min(
          UInt64(Self.pipeTimeoutMilliseconds),
          (Self.operationTimeoutNanoseconds - elapsed) / 1_000_000
        )
      )
      guard remainingMilliseconds > 0 else {
        NSLog("[FlutterThermalPrinterNative] macos.usb write.timeout bytes=\(data.count) offset=\(offset)")
        return false
      }
      let result = data.withUnsafeBytes { buffer in
        interface.WritePipeTO(
          interfacePointer,
          pipe,
          UnsafeMutableRawPointer(mutating: buffer.baseAddress!.advanced(by: offset)),
          UInt32(length),
          remainingMilliseconds,
          remainingMilliseconds
        )
      }
      guard result == KERN_SUCCESS else {
        NSLog("[FlutterThermalPrinterNative] macos.usb write.failed offset=\(offset) result=\(result)")
        return false
      }
      offset += length
    }

    NSLog("[FlutterThermalPrinterNative] macos.usb write.success bytes=\(data.count)")
    return true
  }

  func close() {
    guard !isClosed else { return }
    isClosed = true
    guard let interface = interfacePointer.pointee?.pointee else { return }
    _ = interface.USBInterfaceClose(interfacePointer)
    _ = interface.Release(interfacePointer)
    if let plugin = pluginPointer.pointee?.pointee {
      _ = plugin.Release(pluginPointer)
    }
  }

  private func bulkOutPipe() -> UInt8? {
    guard let interface = interfacePointer.pointee?.pointee else { return nil }
    var endpointCount: UInt8 = 0
    guard interface.GetNumEndpoints(interfacePointer, &endpointCount) == KERN_SUCCESS else { return nil }

    for pipe in 1...endpointCount {
      var direction: UInt8 = 0
      var number: UInt8 = 0
      var transferType: UInt8 = 0
      var maxPacketSize: UInt16 = 0
      var interval: UInt8 = 0
      guard interface.GetPipeProperties(
        interfacePointer,
        pipe,
        &direction,
        &number,
        &transferType,
        &maxPacketSize,
        &interval
      ) == KERN_SUCCESS else {
        continue
      }
      if direction == UInt8(kUSBOut), transferType == UInt8(kUSBBulk) {
        return pipe
      }
    }
    return nil
  }
}

private enum MacUsbIokitIds {
  static let deviceUserClientType = CFUUIDGetConstantUUIDWithBytes(
    nil, 0x9D, 0xC7, 0xB7, 0x80, 0x9E, 0xC0, 0x11, 0xD4,
    0xA5, 0x4F, 0x00, 0x0A, 0x27, 0x05, 0x28, 0x61
  )
  static let interfaceUserClientType = CFUUIDGetConstantUUIDWithBytes(
    nil, 0x2D, 0x97, 0x86, 0xC6, 0x9E, 0xF3, 0x11, 0xD4,
    0xAD, 0x51, 0x00, 0x0A, 0x27, 0x05, 0x28, 0x61
  )
  static let pluginInterface = CFUUIDGetConstantUUIDWithBytes(
    nil, 0xC2, 0x44, 0xE8, 0x58, 0x10, 0x9C, 0x11, 0xD4,
    0x91, 0xD4, 0x00, 0x50, 0xE4, 0xC6, 0x42, 0x6F
  )
  static let deviceInterface = CFUUIDGetConstantUUIDWithBytes(
    nil, 0x5C, 0x81, 0x87, 0xD0, 0x9E, 0xF3, 0x11, 0xD4,
    0x8B, 0x45, 0x00, 0x0A, 0x27, 0x05, 0x28, 0x61
  )
  static let interfaceInterface = CFUUIDGetConstantUUIDWithBytes(
    nil, 0x73, 0xC9, 0x7A, 0xE8, 0x9E, 0xF3, 0x11, 0xD4,
    0xB1, 0xD0, 0x00, 0x0A, 0x27, 0x05, 0x28, 0x61
  )
}

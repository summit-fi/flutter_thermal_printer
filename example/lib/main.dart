// ignore_for_file: depend_on_referenced_packages

import 'dart:async';
import 'dart:developer';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_thermal_printer/flutter_thermal_printer.dart';
import 'package:flutter_thermal_printer/utils/printer.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  final _flutterThermalPrinterPlugin = FlutterThermalPrinter.instance;

  String _ip = '192.168.0.100';
  String _port = '9100';
  final _bluetoothAddressController = TextEditingController();
  final _bluetoothNameController = TextEditingController(text: 'Bluetooth printer');

  List<Printer> printers = [];

  StreamSubscription<List<Printer>>? _devicesStreamSubscription;
  bool _isBluetoothConnected = false;
  String? _bluetoothMessage;

  // Get Printer List
  void startScan() async {
    _devicesStreamSubscription?.cancel();
    await _flutterThermalPrinterPlugin.getPrinters(connectionTypes: [
      ConnectionType.USB,
      ConnectionType.BLE,
    ]);
    _devicesStreamSubscription = _flutterThermalPrinterPlugin.devicesStream.listen((List<Printer> event) {
      setState(() {
        printers = event;
        printers.removeWhere((element) =>
            element.name == null ||
            element.name == '' ||
            element.name!.toLowerCase().contains("print") == false);
      });
    });
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((timeStamp) {
      _flutterThermalPrinterPlugin.bleConfig = const BleConfig(
        connectionStabilizationDelay: Duration(seconds: 3),
      );
      startScan();
    });
  }

  stopScan() {
    _flutterThermalPrinterPlugin.stopScan();
  }

  Printer get _bluetoothPrinter => Printer(
        address: _bluetoothAddressController.text.trim(),
        name: _bluetoothNameController.text.trim(),
        connectionType: ConnectionType.BLUETOOTH_CLASSIC,
      );

  Future<void> _connectBluetoothPrinter() async {
    final address = _bluetoothAddressController.text.trim();
    if (address.isEmpty) {
      setState(() => _bluetoothMessage = 'Enter the Bluetooth address first.');
      return;
    }
    log('[FlutterThermalPrinterExample] bluetooth.connect.started address=$address');
    final connected = await _flutterThermalPrinterPlugin.connect(_bluetoothPrinter);
    if (!mounted) return;
    setState(() {
      _isBluetoothConnected = connected;
      _bluetoothMessage = connected ? 'Connected.' : 'Unable to open the Bluetooth serial channel.';
    });
    log('[FlutterThermalPrinterExample] bluetooth.connect.completed address=$address connected=$connected');
  }

  Future<void> _printBluetoothReceipt() async {
    final address = _bluetoothAddressController.text.trim();
    if (address.isEmpty) {
      setState(() => _bluetoothMessage = 'Enter the Bluetooth address first.');
      return;
    }
    log('[FlutterThermalPrinterExample] bluetooth.print.started address=$address');
    try {
      final printed = await _flutterThermalPrinterPlugin.printData(
        _bluetoothPrinter,
        await _generateReceipt(type: 'Bluetooth Classic'),
        longData: true,
      );
      if (!mounted) return;
      setState(
        () => _bluetoothMessage = printed
            ? 'Print command sent. Check the printer.'
            : 'The printer did not accept the print command.',
      );
      log('[FlutterThermalPrinterExample] bluetooth.print.completed address=$address printed=$printed');
    } catch (error) {
      if (!mounted) return;
      setState(() => _bluetoothMessage = 'Print failed: $error');
      log('[FlutterThermalPrinterExample] bluetooth.print.failed address=$address error=$error');
    }
  }

  Future<void> _disconnectBluetoothPrinter() async {
    final address = _bluetoothAddressController.text.trim();
    if (address.isEmpty) return;
    await _flutterThermalPrinterPlugin.disconnect(_bluetoothPrinter);
    if (!mounted) return;
    setState(() {
      _isBluetoothConnected = false;
      _bluetoothMessage = 'Disconnected.';
    });
    log('[FlutterThermalPrinterExample] bluetooth.disconnect.completed address=$address');
  }

  @override
  void dispose() {
    _devicesStreamSubscription?.cancel();
    _bluetoothAddressController.dispose();
    _bluetoothNameController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(
          title: const Text('Plugin example app'),
          systemOverlayStyle: const SystemUiOverlayStyle(
            statusBarColor: Colors.transparent,
          ),
        ),
        body: Padding(
          padding: const EdgeInsets.all(20),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Text(
                'NETWORK',
                style: Theme.of(context).textTheme.titleLarge,
              ),
              const SizedBox(height: 12),
              TextFormField(
                initialValue: _ip,
                decoration: const InputDecoration(
                  labelText: 'Enter IP Address',
                ),
                onChanged: (value) {
                  _ip = value;
                },
              ),
              const SizedBox(height: 12),
              TextFormField(
                initialValue: _port,
                decoration: const InputDecoration(
                  labelText: 'Enter Port',
                ),
                onChanged: (value) {
                  _port = value;
                },
              ),
              const SizedBox(height: 12),
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                children: [
                  Expanded(
                    child: ElevatedButton(
                      onPressed: () async {
                        final service = FlutterThermalPrinterNetwork(
                          _ip,
                          port: int.parse(_port),
                        );
                        await service.connect();
                        final profile = await CapabilityProfile.load();
                        final generator = Generator(PaperSize.mm80, profile);
                        List<int> bytes = [];
                        if (context.mounted) {
                          bytes = await FlutterThermalPrinter.instance.screenShotWidget(
                            context,
                            generator: generator,
                            widget: receiptWidget("Network"),
                          );
                          bytes += generator.cut();
                          await service.printTicket(bytes);
                        }
                        await service.disconnect();
                      },
                      child: const Text('Test network printer'),
                    ),
                  ),
                  const SizedBox(width: 22),
                  Expanded(
                    child: ElevatedButton(
                      onPressed: () async {
                        final service = FlutterThermalPrinterNetwork(_ip, port: int.parse(_port));
                        await service.connect();
                        final bytes = await _generateReceipt();
                        await service.printTicket(bytes);
                        await service.disconnect();
                      },
                      child: const Text('Test network printer widget'),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 12),
              const Divider(),
              const SizedBox(height: 22),
              Text(
                'BLUETOOTH CLASSIC',
                style: Theme.of(context).textTheme.titleLarge,
              ),
              const SizedBox(height: 12),
              TextField(
                controller: _bluetoothAddressController,
                decoration: const InputDecoration(labelText: 'Bluetooth address'),
              ),
              const SizedBox(height: 12),
              TextField(
                controller: _bluetoothNameController,
                decoration: const InputDecoration(labelText: 'Printer name'),
              ),
              const SizedBox(height: 12),
              Wrap(
                spacing: 12,
                runSpacing: 12,
                children: [
                  ElevatedButton(
                    onPressed: _connectBluetoothPrinter,
                    child: const Text('Connect'),
                  ),
                  ElevatedButton(
                    onPressed: _isBluetoothConnected ? _printBluetoothReceipt : null,
                    child: const Text('Test receipt'),
                  ),
                  ElevatedButton(
                    onPressed: _isBluetoothConnected ? _disconnectBluetoothPrinter : null,
                    child: const Text('Disconnect'),
                  ),
                ],
              ),
              if (_bluetoothMessage != null) ...[
                const SizedBox(height: 12),
                Text(_bluetoothMessage!),
              ],
              const SizedBox(height: 22),
              const Divider(),
              const SizedBox(height: 22),
              Text(
                'USB/BLE',
                style: Theme.of(context).textTheme.titleLarge,
              ),
              const SizedBox(height: 22),
              Row(
                children: [
                  Expanded(
                    child: ElevatedButton(
                      onPressed: () {
                        // startScan();
                        startScan();
                      },
                      child: const Text('Get Printers'),
                    ),
                  ),
                  const SizedBox(width: 22),
                  Expanded(
                    child: ElevatedButton(
                      onPressed: () {
                        // startScan();
                        stopScan();
                      },
                      child: const Text('Stop Scan'),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 12),
              Expanded(
                child: ListView.builder(
                  itemCount: printers.length,
                  itemBuilder: (context, index) {
                    return ListTile(
                      onTap: () async {
                        if (printers[index].isConnected ?? false) {
                          await _flutterThermalPrinterPlugin.disconnect(printers[index]);
                        } else {
                          await _flutterThermalPrinterPlugin.connect(printers[index]);
                        }
                      },
                      title: Text(printers[index].name ?? 'No Name'),
                      subtitle: Text("Connected: ${printers[index].isConnected ?? false}"),
                      trailing: IconButton(
                        icon: const Icon(Icons.connect_without_contact),
                        onPressed: () async {
                          // final data = await _generateReceipt(
                          //   type: printers[index].connectionTypeString,
                          // );
                          // await _flutterThermalPrinterPlugin.printData(
                          //   printers[index],
                          //   data,
                          //   longData: true,
                          // );

                          await _flutterThermalPrinterPlugin.printWidget(
                            context,
                            printOnBle: true,
                            cutAfterPrinted: true,
                            printer: printers[index],
                            widget: receiptWidget(
                              printers[index].connectionTypeString,
                            ),
                          );
                        },
                      ),
                    );
                  },
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Future<List<int>> _generateReceipt({String? type}) async {
    final profile = await CapabilityProfile.load();
    final generator = Generator(PaperSize.mm80, profile);
    List<int> bytes = [];
    bytes += generator.text(
      'FLUTTER THERMAL PRINTER',
      styles: const PosStyles(
        align: PosAlign.center,
        bold: true,
        height: PosTextSize.size2,
        width: PosTextSize.size2,
      ),
    );
    bytes += generator.hr();
    bytes += generator.row([
      PosColumn(
        text: 'Item',
        width: 6,
        styles: const PosStyles(bold: true),
      ),
      PosColumn(
        text: 'Price',
        width: 6,
        styles: const PosStyles(align: PosAlign.right, bold: true),
      ),
    ]);
    bytes += generator.hr();
    bytes += generator.row([
      PosColumn(text: 'Apple', width: 6),
      PosColumn(text: '\$1.00', width: 6, styles: const PosStyles(align: PosAlign.right)),
    ]);
    bytes += generator.row([
      PosColumn(text: 'Banana', width: 6),
      PosColumn(text: '\$0.50', width: 6, styles: const PosStyles(align: PosAlign.right)),
    ]);
    bytes += generator.row([
      PosColumn(text: 'Orange', width: 6),
      PosColumn(text: '\$0.75', width: 6, styles: const PosStyles(align: PosAlign.right)),
    ]);
    bytes += generator.hr();
    bytes += generator.row([
      PosColumn(
        text: 'Total',
        width: 6,
        styles: const PosStyles(bold: true),
      ),
      PosColumn(
        text: '\$2.25',
        width: 6,
        styles: const PosStyles(align: PosAlign.right, bold: true),
      ),
    ]);
    bytes += generator.feed(1);
    bytes += generator.text(
      'Printer Type: ${type ?? "Unknown"}',
      styles: const PosStyles(align: PosAlign.left),
    );
    bytes += generator.feed(2);
    bytes += generator.text(
      'Thank you for your purchase!',
      styles: const PosStyles(
        align: PosAlign.center,
      ),
    );

    bytes += generator.cut();
    return bytes;
  }

  Widget receiptWidget(String printerType) {
    log("Date1: ${DateTime.now()}");
    final widget = SizedBox(
      width: 550,
      child: Material(
        child: Padding(
          padding: const EdgeInsets.all(16.0),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Center(
                child: Text(
                  'FLUTTER THERMAL PRINTER',
                  style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold),
                ),
              ),
              const Divider(thickness: 2),
              const SizedBox(height: 10),
              _buildReceiptRow('Item', 'Price'),
              const Divider(),
              _buildReceiptRow('Apple', '\$1.00'),
              _buildReceiptRow('Banana', '\$0.50'),
              _buildReceiptRow('Orange', '\$0.75'),
              const Divider(thickness: 2),
              _buildReceiptRow('Total', '\$2.25', isBold: true),
              const SizedBox(height: 20),
              _buildReceiptRow('Printer Type', printerType),
              const SizedBox(height: 50),
              const Center(
                child: Text(
                  'Thank you for your purchase!',
                  style: TextStyle(fontSize: 16, fontStyle: FontStyle.italic),
                ),
              ),
            ],
          ),
        ),
      ),
    );

    log("Date1: ${DateTime.now()}");
    return widget;
  }
}

Widget _buildReceiptRow(String leftText, String rightText, {bool isBold = false}) {
  return Padding(
    padding: const EdgeInsets.symmetric(vertical: 4.0),
    child: Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Text(
          leftText,
          style: TextStyle(fontSize: 16, fontWeight: isBold ? FontWeight.bold : FontWeight.normal),
        ),
        Text(
          rightText,
          style: TextStyle(fontSize: 16, fontWeight: isBold ? FontWeight.bold : FontWeight.normal),
        ),
      ],
    ),
  );
}

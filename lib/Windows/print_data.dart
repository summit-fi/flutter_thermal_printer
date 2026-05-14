// Sends RAW data (string or hex sequences) directly to the printer

// Example taken from:
// https://learn.microsoft.com/windows/win32/printdocs/sending-data-directly-to-a-printer

import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:win32/win32.dart';

class RawPrinter {
  RawPrinter(this.printerName, this.alloc);
  final String printerName;
  final Arena alloc;

  void printEscPosWin32(List<int> data) {
    final hPrinter = calloc<HANDLE>();
    final docInfo = calloc<DOC_INFO_1>();

    final printerNamePtr = printerName.toNativeUtf16();
    final docNamePtr = 'ESC/POS Print Job'.toNativeUtf16();
    // Force RAW datatype so the spooler bypasses the printer driver's
    // GDI conversion path. Without this, drivers (esp. Star line-mode and
    // POS Class drivers) can mangle Star Graphics / ESC/POS bytes.
    final dataTypePtr = 'RAW'.toNativeUtf16();

    docInfo.ref.pDocName = docNamePtr;
    docInfo.ref.pOutputFile = nullptr;
    docInfo.ref.pDatatype = dataTypePtr;

    if (OpenPrinter(printerNamePtr, hPrinter, nullptr) != 0) {
      final printerHandle = hPrinter.value;

      if (StartDocPrinter(printerHandle, 1, docInfo.cast()) != 0) {
        StartPagePrinter(printerHandle);

        final buffer = Uint8List.fromList(data);
        final bytesWritten = calloc<DWORD>();

        WritePrinter(
          printerHandle,
          buffer.allocatePointer(),
          buffer.length,
          bytesWritten,
        );

        EndPagePrinter(printerHandle);
        EndDocPrinter(printerHandle);
      }

      ClosePrinter(printerHandle);
    }

    calloc
      ..free(printerNamePtr)
      ..free(docNamePtr)
      ..free(dataTypePtr)
      ..free(hPrinter)
      ..free(docInfo);
  }
}

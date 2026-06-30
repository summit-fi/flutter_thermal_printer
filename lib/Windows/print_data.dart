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
    final hPrinter = calloc<Pointer>();
    final docInfo = calloc<DOC_INFO_1>();

    final printerNamePtr = printerName.toNativeUtf16();
    final docNamePtr = 'ESC/POS Print Job'.toNativeUtf16();
    // Force RAW datatype so the spooler bypasses the printer driver's
    // GDI conversion path. Without this, drivers (esp. Star line-mode and
    // POS Class drivers) can mangle Star Graphics / ESC/POS bytes.
    final dataTypePtr = 'RAW'.toNativeUtf16();

    docInfo.ref.pDocName = PWSTR(docNamePtr);
    docInfo.ref.pOutputFile = PWSTR(nullptr);
    docInfo.ref.pDatatype = PWSTR(dataTypePtr);

    if (OpenPrinter(PCWSTR(printerNamePtr), hPrinter, null).value) {
      final printerHandle = PRINTER_HANDLE(hPrinter.value);

      if (StartDocPrinter(printerHandle, 1, docInfo) != 0) {
        StartPagePrinter(printerHandle);

        final buffer = Uint8List.fromList(data);
        final bytesWritten = calloc<DWORD>();
        final nativeBuffer = calloc<Uint8>(buffer.length);
        nativeBuffer.asTypedList(buffer.length).setAll(0, buffer);

        WritePrinter(
          printerHandle,
          nativeBuffer,
          buffer.length,
          bytesWritten,
        );

        calloc
          ..free(nativeBuffer)
          ..free(bytesWritten);

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

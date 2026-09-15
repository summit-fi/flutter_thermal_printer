/// Stable result returned by a native printer operation.
final class PrinterOperationResult {
  const PrinterOperationResult.success()
      : isSuccess = true,
        errorCode = 'SUCCESS',
        message = null;

  const PrinterOperationResult.failure({
    required this.errorCode,
    this.message,
  }) : isSuccess = false;

  final bool isSuccess;
  final String errorCode;
  final String? message;
}

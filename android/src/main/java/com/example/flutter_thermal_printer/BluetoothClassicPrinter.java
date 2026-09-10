package com.example.flutter_thermal_printer;

import android.Manifest;
import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.util.Log;

import java.io.IOException;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

public class BluetoothClassicPrinter {
    private static final String TAG = "FPP";
    private static final UUID SPP_UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final long OPERATION_TIMEOUT_MS = 30_000L;

    private final Context context;
    private final Map<String, BluetoothSocket> sockets = new HashMap<>();
    private final ScheduledExecutorService timeoutExecutor = Executors.newSingleThreadScheduledExecutor();
    private volatile BluetoothSocket activeSocket;

    BluetoothClassicPrinter(Context context) {
        this.context = context.getApplicationContext();
    }

    public synchronized boolean connect(String address) {
        if (!hasBluetoothConnectPermission()) {
            Log.d(TAG, "Cannot connect Bluetooth Classic printer. BLUETOOTH_CONNECT is not granted.");
            return false;
        }
        if (address == null || address.trim().isEmpty()) {
            Log.d(TAG, "Cannot connect Bluetooth Classic printer. Address is empty.");
            return false;
        }

        BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            Log.d(TAG, "Cannot connect Bluetooth Classic printer. Bluetooth adapter is unavailable or disabled.");
            return false;
        }

        try {
            BluetoothDevice device = adapter.getRemoteDevice(address);
            if (device.getBondState() != BluetoothDevice.BOND_BONDED) {
                Log.d(TAG, "Cannot connect Bluetooth Classic printer. Device is not bonded: " + address);
                return false;
            }

            BluetoothSocket existingSocket = sockets.get(address);
            if (existingSocket != null && existingSocket.isConnected()) {
                return true;
            }
            closeQuietly(existingSocket);
            sockets.remove(address);

            adapter.cancelDiscovery();

            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(OPERATION_TIMEOUT_MS);
            BluetoothSocket socket = connectSocket(device, false, remainingMillis(deadline));
            if (socket == null && remainingMillis(deadline) > 0) {
                socket = connectSocket(device, true, remainingMillis(deadline));
            }
            if (socket == null) {
                Log.d(TAG, "Cannot connect Bluetooth Classic printer. RFCOMM socket failed: " + address);
                return false;
            }

            sockets.put(address, socket);
            Log.d(TAG, "Bluetooth Classic printer connected: " + address);
            return true;
        } catch (SecurityException error) {
            Log.d(TAG, "Bluetooth Classic connect permission error: " + error);
            return false;
        } catch (IllegalArgumentException error) {
            Log.d(TAG, "Bluetooth Classic invalid address: " + address + ", error=" + error);
            return false;
        }
    }

    public synchronized boolean printText(String address, List<Integer> bytes) {
        if (bytes == null || bytes.isEmpty()) {
            Log.d(TAG, "Cannot print Bluetooth Classic data. Data is empty.");
            return false;
        }
        if (!isConnected(address) && !connect(address)) {
            return false;
        }

        BluetoothSocket socket = sockets.get(address);
        if (socket == null || !socket.isConnected()) {
            return false;
        }

        ScheduledFuture<?> timeout = scheduleSocketClose(socket, OPERATION_TIMEOUT_MS, "print");
        try {
            OutputStream outputStream = socket.getOutputStream();
            byte[] data = new byte[bytes.size()];
            for (int i = 0; i < bytes.size(); i++) {
                data[i] = bytes.get(i).byteValue();
            }
            outputStream.write(data);
            outputStream.flush();
            return true;
        } catch (IOException error) {
            Log.d(TAG, "Bluetooth Classic print failed: " + error);
            disconnect(address);
            return false;
        } finally {
            cancelTimeout(timeout);
        }
    }

    public synchronized boolean isConnected(String address) {
        BluetoothSocket socket = sockets.get(address);
        return socket != null && socket.isConnected();
    }

    public synchronized boolean disconnect(String address) {
        BluetoothSocket socket = sockets.remove(address);
        if (socket == null) {
            return true;
        }
        closeQuietly(socket);
        return true;
    }

    public void cancelActiveOperation() {
        closeQuietly(activeSocket);
    }

    public void closeAll() {
        closeQuietly(activeSocket);
        synchronized (this) {
            for (BluetoothSocket socket : sockets.values()) {
                closeQuietly(socket);
            }
            sockets.clear();
        }
        timeoutExecutor.shutdownNow();
    }

    @SuppressLint("MissingPermission")
    private BluetoothSocket connectSocket(BluetoothDevice device, boolean insecure, long timeoutMs) {
        BluetoothSocket socket = null;
        if (timeoutMs <= 0) return null;
        ScheduledFuture<?> timeout = null;
        try {
            socket = insecure
                    ? device.createInsecureRfcommSocketToServiceRecord(SPP_UUID)
                    : device.createRfcommSocketToServiceRecord(SPP_UUID);
            activeSocket = socket;
            timeout = scheduleSocketClose(socket, timeoutMs, insecure ? "connect_insecure" : "connect_secure");
            socket.connect();
            return socket;
        } catch (IOException error) {
            Log.d(TAG, "Bluetooth Classic " + (insecure ? "insecure" : "secure") + " socket failed: " + error);
            closeQuietly(socket);
            return null;
        } finally {
            cancelTimeout(timeout);
            if (activeSocket == socket) activeSocket = null;
        }
    }

    private long remainingMillis(long deadline) {
        return Math.max(0L, TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime()));
    }

    private ScheduledFuture<?> scheduleSocketClose(
            BluetoothSocket socket,
            long timeoutMs,
            String operation) {
        return timeoutExecutor.schedule(() -> {
            Log.d(TAG, "Bluetooth Classic operation timed out: " + operation);
            closeQuietly(socket);
        }, timeoutMs, TimeUnit.MILLISECONDS);
    }

    private void cancelTimeout(ScheduledFuture<?> timeout) {
        if (timeout != null) timeout.cancel(false);
    }

    private boolean hasBluetoothConnectPermission() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
                context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    private void closeQuietly(BluetoothSocket socket) {
        if (socket == null) {
            return;
        }
        try {
            socket.close();
        } catch (IOException ignored) {
        }
    }
}

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

public class BluetoothClassicPrinter {
    private static final String TAG = "FPP";
    private static final UUID SPP_UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");

    private final Context context;
    private final Map<String, BluetoothSocket> sockets = new HashMap<>();

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

            BluetoothSocket socket = connectSocket(device, false);
            if (socket == null) {
                socket = connectSocket(device, true);
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

    @SuppressLint("MissingPermission")
    private BluetoothSocket connectSocket(BluetoothDevice device, boolean insecure) {
        BluetoothSocket socket = null;
        try {
            socket = insecure
                    ? device.createInsecureRfcommSocketToServiceRecord(SPP_UUID)
                    : device.createRfcommSocketToServiceRecord(SPP_UUID);
            socket.connect();
            return socket;
        } catch (IOException error) {
            Log.d(TAG, "Bluetooth Classic " + (insecure ? "insecure" : "secure") + " socket failed: " + error);
            closeQuietly(socket);
            return null;
        }
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

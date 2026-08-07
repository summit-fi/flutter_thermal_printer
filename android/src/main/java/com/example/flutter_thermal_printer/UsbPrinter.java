package com.example.flutter_thermal_printer;

import static android.content.Context.USB_SERVICE;

import android.annotation.SuppressLint;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbManager;
import android.os.Build;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

import android.content.BroadcastReceiver;
import android.content.IntentFilter;
import android.util.Log;

import io.flutter.plugin.common.EventChannel;

public class UsbPrinter implements EventChannel.StreamHandler {
    @SuppressLint("StaticFieldLeak")
    private static Context context;

    private static final String ACTION_USB_PERMISSION = "com.example.flutter_thermal_printer.USB_PERMISSION";
    private static final String ACTION_USB_ATTACHED = "android.hardware.usb.action.USB_DEVICE_ATTACHED";
    private static final String ACTION_USB_DETACHED = "android.hardware.usb.action.USB_DEVICE_DETACHED";
    private static final String TAG = "FPP";
    private EventChannel.EventSink events;

    private BroadcastReceiver usbStateChangeReceiver;

    private void createUsbStateChangeReceiver() {
        usbStateChangeReceiver =  new BroadcastReceiver() {
            @SuppressLint("LongLogTag")
            @Override
            public void onReceive(Context context, Intent intent) {
                if (Objects.equals(intent.getAction(), ACTION_USB_ATTACHED)) {
                    UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                    Log.d(TAG, "ACTION_USB_ATTACHED");
                    sendDevice(device);
                } else if (Objects.equals(intent.getAction(), ACTION_USB_DETACHED)) {
                    UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                    Log.d(TAG, "ACTION_USB_DETACHED");
                    sendDevice(device);
                }
                Log.d(TAG, "ACTION_USB_PERMISSION " + (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)));
                if (Objects.equals(intent.getAction(), ACTION_USB_PERMISSION)) {
                    synchronized (this) {
                        UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                        boolean permissionGranted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                        if(permissionGranted) {
                            Log.d(TAG, "Permission granted for device " + device);
                            sendDevice(device);
                        } else {
                            Log.d(TAG, "Permission denied for device " + device);
                            connect(connectionVendorId, connectionProductId);
                        }
                    }
                }
            }
        };
    }

    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    @Override
    public void onListen(Object arguments, EventChannel.EventSink events) {
        this.events = events;
        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_USB_ATTACHED);
        filter.addAction(ACTION_USB_DETACHED);
        filter.addAction(ACTION_USB_PERMISSION);
        createUsbStateChangeReceiver();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(usbStateChangeReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            context.registerReceiver(usbStateChangeReceiver, filter);
        }
    }


    private void sendDevice(UsbDevice device ) {
        if (device == null) {
            Log.d(TAG, "Device is null.");
            return;
        }
        boolean isConnected = isConnected(String.valueOf(device.getVendorId()), String.valueOf(device.getProductId()));
        HashMap<String, Object> deviceData = new HashMap<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            deviceData.put("name", device.getProductName());
        }
        deviceData.put("vendorId", String.valueOf(device.getVendorId()));
        deviceData.put("productId", String.valueOf(device.getProductId()));
        deviceData.put("connected", isConnected);
        Log.d(TAG, "Sending device data: " + deviceData);
        if (events != null) {
            events.success(deviceData);
        }
    }


    @Override
    public void onCancel(Object arguments) {
        if (events != null) {
            context.unregisterReceiver(usbStateChangeReceiver);
            events = null;
        }
    }

    private static PendingIntent mPermissionIntent;

    UsbPrinter(Context context) {
        UsbPrinter.context = context;
        mPermissionIntent = PendingIntent.getActivity(context, 0, new Intent(ACTION_USB_PERMISSION), PendingIntent.FLAG_IMMUTABLE);
    }

    public List<Map<String, Object>> getUsbDevicesList() {
        UsbManager m = (UsbManager) context.getSystemService(USB_SERVICE);
        HashMap<String, UsbDevice> usbDevices = m.getDeviceList();
        List<Map<String, Object>> data = new ArrayList<Map<String, Object>>();
        for (Map.Entry<String, UsbDevice> entry : usbDevices.entrySet()) {
            UsbDevice device = entry.getValue();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                HashMap<String, Object> deviceData = new HashMap<String, Object>();
                deviceData.put("name", device.getProductName());
                deviceData.put("vendorId", String.valueOf(device.getVendorId()));
                deviceData.put("productId", String.valueOf(device.getProductId()));
                deviceData.put("connected", m.hasPermission(device));
                data.add(deviceData);
            }
        }
        return data;
    }

    private String connectionVendorId;
    private String connectionProductId;

    private Integer requestingPermission = 0;

    private UsbDevice findDevice(String vendorId, String productId) {
        UsbManager m = (UsbManager) context.getSystemService(USB_SERVICE);
        HashMap<String, UsbDevice> usbDevices = m.getDeviceList();

        for (Map.Entry<String, UsbDevice> entry : usbDevices.entrySet()) {
            UsbDevice device = entry.getValue();
            if (usbIdMatches(vendorId, device.getVendorId()) && usbIdMatches(productId, device.getProductId())) {
                return device;
            }
        }

        return null;
    }

    private boolean usbIdMatches(String value, int actualId) {
        if (value == null) {
            return false;
        }

        String normalized = value.trim();
        if (normalized.isEmpty()) {
            return false;
        }

        String lower = normalized.toLowerCase();
        String decimal = String.valueOf(actualId);
        String hex = Integer.toHexString(actualId);
        String paddedHex = String.format("%04x", actualId);

        return lower.equals(decimal) || lower.equals(hex) || lower.equals(paddedHex) || lower.equals("0x" + hex) || lower.equals("0x" + paddedHex);
    }

    //    Connect using VendorId and ProductId
    public boolean connect(String vendorId, String productId) {
        connectionVendorId = vendorId;
        connectionProductId = productId;
        UsbManager m = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        UsbDevice device = findDevice(vendorId, productId);

        if (device == null) {
            Log.d(TAG, "Device not found. vendorId=" + vendorId + ", productId=" + productId);
            return false;
        }

        if (!m.hasPermission(device) && requestingPermission <2) {
            requestingPermission++;
            PendingIntent permissionIntent = PendingIntent.getBroadcast(context, 0, new Intent(ACTION_USB_PERMISSION), PendingIntent.FLAG_IMMUTABLE);
            m.requestPermission(device, permissionIntent);
            return false;
        } else {
            requestingPermission = 0;
            sendDevice(device); // Proceed directly if permission exists
            return m.hasPermission(device);
        }
    }

    //    Print text on the printer
    public boolean printText(String vendorId, String productId, List<Integer> bytes) {
        UsbManager m = (UsbManager) context.getSystemService(USB_SERVICE);
        UsbDevice device = findDevice(vendorId, productId);
        if (device == null) {
            Log.d(TAG, "Cannot print. Device not found. vendorId=" + vendorId + ", productId=" + productId);
            return false;
        }
        if (bytes == null || bytes.isEmpty()) {
            Log.d(TAG, "Cannot print. Data is empty.");
            return false;
        }
        if (!m.hasPermission(device)) {
            m.requestPermission(device, mPermissionIntent);
        }
        if (!m.hasPermission(device)) {
            Log.d(TAG, "Cannot print. Permission is not granted.");
            return false;
        }
        UsbDeviceConnection connection = m.openDevice(device);

        if (connection == null) {
            Log.d(TAG, "Cannot print. Failed to open USB device.");
            return false;
        }
        if (device.getInterfaceCount() == 0) {
            Log.d(TAG, "Cannot print. USB device has no interfaces.");
            connection.close();
            return false;
        }
        boolean interfaceClaimed = connection.claimInterface(device.getInterface(0), true);
        if (!interfaceClaimed) {
            Log.d(TAG, "Cannot print. Failed to claim USB interface.");
            connection.close();
            return false;
        }
        UsbEndpoint mBulkEndOut = null;
        for (int i = 0; i < device.getInterface(0).getEndpointCount(); i++) {
            if (device.getInterface(0).getEndpoint(i).getType() == UsbConstants.USB_ENDPOINT_XFER_BULK && device.getInterface(0).getEndpoint(i).getDirection() == UsbConstants.USB_DIR_OUT) {
                mBulkEndOut = device.getInterface(0).getEndpoint(i);
                break;
            }
        }
        if (mBulkEndOut == null) {
            Log.d(TAG, "Cannot print. Bulk OUT endpoint not found.");
            connection.releaseInterface(device.getInterface(0));
            connection.close();
            return false;
        }
        byte[] data = new byte[bytes.size()];
        for (int i = 0; i < bytes.size(); i++) {
            data[i] = bytes.get(i).byteValue();
        }
        int transferred = connection.bulkTransfer(mBulkEndOut, data, data.length, 5000);
        connection.releaseInterface(device.getInterface(0));
        connection.close();
        return transferred >= 0;
    }

    public boolean isConnected(String vendorId, String productId) {
        UsbManager m = (UsbManager) context.getSystemService(USB_SERVICE);
        UsbDevice device = findDevice(vendorId, productId);
        if (device == null) {
            return false;
        }
        return m.hasPermission(device);
    }


    public boolean disconnect(String vendorId, String productId) {
        UsbManager m = (UsbManager) context.getSystemService(USB_SERVICE);
        UsbDevice device = findDevice(vendorId, productId);
        if (device == null) {
            return false;
        }
        boolean hasPermission = m.hasPermission(device);
        if (!hasPermission) {
            return false;
        }
        //  Release the interface
        UsbDeviceConnection connection = m.openDevice(device);
        if (connection == null) {
            return false;
        }
        if (device.getInterfaceCount() == 0) {
            connection.close();
            return false;
        }
        connection.releaseInterface(device.getInterface(0));
        connection.close();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            HashMap<String, Object> deviceData = new HashMap<String, Object>();
            deviceData.put("name", device.getProductName());
            deviceData.put("vendorId", String.valueOf(device.getVendorId()));
            deviceData.put("productId", String.valueOf(device.getProductId()));
            deviceData.put("connected", m.hasPermission(device));
            if (events != null) {
                events.success(deviceData);
            }
        }
        return true;
    }
}

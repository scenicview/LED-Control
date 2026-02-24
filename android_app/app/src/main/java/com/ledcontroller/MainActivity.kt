package com.ledcontroller

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.util.*

data class ScannedDevice(
    val device: BluetoothDevice,
    val name: String,
    val address: String,
    var rssi: Int
)

class MainActivity : AppCompatActivity() {

    companion object {
        private const val REQUEST_PERMISSIONS = 1
        private const val SERVICE_UUID = "19B10000-E8F2-537E-4F6C-D104768A1214"
        private const val COLOR_CHAR_UUID = "19B10001-E8F2-537E-4F6C-D104768A1214"
        private const val MODE_CHAR_UUID = "19B10002-E8F2-537E-4F6C-D104768A1214"
        private const val BRIGHTNESS_CHAR_UUID = "19B10003-E8F2-537E-4F6C-D104768A1214"
        private const val POWER_CHAR_UUID = "19B10004-E8F2-537E-4F6C-D104768A1214"
        private val DEVICE_NAME_PREFIXES = listOf("RGB-LED", "Games Room LED")
        private const val SCAN_DURATION_MS = 8000L
    }

    private var bluetoothAdapter: BluetoothAdapter? = null
    private var bluetoothGatt: BluetoothGatt? = null
    private var colorCharacteristic: BluetoothGattCharacteristic? = null
    private var modeCharacteristic: BluetoothGattCharacteristic? = null
    private var brightnessCharacteristic: BluetoothGattCharacteristic? = null

    private var isConnected = false
    private var isScanning = false
    private var currentColor = Color.RED
    private var currentMode = 0
    private var isPowerOn = true
    private var connectedDeviceName: String? = null

    private val scannedDevices = mutableMapOf<String, ScannedDevice>()
    private var scanCallback: ScanCallback? = null

    private lateinit var scanButton: Button
    private lateinit var scanningLayout: LinearLayout
    private lateinit var deviceListContainer: LinearLayout
    private lateinit var connectedDeviceLayout: LinearLayout
    private lateinit var connectedDeviceNameText: TextView
    private lateinit var disconnectButton: Button
    private lateinit var connectionIndicator: View
    private lateinit var previewView: View
    private lateinit var powerButton: Button
    private lateinit var brightnessSeekBar: SeekBar

    private val handler = Handler(Looper.getMainLooper())

    private val presetColors = listOf(
        Color.RED, Color.parseColor("#FF4500"), Color.parseColor("#FFA500"),
        Color.YELLOW, Color.parseColor("#ADFF2F"), Color.GREEN,
        Color.parseColor("#00FA9A"), Color.CYAN, Color.parseColor("#1E90FF"),
        Color.BLUE, Color.parseColor("#8A2BE2"), Color.MAGENTA,
        Color.parseColor("#FF1493"), Color.WHITE, Color.parseColor("#FFD700")
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        initViews()
        setupColorGrid()
        setupModeButtons()
        setupBrightness()
        setupPowerButton()
        initBluetooth()
        checkPermissions()
    }

    private fun initViews() {
        scanButton = findViewById(R.id.scanButton)
        scanningLayout = findViewById(R.id.scanningLayout)
        deviceListContainer = findViewById(R.id.deviceListContainer)
        connectedDeviceLayout = findViewById(R.id.connectedDeviceLayout)
        connectedDeviceNameText = findViewById(R.id.connectedDeviceName)
        disconnectButton = findViewById(R.id.disconnectButton)
        connectionIndicator = findViewById(R.id.connectionIndicator)
        previewView = findViewById(R.id.previewView)
        powerButton = findViewById(R.id.powerButton)
        brightnessSeekBar = findViewById(R.id.brightnessSeekBar)

        scanButton.setOnClickListener {
            if (isScanning) {
                stopScan()
            } else {
                startScan()
            }
        }

        disconnectButton.setOnClickListener {
            disconnect()
        }

        updateConnectionUI(false)
    }

    private fun setupColorGrid() {
        val colorGrid = findViewById<GridLayout>(R.id.colorGrid)

        presetColors.forEachIndexed { index, color ->
            val button = View(this).apply {
                layoutParams = GridLayout.LayoutParams().apply {
                    width = 0
                    height = resources.getDimensionPixelSize(R.dimen.color_button_size)
                    columnSpec = GridLayout.spec(index % 5, 1f)
                    rowSpec = GridLayout.spec(index / 5)
                    setMargins(8, 8, 8, 8)
                }
                setBackgroundColor(color)
                background = ContextCompat.getDrawable(context, R.drawable.color_button_bg)?.apply {
                    setTint(color)
                }
                setOnClickListener { selectColor(color) }
            }
            colorGrid.addView(button)
        }
    }

    private fun setupModeButtons() {
        val normalBtn = findViewById<Button>(R.id.modeNormal)
        val pulseBtn = findViewById<Button>(R.id.modePulse)
        val strobeBtn = findViewById<Button>(R.id.modeStrobe)
        val princessBtn = findViewById<Button>(R.id.modePrincess)

        val modeButtons = listOf(normalBtn, pulseBtn, strobeBtn, princessBtn)

        modeButtons.forEachIndexed { index, button ->
            button.setOnClickListener {
                currentMode = index
                modeButtons.forEach { btn ->
                    btn.isSelected = false
                    // Reset background tint for non-selected buttons
                    if (btn == princessBtn) {
                        btn.backgroundTintList = ContextCompat.getColorStateList(this,
                            if (btn.isSelected) android.R.color.holo_purple else R.color.princess_button)
                    } else {
                        btn.backgroundTintList = ContextCompat.getColorStateList(this, R.color.mode_button)
                    }
                }
                button.isSelected = true
                button.backgroundTintList = ContextCompat.getColorStateList(this, R.color.mode_button_selected)
                sendMode(index)
            }
        }

        // Princess mode is default (index 3)
        princessBtn.isSelected = true
        princessBtn.backgroundTintList = ContextCompat.getColorStateList(this, R.color.mode_button_selected)
        currentMode = 3
    }

    private fun setupBrightness() {
        brightnessSeekBar.max = 250
        brightnessSeekBar.progress = 250  // Full brightness by default

        brightnessSeekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {}
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {
                sendBrightness(brightnessSeekBar.progress + 5)
            }
        })
    }

    private fun setupPowerButton() {
        powerButton.setOnClickListener {
            isPowerOn = !isPowerOn
            updatePowerButton()
            sendPower(isPowerOn)
        }
    }

    private fun updatePowerButton() {
        if (isPowerOn) {
            powerButton.text = "POWER ON"
            powerButton.backgroundTintList = ContextCompat.getColorStateList(this, android.R.color.holo_red_light)
        } else {
            powerButton.text = "POWER OFF"
            powerButton.backgroundTintList = ContextCompat.getColorStateList(this, android.R.color.darker_gray)
        }
    }

    private fun selectColor(color: Int) {
        currentColor = color
        previewView.setBackgroundColor(color)
        sendColor(Color.red(color), Color.green(color), Color.blue(color))
    }

    private fun initBluetooth() {
        val bluetoothManager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothAdapter = bluetoothManager.adapter

        if (bluetoothAdapter == null) {
            Toast.makeText(this, "Bluetooth not supported", Toast.LENGTH_LONG).show()
            finish()
        }
    }

    private fun checkPermissions() {
        val permissions = mutableListOf<String>()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
                permissions.add(Manifest.permission.BLUETOOTH_SCAN)
            }
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                permissions.add(Manifest.permission.BLUETOOTH_CONNECT)
            }
        }

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            permissions.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }

        if (permissions.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, permissions.toTypedArray(), REQUEST_PERMISSIONS)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        if (isScanning) return

        scannedDevices.clear()
        deviceListContainer.removeAllViews()

        val scanner = bluetoothAdapter?.bluetoothLeScanner
        if (scanner == null) {
            Toast.makeText(this, "Bluetooth not available", Toast.LENGTH_SHORT).show()
            return
        }

        isScanning = true
        scanButton.text = "Stop Scan"
        scanningLayout.visibility = View.VISIBLE

        scanCallback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val device = result.device
                val deviceName = device.name ?: return

                // Only show devices with our expected name prefixes
                if (DEVICE_NAME_PREFIXES.any { deviceName.startsWith(it) }) {
                    val address = device.address
                    val existingDevice = scannedDevices[address]

                    if (existingDevice != null) {
                        existingDevice.rssi = result.rssi
                        handler.post { updateDeviceInList(existingDevice) }
                    } else {
                        val scannedDevice = ScannedDevice(device, deviceName, address, result.rssi)
                        scannedDevices[address] = scannedDevice
                        handler.post { addDeviceToList(scannedDevice) }
                    }
                }
            }

            override fun onScanFailed(errorCode: Int) {
                handler.post {
                    Toast.makeText(this@MainActivity, "Scan failed: $errorCode", Toast.LENGTH_SHORT).show()
                    stopScan()
                }
            }
        }

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner.startScan(null, settings, scanCallback)

        // Auto stop after duration
        handler.postDelayed({
            if (isScanning) {
                stopScan()
            }
        }, SCAN_DURATION_MS)
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        if (!isScanning) return

        isScanning = false
        scanButton.text = "Scan for Devices"
        scanningLayout.visibility = View.GONE

        scanCallback?.let {
            bluetoothAdapter?.bluetoothLeScanner?.stopScan(it)
        }
        scanCallback = null

        if (scannedDevices.isEmpty()) {
            val noDevicesText = TextView(this).apply {
                text = "No devices found. Make sure your LED controllers are powered on."
                setTextColor(Color.parseColor("#888888"))
                textSize = 14f
                setPadding(0, 16, 0, 0)
            }
            deviceListContainer.addView(noDevicesText)
        }
    }

    @SuppressLint("MissingPermission")
    private fun addDeviceToList(scannedDevice: ScannedDevice) {
        val inflater = LayoutInflater.from(this)
        val deviceView = inflater.inflate(R.layout.item_device, deviceListContainer, false)

        deviceView.tag = scannedDevice.address

        val nameText = deviceView.findViewById<TextView>(R.id.deviceName)
        val addressText = deviceView.findViewById<TextView>(R.id.deviceAddress)
        val rssiText = deviceView.findViewById<TextView>(R.id.deviceRssi)
        val connectBtn = deviceView.findViewById<Button>(R.id.connectDeviceButton)

        nameText.text = scannedDevice.name
        addressText.text = scannedDevice.address
        rssiText.text = "Signal: ${scannedDevice.rssi} dBm"

        connectBtn.setOnClickListener {
            stopScan()
            connectToDevice(scannedDevice.device, scannedDevice.name)
        }

        deviceListContainer.addView(deviceView)
    }

    private fun updateDeviceInList(scannedDevice: ScannedDevice) {
        for (i in 0 until deviceListContainer.childCount) {
            val view = deviceListContainer.getChildAt(i)
            if (view.tag == scannedDevice.address) {
                val rssiText = view.findViewById<TextView>(R.id.deviceRssi)
                rssiText?.text = "Signal: ${scannedDevice.rssi} dBm"
                break
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun connectToDevice(device: BluetoothDevice, deviceName: String) {
        connectedDeviceName = deviceName
        connectedDeviceNameText.text = "Connecting to $deviceName..."
        connectedDeviceLayout.visibility = View.VISIBLE
        disconnectButton.isEnabled = false

        bluetoothGatt = device.connectGatt(this, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        gatt.discoverServices()
                    }
                    BluetoothProfile.STATE_DISCONNECTED -> {
                        handler.post {
                            isConnected = false
                            updateConnectionUI(false)
                            Toast.makeText(this@MainActivity, "Disconnected from $connectedDeviceName", Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }

            override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    val service = gatt.getService(UUID.fromString(SERVICE_UUID))
                    if (service != null) {
                        colorCharacteristic = service.getCharacteristic(UUID.fromString(COLOR_CHAR_UUID))
                        modeCharacteristic = service.getCharacteristic(UUID.fromString(MODE_CHAR_UUID))
                        brightnessCharacteristic = service.getCharacteristic(UUID.fromString(BRIGHTNESS_CHAR_UUID))

                        handler.post {
                            isConnected = true
                            updateConnectionUI(true)
                            Toast.makeText(this@MainActivity, "Connected to $connectedDeviceName", Toast.LENGTH_SHORT).show()
                        }
                    } else {
                        handler.post {
                            Toast.makeText(this@MainActivity, "LED service not found on device", Toast.LENGTH_SHORT).show()
                            disconnect()
                        }
                    }
                }
            }
        })
    }

    @SuppressLint("MissingPermission")
    private fun disconnect() {
        bluetoothGatt?.disconnect()
        bluetoothGatt?.close()
        bluetoothGatt = null
        isConnected = false
        colorCharacteristic = null
        modeCharacteristic = null
        brightnessCharacteristic = null
        updateConnectionUI(false)
    }

    private fun updateConnectionUI(connected: Boolean) {
        if (connected) {
            connectedDeviceNameText.text = connectedDeviceName ?: "Connected"
            connectedDeviceLayout.visibility = View.VISIBLE
            disconnectButton.isEnabled = true
            connectionIndicator.setBackgroundResource(R.drawable.connection_indicator)
            deviceListContainer.removeAllViews()
        } else {
            connectedDeviceLayout.visibility = View.GONE
            connectedDeviceName = null
        }
    }

    @SuppressLint("MissingPermission")
    private fun sendColor(r: Int, g: Int, b: Int) {
        if (!isConnected || colorCharacteristic == null) return
        colorCharacteristic?.value = byteArrayOf(r.toByte(), g.toByte(), b.toByte())
        bluetoothGatt?.writeCharacteristic(colorCharacteristic)
    }

    @SuppressLint("MissingPermission")
    private fun sendMode(mode: Int) {
        if (!isConnected || modeCharacteristic == null) return
        modeCharacteristic?.value = byteArrayOf(mode.toByte())
        bluetoothGatt?.writeCharacteristic(modeCharacteristic)
    }

    @SuppressLint("MissingPermission")
    private fun sendBrightness(brightness: Int) {
        if (!isConnected || brightnessCharacteristic == null) return
        brightnessCharacteristic?.value = byteArrayOf(brightness.toByte())
        bluetoothGatt?.writeCharacteristic(brightnessCharacteristic)
    }

    @SuppressLint("MissingPermission")
    private fun sendPower(on: Boolean) {
        // Power is controlled via brightness (0 = off) or we send a special color
        if (!isConnected) return
        if (!on) {
            // Turn off by setting color to black
            colorCharacteristic?.value = byteArrayOf(0, 0, 0)
            bluetoothGatt?.writeCharacteristic(colorCharacteristic)
        } else {
            // Restore current color
            sendColor(Color.red(currentColor), Color.green(currentColor), Color.blue(currentColor))
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        stopScan()
        disconnect()
    }
}

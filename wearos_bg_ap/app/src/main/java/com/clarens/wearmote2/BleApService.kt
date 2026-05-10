package com.clarens.wearmote2

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.util.Log
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.*
import java.util.UUID

/* ── UUIDs – must match ESP32 ble_ap.h ─────────────────────────────── */
private val SERVICE_UUID    = UUID.fromString("12340001-1234-1234-1234-123456789ABC")
private val CHR_CMD_UUID    = UUID.fromString("12340002-1234-1234-1234-123456789ABC")
private val CHR_STATUS_UUID = UUID.fromString("12340003-1234-1234-1234-123456789ABC")
private val CCCD_UUID       = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
private const val SERVICE_NAME="BG_AP"
private var lastMode : ApMode=ApMode.UNKNOWN
private const val TAG = "BleApService"

/* ── BLE command bytes – must match ESP32 ble_ap.h ─────────────────── */
object BleCmd {
    const val STANDBY    = 0x01.toByte()
    const val AUTO       = 0x02.toByte()
    const val WIND       = 0x03.toByte()
    const val NAV        = 0x04.toByte()
    const val PLUS_1     = 0x05.toByte()
    const val MINUS_1    = 0x06.toByte()
    const val PLUS_10    = 0x07.toByte()
    const val MINUS_10   = 0x08.toByte()
    const val MODE_CYCLE = 0x09.toByte()
}

/* ── AP mode enum ───────────────────────────────────────────────────── */
enum class ApMode(val label: String) {
    STANDBY("STANDBY"),
    AUTO("AUTO"),
    NFU("NFU"),
    WIND("WIND"),
    NAV("NAV"),
    UNKNOWN("—");

    companion object {
        fun from(byte: Int) = when (byte) {
            0    -> STANDBY
            1    -> AUTO
            2    -> NFU
            3    -> WIND
            4    -> NAV
            else -> UNKNOWN
        }
    }
}

/* ── Status packet decoded from ESP32 notify ────────────────────────── */
data class ApStatus(
    val mode           : ApMode  = ApMode.UNKNOWN,
    val vesselHeading  : Float   = 0f,    // degrees
    val apHeading      : Float   = 0f,    // degrees (commanded by AP)
    val statusValid    : Boolean = false,
    val apN2kAddr      : Int     = 0xFF,
)

/* ── Connection state ───────────────────────────────────────────────── */
sealed class BleState {
    object Scanning    : BleState()
    object Connecting  : BleState()
    object Connected   : BleState()
    object Disconnected: BleState()
    data class Error(val msg: String) : BleState()
}

/* ── Service ────────────────────────────────────────────────────────── */
@SuppressLint("MissingPermission")
class BleApService(private val context: Context) {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    /* Public state flows */
    private val _bleState  = MutableStateFlow<BleState>(BleState.Disconnected)
    private val _apStatus  = MutableStateFlow(ApStatus())
    val bleState : StateFlow<BleState>  = _bleState.asStateFlow()
    val apStatus : StateFlow<ApStatus>  = _apStatus.asStateFlow()

    private var gatt       : BluetoothGatt?           = null
    private var cmdChr     : BluetoothGattCharacteristic? = null
    private var statusChr  : BluetoothGattCharacteristic? = null
    private var cmdQueue   : Channel<Byte>            = Channel(8)

    /* ── Scan ─────────────────────────────────────────────────────── */
    fun connect() {
        scope.launch { scan() }
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        _bleState.value = BleState.Disconnected
    }

    private suspend fun scan() {
        _bleState.value = BleState.Scanning
        Log.i(TAG, "Scanning for " +SERVICE_NAME)

        val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE)
                as BluetoothManager).adapter

        if (adapter == null || !adapter.isEnabled) {
            _bleState.value = BleState.Error("Bluetooth not available")
            return
        }

        val scanner = adapter.bluetoothLeScanner ?: run {
            _bleState.value = BleState.Error("BLE scanner unavailable")
            return
        }

        val filter = ScanFilter.Builder()
            .setDeviceName(SERVICE_NAME)
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        val found = CompletableDeferred<BluetoothDevice>()

        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                Log.i(TAG, "Found: ${result.device.address}")
                scanner.stopScan(this)
                found.complete(result.device)
            }
            override fun onScanFailed(errorCode: Int) {
                found.completeExceptionally(Exception("Scan failed: $errorCode"))
            }
        }

        scanner.startScan(listOf(filter), settings, cb)

        try {
            val device = withTimeout(15_000) { found.await() }
            connectGatt(device)
        } catch (e: TimeoutCancellationException) {
            scanner.stopScan(cb)
            _bleState.value = BleState.Error("Device not found – is BG_AP powered on?")
        } catch (e: Exception) {
            _bleState.value = BleState.Error(e.message ?: "Scan error")
        }
    }

    /* ── GATT connect ─────────────────────────────────────────────── */
    private fun connectGatt(device: BluetoothDevice) {
        _bleState.value = BleState.Connecting
        Log.i(TAG, "Connecting GATT to ${device.address}")
        gatt = device.connectGatt(context, false, gattCallback,
                                   BluetoothDevice.TRANSPORT_LE)
    }

    /* ── GATT callback ────────────────────────────────────────────── */
    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    Log.i(TAG, "GATT connected, discovering services…")
                    gatt.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    Log.i(TAG, "GATT disconnected")
                    cmdChr    = null
                    statusChr = null
                    _bleState.value = BleState.Disconnected
                    // Auto-reconnect after 3 s
                    scope.launch {
                        delay(3_000)
                        scan()
                    }
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                _bleState.value = BleState.Error("Service discovery failed: $status")
                return
            }

            val svc = gatt.getService(SERVICE_UUID) ?: run {
                _bleState.value = BleState.Error("BG_AP service not found")
                return
            }

            cmdChr    = svc.getCharacteristic(CHR_CMD_UUID)
            statusChr = svc.getCharacteristic(CHR_STATUS_UUID)

            if (cmdChr == null || statusChr == null) {
                _bleState.value = BleState.Error("Required characteristics missing")
                return
            }

            /* Enable notifications on STATUS characteristic */
            gatt.setCharacteristicNotification(statusChr, true)
            val cccd = statusChr!!.getDescriptor(CCCD_UUID)
            cccd?.let {
                it.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                gatt.writeDescriptor(it)
            }

            _bleState.value = BleState.Connected
            Log.i(TAG, "Ready – CMD and STATUS characteristics found")

            /* Start command dispatch loop */
            scope.launch { commandDispatcher() }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == CHR_STATUS_UUID) {
                parseStatus(characteristic.value)
            }
        }

        /* API 33+ */
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == CHR_STATUS_UUID) {
                parseStatus(value)
            }
        }
    }
  
    /* ── Status packet parser ─────────────────────────────────────── */
    private fun parseStatus(data: ByteArray) {
Log.d(TAG, data.joinToString("parseStatus: ") { "%02x".format(it) }) // "414243" (hex)
        if (data.size < 7) return

        val mode         = ApMode.from(data[0].toInt() and 0xFF)
        val vesselHdg    = ((data[1].toInt() and 0xFF) or
                           ((data[2].toInt() and 0xFF) shl 8)).toFloat() / 10f
        val apHdg        = ((data[3].toInt() and 0xFF) or
                           ((data[4].toInt() and 0xFF) shl 8)).toFloat() / 10f
        val valid        = data[5].toInt() != 0
        val n2kAddr      = data[6].toInt() and 0xFF

        _apStatus.value = ApStatus(mode, vesselHdg, apHdg, valid, n2kAddr)
        
        if(lastMode!= mode) {
          lastMode=mode;
          Log.d(TAG, "Status: mode=$mode hdg=${vesselHdg}° apHdg=${apHdg}° valid=$valid")
        }
    }

    /* ── Command sender ───────────────────────────────────────────── */
    fun sendCommand(cmd: Byte) {
        scope.launch { cmdQueue.trySend(cmd) }
    }

    private suspend fun commandDispatcher() {
        for (cmd in cmdQueue) {
            val chr = cmdChr ?: break
            val g   = gatt   ?: break
            chr.value      = byteArrayOf(cmd)
            chr.writeType  = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            g.writeCharacteristic(chr)
            delay(100)   /* small gap between commands */
        }
    }

    fun destroy() {
        disconnect()
        scope.cancel()
    }
}

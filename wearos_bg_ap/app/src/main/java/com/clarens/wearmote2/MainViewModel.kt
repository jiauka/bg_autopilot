package com.clarens.wearmote2

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch

class MainViewModel(app: Application) : AndroidViewModel(app) {

    val ble = BleApService(app.applicationContext)

    val bleState : StateFlow<BleState>  = ble.bleState
    val apStatus : StateFlow<ApStatus>  = ble.apStatus

    init {
        ble.connect()
    }

    fun sendCmd(cmd: Byte) = ble.sendCommand(cmd)

    fun retry() = ble.connect()

    override fun onCleared() {
        ble.destroy()
        super.onCleared()
    }
}

package com.clarens.wearmote2

import kotlinx.coroutines.flow.StateFlow

interface ApService {
    val bleState: StateFlow<BleState>
    val apStatus: StateFlow<ApStatus>
    fun connect()
    fun disconnect()
    fun sendCommand(cmd: Byte)
    fun destroy()
}

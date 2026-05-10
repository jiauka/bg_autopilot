package com.clarens.bgap

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.*
import androidx.compose.ui.graphics.drawscope.*
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.*
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/* ── Colours ──────────────────────────────────────────────────────── */
private val BgBlue    = Color(0xFF1565C0)
private val BgAccent  = Color(0xFF29B6F6)
private val TextSub   = Color(0xFFB0BEC5)
private val QuadRed   = Color(0xFFBB0000)
private val QuadGreen = Color(0xFF007700)
private val BtnDark   = Color(0xFF2A2A2A)

@Composable
fun BgApApp(vm: MainViewModel, onQuit: () -> Unit = {}) {
    val bleState by vm.bleState.collectAsStateWithLifecycle()
    val apStatus by vm.apStatus.collectAsStateWithLifecycle()

    MaterialTheme {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color.Black),
            contentAlignment = Alignment.Center
        ) {
            when (bleState) {
                is BleState.Scanning, is BleState.Connecting ->
                    ConnectingScreen(bleState)

                is BleState.Error ->
                    ErrorScreen(
                        msg     = (bleState as BleState.Error).msg,
                        onRetry = { vm.retry() }
                    )

                BleState.Disconnected ->
                    ErrorScreen(
                        msg     = "Disconnected",
                        onRetry = { vm.retry() }
                    )

                BleState.Connected ->
                    MainScreen(
                        status = apStatus,
                        onCmd  = { vm.sendCmd(it) },
                        onQuit = onQuit
                    )
            }
        }
    }
}

/* ── Connecting / Scanning screen ─────────────────────────────────── */
@Composable
private fun ConnectingScreen(state: BleState) {
    val label = if (state is BleState.Scanning) "Metis?" else "Connecting…"
    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Image(
            painter            = painterResource(R.drawable.metis),
            contentDescription = null,
            modifier           = Modifier.fillMaxSize(),
            contentScale       = ContentScale.Crop,
            alpha              = 0.45f
        )
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            CircularProgressIndicator(
                modifier    = Modifier.size(40.dp),
                color       = BgAccent,
                trackColor  = BgBlue.copy(alpha = 0.3f),
                strokeWidth = 3.dp
            )
            Spacer(Modifier.height(8.dp))
            Text(
                label,
                color     = Color.White,
                fontSize  = 25.sp,
                textAlign = TextAlign.Center,
                style     = TextStyle(
                    fontSize   = 24.sp,
                    shadow     = Shadow(color = Color.Black, offset = Offset(10f, 10f), blurRadius = 10f),
                    fontWeight = FontWeight.Bold
                )
            )
        }
    }
}

/* ── Error screen ─────────────────────────────────────────────────── */
@Composable
private fun ErrorScreen(msg: String, onRetry: () -> Unit) {
    Column(
        horizontalAlignment = Alignment.CenterHorizontally,
        modifier = Modifier.padding(12.dp)
    ) {
        Text("⚠", fontSize = 28.sp)
        Spacer(Modifier.height(4.dp))
        Text(msg, color = TextSub, fontSize = 11.sp,
            textAlign = TextAlign.Center, lineHeight = 14.sp)
        Spacer(Modifier.height(8.dp))
        Button(
            onClick  = onRetry,
            modifier = Modifier.size(width = 80.dp, height = 32.dp),
            colors   = ButtonDefaults.buttonColors(containerColor = BgBlue)
        ) {
            Text("Retry", fontSize = 12.sp, color = Color.White)
        }
    }
}

/* ── Main autopilot screen ────────────────────────────────────────── */
@Composable
private fun MainScreen(
    status : ApStatus,
    onCmd  : (Byte) -> Unit,
    onQuit : () -> Unit
) {
    val mode   = status.mode
    val isStby = mode == ApMode.STANDBY || mode == ApMode.UNKNOWN || mode == ApMode.NFU
    var heading = status.apHeading
    var twa     = status.twa

    var upTextColor = QuadGreen
    var btTextColor = Color.Blue

    if (mode == ApMode.WIND) {
        btTextColor = QuadGreen
        if (heading > 180) { heading = 360 - heading; upTextColor = QuadRed }
        if (twa    > 180) { twa     = 360 - twa;     btTextColor = QuadRed }
    }

    val upText = if (isStby) "" else "%.0f°".format(heading)
    val btText = when (mode) {
        ApMode.WIND -> "%.0f°".format(twa)
        ApMode.AUTO -> "%.0f°".format(status.vesselHeading)
        else        -> ""
    }

    // In WIND mode the step commands are polarity-inverted relative to AUTO:
    // the UI label always reflects the visual change to the displayed angle.
    fun minus10() = onCmd(if (mode == ApMode.WIND) BleCmd.PLUS_10  else BleCmd.MINUS_10)
    fun minus1()  = onCmd(if (mode == ApMode.WIND) BleCmd.PLUS_1   else BleCmd.MINUS_1)
    fun plus1()   = onCmd(if (mode == ApMode.WIND) BleCmd.MINUS_1  else BleCmd.PLUS_1)
    fun plus10()  = onCmd(if (mode == ApMode.WIND) BleCmd.MINUS_10 else BleCmd.PLUS_10)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .padding(horizontal = 16.dp, vertical = 20.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.SpaceBetween
    ) {

        /* ── Upper text: commanded heading / target TWA ───────────── */
        Box(
            modifier = Modifier.fillMaxWidth().height(72.dp),
            contentAlignment = Alignment.Center
        ) {
            if (upText.isNotEmpty())
                OutlinedText(upText, color = upTextColor, fontSize = 52.sp, fontWeight = FontWeight.Bold)
        }

        /* ── Heading step buttons: 2 rows ────────────────────────────── */
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ApButton( "-1°", Modifier.weight(1f), enabled = !isStby, active = !isStby, activeColor = QuadRed)   { minus1()  }
                ApButton( "+1°", Modifier.weight(1f), enabled = !isStby, active = !isStby, activeColor = QuadGreen) { plus1()   }
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ApButton("-10°", Modifier.weight(1f), enabled = !isStby, active = !isStby, activeColor = QuadRed)   { minus10() }
                ApButton("+10°", Modifier.weight(1f), enabled = !isStby, active = !isStby, activeColor = QuadGreen) { plus10()  }
            }
        }

        /* ── Mode buttons ─────────────────────────────────────────── */
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ApButton(
                    "AUTO", Modifier.weight(1f),
                    active      = mode == ApMode.AUTO,
                    activeColor = BgAccent
                ) { onCmd(BleCmd.AUTO) }

                ApButton(
                    "WIND", Modifier.weight(1f),
                    active      = mode == ApMode.WIND,
                    activeColor = QuadGreen
                ) { onCmd(BleCmd.WIND) }
            }

            StandbyButton(
                modifier    = Modifier.fillMaxWidth(),
                active      = isStby,
                onTap       = { onCmd(BleCmd.STANDBY) },
                onLongPress = { onCmd(BleCmd.STANDBY); onQuit() }
            )
        }

        /* ── Lower text: actual vessel heading / actual TWA ───────── */
        Box(
            modifier = Modifier.fillMaxWidth().height(72.dp),
            contentAlignment = Alignment.Center
        ) {
            if (btText.isNotEmpty())
                OutlinedText(btText, color = btTextColor, fontSize = 52.sp, fontWeight = FontWeight.Bold)
        }
    }
}

/* ── Reusable dark button ─────────────────────────────────────────── */
@Composable
private fun ApButton(
    label       : String,
    modifier    : Modifier = Modifier,
    enabled     : Boolean  = true,
    active      : Boolean  = false,
    activeColor : Color    = BgBlue,
    onClick     : () -> Unit
) {
    Button(
        onClick  = onClick,
        enabled  = enabled,
        modifier = modifier.height(68.dp),
        shape    = RoundedCornerShape(8.dp),
        colors   = ButtonDefaults.buttonColors(
            containerColor         = if (active) activeColor else BtnDark,
            disabledContainerColor = Color(0xFF1A1A1A),
            contentColor           = Color.White,
            disabledContentColor   = Color(0xFFAAAAAA)
        )
    ) {
        Text(label, fontSize = 20.sp, fontWeight = FontWeight.Bold)
    }
}

/* ── Standby button (needs long-press-to-quit) ────────────────────── */
@Composable
private fun StandbyButton(
    modifier    : Modifier = Modifier,
    active      : Boolean,
    onTap       : () -> Unit,
    onLongPress : () -> Unit
) {
    Box(
        modifier = modifier
            .height(68.dp)
            .clip(RoundedCornerShape(8.dp))
            .background(if (active) QuadRed else BtnDark)
            .pointerInput(Unit) {
                detectTapGestures(onTap = { onTap() }, onLongPress = { onLongPress() })
            },
        contentAlignment = Alignment.Center
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            PowerIcon(iconSize = 20.dp)
            Text("STBY", color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Bold)
        }
    }
}

/* ── Text with 2-pixel white outline ─────────────────────────────── */
@Composable
private fun OutlinedText(
    text      : String,
    modifier  : Modifier    = Modifier,
    color     : Color,
    fontSize  : TextUnit,
    fontWeight: FontWeight? = null
) {
    val o = with(LocalDensity.current) { 2.toDp() }
    Box(modifier = modifier, contentAlignment = Alignment.Center) {
        for (dx in listOf(-o, 0.dp, o)) for (dy in listOf(-o, 0.dp, o))
            if (dx != 0.dp || dy != 0.dp)
                Text(text, Modifier.offset(dx, dy), color = Color.White,
                     fontSize = fontSize, fontWeight = fontWeight)
        Text(text, color = color, fontSize = fontSize, fontWeight = fontWeight)
    }
}

/* ── Drawn power / standby icon ───────────────────────────────────── */
@Composable
private fun PowerIcon(iconSize: Dp = 44.dp) {
    Canvas(modifier = Modifier.size(iconSize)) {
        val cx = size.width / 2f
        val cy = size.height / 2f
        val r  = size.minDimension * 0.34f
        val sw = size.minDimension * 0.11f

        drawArc(
            color      = Color.White,
            startAngle = -225f,
            sweepAngle = 270f,
            useCenter  = false,
            topLeft    = Offset(cx - r, cy - r),
            size       = Size(r * 2f, r * 2f),
            style      = Stroke(width = sw, cap = StrokeCap.Round)
        )
        drawLine(
            color       = Color.White,
            start       = Offset(cx, cy - r * 1.35f),
            end         = Offset(cx, cy - r * 0.15f),
            strokeWidth = sw,
            cap         = StrokeCap.Round
        )
    }
}

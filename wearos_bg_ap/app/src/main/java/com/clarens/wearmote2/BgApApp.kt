package com.clarens.wearmote2

import androidx.compose.animation.core.*
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.*
import androidx.compose.ui.graphics.drawscope.*
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.*
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.wear.compose.material.*

/* ── Colours ──────────────────────────────────────────────────────── */
private val BgBlue     = Color(0xFF1565C0)
private val BgAccent   = Color(0xFF29B6F6)
private val TextSub    = Color(0xFFB0BEC5)
private val QuadRed    = Color(0xFFBB0000)
private val QuadGreen  = Color(0xFF00DD00)

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
            painter        = painterResource(R.drawable.metis),
            contentDescription = null,
            modifier       = Modifier.fillMaxSize(),
            contentScale   = ContentScale.Crop,
            alpha          = 0.45f
        )
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            CircularProgressIndicator(
                modifier       = Modifier.size(40.dp),
                indicatorColor = BgAccent,
                trackColor     = BgBlue.copy(alpha = 0.3f),
                strokeWidth    = 3.dp
            )
            Spacer(Modifier.height(8.dp))
            Text(
                label,
                color = Color.White,
                fontSize = 25.sp,
                textAlign = TextAlign.Center,
                style = TextStyle(
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
            colors   = ButtonDefaults.buttonColors(backgroundColor = BgBlue)
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
    var stepTen by remember { mutableStateOf(false) }
    val mode   = status.mode
    val isStby = mode == ApMode.STANDBY || mode == ApMode.UNKNOWN || mode == ApMode.NFU
    var heading = status.apHeading
    var upTextColor=QuadGreen

    LaunchedEffect(isStby) { if (isStby) stepTen = false }

    val uplLabel = if (isStby) "AUTO"  else if (stepTen) "-10°" else "-1°"
    val uprLabel = if (isStby) "WIND"  else if (stepTen) "+10°" else "+1°"
    val btrLabel = if (isStby) "---"   else "1/10"

    if (mode ==ApMode.WIND) {
      if(heading > 180 ) {
        heading=360-heading;
        upTextColor=QuadRed
      }
    }
       
    val upText = if (isStby) "" else  "%.0f°".format(heading)
    val btText = if (isStby) "" else "%.0f°".format(status.vesselHeading)
    val btTextColor=Color.Blue

    Box(modifier = Modifier.fillMaxSize().background(Color.Black)) {

        Column(modifier = Modifier.fillMaxSize()) {

            /* ── Top row: UPL | UPR ───────────────────────────────── */
            Row(modifier = Modifier.fillMaxWidth().weight(1f)) {

                /* UPL */
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .background(Color.Black)
                        .clickable {
                            if (isStby) onCmd(BleCmd.AUTO)
                            else if (mode ==ApMode.WIND) {
                              if (stepTen) onCmd(BleCmd.PLUS_10)
                              else onCmd(BleCmd.PLUS_1)
                            }
                            else if (mode ==ApMode.AUTO) {
                              if (stepTen) onCmd(BleCmd.MINUS_10)
                              else onCmd(BleCmd.MINUS_1)
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(uplLabel, color = QuadRed, fontSize = 22.sp, fontWeight = FontWeight.Bold)
                }

                /* UPR */
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .background(Color.Black)
                        .clickable {
                            if (isStby) onCmd(BleCmd.WIND)
                            else if (mode ==ApMode.WIND) {
                              if (stepTen) onCmd(BleCmd.MINUS_10)
                              else onCmd(BleCmd.MINUS_1)
                            }
                            else if (mode ==ApMode.AUTO) {
                              if (stepTen) onCmd(BleCmd.PLUS_10)
                              else onCmd(BleCmd.PLUS_1)
                            }
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(uprLabel, color = QuadGreen, fontSize = 22.sp, fontWeight = FontWeight.Bold)
                }
            }

            /* ── Bottom row: BTL (STBY) | BTR ────────────────────── */
            Row(modifier = Modifier.fillMaxWidth().weight(1f)) {

                /* BTL – always STANDBY; long-press also quits */
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .background(QuadRed)
                        .pointerInput(Unit) {
                            detectTapGestures(
                                onTap       = { onCmd(BleCmd.STANDBY) },
                                onLongPress = { onCmd(BleCmd.STANDBY); onQuit() }
                            )
                        },
                    contentAlignment = Alignment.Center
                ) {
                    PowerIcon()
                }

                /* BTR */
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .background(Color.Black)
                        .clickable {
                            if (!isStby) stepTen = !stepTen //if (isStby) onCmd(BleCmd.NAV) else stepTen = !stepTen
                        },
                    contentAlignment = Alignment.Center
                ) {
                    Text(btrLabel, color = QuadGreen, fontSize = 22.sp, fontWeight = FontWeight.Bold)
                }
            }
        }

        /* ── UPTEXT: desired heading / wind angle ─────────────────── */
        if (upText.isNotEmpty()) {
            Text(
                upText,
                modifier   = Modifier.align(Alignment.TopCenter).padding(top = 6.dp),
                color      = upTextColor,
                fontSize   = 32.sp,
                fontWeight = FontWeight.Bold
            )
        }

        /* ── BTTEXT: actual vessel course / wind angle ────────────── */
        if (btText.isNotEmpty()) {
            Text(
                btText,
                modifier   = Modifier.align(Alignment.BottomCenter).padding(bottom = 6.dp),
                color      = btTextColor,
                fontSize   = 32.sp,
                fontWeight = FontWeight.Bold
            )
        }
    }
}

/* ── Drawn power / standby icon ───────────────────────────────────── */
@Composable
private fun PowerIcon() {
    Canvas(modifier = Modifier.size(44.dp)) {
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

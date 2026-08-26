package com.hpg.vbase

import android.content.pm.ActivityInfo
import android.os.Build
import android.os.Bundle
import android.view.OrientationEventListener
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import com.google.androidgamesdk.GameActivity

/**
 * Тонкая Kotlin-обвязка. Вся логика — в нативном коде (app/src/main/cpp).
 * GameActivity сама создаёт SurfaceView и пробрасывает жизненный цикл
 * и ввод в android_main().
 */
class MainActivity : GameActivity() {

    // Переворот landscape <-> reverse-landscape (на 180 градусов) НЕ меняет
    // Configuration, поэтому системный автоповорот (даже sensorLandscape) на
    // многих прошивках его не подхватывает, и картинка оказывается "вверх ногами".
    // Слушаем датчик сами и явно запрашиваем нужный landscape через
    // setRequestedOrientation — работает независимо от блокировки автоповорота.
    private lateinit var orientationListener: OrientationEventListener
    private var requestedLandscape = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        orientationListener = object : OrientationEventListener(this) {
            override fun onOrientationChanged(angle: Int) {
                if (angle == ORIENTATION_UNKNOWN) return
                // angle ~= 90  -> левый бок сверху  -> reverse landscape
                // angle ~= 270 -> правый бок сверху -> обычный landscape
                // около 0/180 (портрет/переворот через торец) не трогаем.
                val target = when (angle) {
                    in 60..140  -> ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
                    in 220..300 -> ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                    else -> return
                }
                if (target != requestedLandscape) {
                    requestedLandscape = target
                    requestedOrientation = target
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (orientationListener.canDetectOrientation()) orientationListener.enable()
    }

    override fun onPause() {
        orientationListener.disable()
        super.onPause()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.apply {
                hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                )
        }
    }

    companion object {
        init {
            System.loadLibrary("vbase")
        }
    }
}

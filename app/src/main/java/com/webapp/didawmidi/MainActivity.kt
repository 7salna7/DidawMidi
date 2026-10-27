package com.webapp.didawmidi

import android.os.Bundle
import android.view.MotionEvent
import android.view.View
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        
        // Start up the low-latency native audio streams
        NativeAudioEngine.nativeInit()
        
        // Start monitoring for USB MIDI OTG keyboards
        NativeAudioEngine.startMidiOtgMonitoring(this)

        // Set up a simple full-screen view to catch touch events instantly
        val mainView = View(this)
        setContentView(mainView)

        mainView.setOnTouchListener { _, event ->
            if (event.action == MotionEvent.ACTION_DOWN) {
                // Low-overhead mock PCM sound buffer block
                val silentBuffer = FloatArray(256)
                // Trigger a 0-delay sample channel execution test
                NativeAudioEngine.nativeTriggerPad(0, silentBuffer, silentBuffer.size, 1.0f)
            }
            true
        }
    }
}

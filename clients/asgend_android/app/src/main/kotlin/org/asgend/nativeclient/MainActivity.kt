package org.asgend.nativeclient

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import org.asgend.nativeclient.ui.AsgendApp
import org.asgend.nativeclient.ui.theme.AsgendTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            AsgendTheme {
                AsgendApp()
            }
        }
    }
}

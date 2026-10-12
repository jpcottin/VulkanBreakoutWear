package com.jpcottin.vulkanbreakoutwear

import android.app.NativeActivity
import android.os.ParcelFileDescriptor
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Instrumented smoke test: launches the NativeActivity, verifies that Vulkan
 * initialises and the activity reaches RESUMED, then starts a round under
 * Auto Play and screenshots it every couple of seconds. The screenshots go to
 * /data/local/tmp/smoke/ (owned by the shell user, survives the post-test
 * uninstall) so CI pulls the whole directory with `adb pull`.
 */
@RunWith(AndroidJUnit4::class)
class SmokeTest {

    private val shotDir = "/data/local/tmp/smoke"

    private fun shell(cmd: String): String {
        val pfd = InstrumentationRegistry.getInstrumentation()
            .uiAutomation.executeShellCommand(cmd)
        return ParcelFileDescriptor.AutoCloseInputStream(pfd)
            .bufferedReader().readText()
    }

    // screencap runs as the shell user, so the file is THIS asserted moment of
    // the game in the foreground, not an unverified relaunch after the test.
    private fun screenshot(name: String) = shell("screencap -p $shotDir/$name.png")

    @Test
    fun vulkanInitialisesAndAutoPlayRuns() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext

        // Switch Auto Play on (and sound off) before the launch by seeding the
        // settings file the game reads at start-up: the layout mirrors
        // Game::saveSettings — magic "SETT", soundOn, autoPlay, all
        // little-endian int32. The test runs in the app's process, so filesDir
        // is the NativeActivity's internalDataPath. Whatever was there before
        // is put back afterwards, so a developer's own watch keeps its choices.
        val settings = File(context.filesDir, "settings.bin")
        val previous = if (settings.exists()) settings.readBytes() else null
        settings.writeBytes(
            ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(0x53455454).putInt(0).putInt(1).array()
        )
        shell("rm -rf $shotDir")
        shell("mkdir -p $shotDir")

        // An emulator reports itself as charging, and Wear OS answers a plug
        // event with a charging screen that takes the foreground. Unplug the
        // emulated battery for the round so the screenshots show the game.
        shell("dumpsys battery unplug")

        // Wear OS turns the screen off after a few seconds and the input
        // dispatcher then drops touches (the display's input viewport goes
        // inactive, see below). "svc power stayon" only applies while
        // charging, which the unplug above rules out, so stretch the
        // screen-off timeout for the test and wake the display.
        val previousTimeout = shell("settings get system screen_off_timeout").trim()
        shell("settings put system screen_off_timeout 600000")
        shell("input keyevent KEYCODE_WAKEUP")

        // Clear logcat BEFORE launching so the first-frame assertion can only be
        // satisfied by THIS launch — the buffer is boot-persistent, and a
        // "First frame presented" from an earlier run would mask a dead
        // renderer. (That's also why the activity is launched manually rather
        // than via ActivityScenarioRule, which starts it before the test body.)
        shell("logcat -c")

        try {
            ActivityScenario.launch(NativeActivity::class.java).use { scenario ->
                // Poll for the first PRESENTED frame instead of a fixed sleep:
                // the software-rasterized CI legs (llvmpipe) can take well over
                // any single guess. "Swapchain ready" only means the swapchain
                // was created — it is logged before any frame is drawn — so
                // wait for "First frame presented", which the renderer logs
                // after the first successful vkQueuePresentKHR.
                // Note: a single filterspec — a second one for the same tag
                // would override the first (":I" already includes warnings
                // and errors).
                val deadline = System.currentTimeMillis() + 60_000
                var log = ""
                while (System.currentTimeMillis() < deadline) {
                    log = shell("logcat -d -s BreakoutWear:I")
                    if (log.contains("First frame presented")) break
                    Thread.sleep(500)
                }

                val state = scenario.state
                assertEquals(
                    "NativeActivity should be RESUMED after Vulkan init, but was $state",
                    Lifecycle.State.RESUMED,
                    state
                )

                // RESUMED alone doesn't prove pixels: the game survives a
                // Vulkan-less device without crashing (it just renders
                // nothing). Assert the renderer actually presented a frame
                // (which implies the swapchain came up), so a dead render path
                // fails the test instead of producing silent black screenshots.
                assertTrue(
                    "Renderer never reached 'First frame presented' — Vulkan log was:\n" +
                        log.lines().takeLast(20).joinToString("\n"),
                    log.contains("First frame presented")
                )
                screenshot("00-title")

                // On a headless emulator the display's input viewport can
                // come up inactive (the preview engine's does): the input
                // reader then keeps every touch device disabled and even
                // injected taps never reach a window, while the game still
                // renders into its composited surface. A sleep/wake cycle
                // with the game up makes the reader reconfigure the touch
                // device for display 0 ("mode DIRECT" in its log), after
                // which taps land. Bring the game back to the front after.
                shell("input keyevent KEYCODE_SLEEP")
                Thread.sleep(3_000)
                shell("input keyevent KEYCODE_WAKEUP")
                Thread.sleep(3_000)
                shell("am start -n ${context.packageName}/android.app.NativeActivity")
                Thread.sleep(4_000)

                // Right after boot a system window (the charging screen, a
                // first-run overlay) can still be in front of the game and
                // would swallow the tap. Give the game window a moment to
                // take the focus; carry on regardless, the tap loop below is
                // the real check.
                val focusDeadline = System.currentTimeMillis() + 15_000
                while (System.currentTimeMillis() < focusDeadline) {
                    val focus = shell("dumpsys window windows").lines()
                        .firstOrNull { it.contains("mCurrentFocus") } ?: ""
                    if (focus.contains("vulkanbreakoutwear")) break
                    Thread.sleep(500)
                }

                // On the title screen any tap away from the gear (bottom of the
                // disc) starts a round, so tap the centre. The activity logs
                // "Round started" when the game leaves the title, so poll for
                // that and tap again if it does not come: a tap that lands
                // mid-round only serves the resting ball, which is harmless.
                // Auto Play then plays; screenshot the round as it goes.
                val dm = context.resources.displayMetrics
                var roundStarted = false
                for (attempt in 1..5) {
                    shell("input tap ${dm.widthPixels / 2} ${dm.heightPixels / 2}")
                    val tapDeadline = System.currentTimeMillis() + 2_000
                    while (System.currentTimeMillis() < tapDeadline) {
                        if (shell("logcat -d -s BreakoutWear:I").contains("Round started")) {
                            roundStarted = true
                            break
                        }
                        Thread.sleep(250)
                    }
                    if (roundStarted) break
                    println("Tap $attempt did not start a round; focus is: " +
                        shell("dumpsys window windows").lines().firstOrNull { it.contains("mCurrentFocus") })
                }
                assertTrue("The round never started after 5 taps on the title screen", roundStarted)
                for (i in 1..8) {
                    Thread.sleep(2_000)
                    screenshot(String.format("%02d-autoplay", i))
                }

                // The game must have survived the round. Not RESUMED: a
                // system overlay (a notification, the charging screen on a
                // plug event) can still take the foreground on an emulator
                // and that is not the game's failure. DESTROYED would be.
                val after = scenario.state
                println("Activity state after the Auto Play round: $after")
                assertTrue(
                    "NativeActivity was destroyed during the Auto Play round",
                    after != Lifecycle.State.DESTROYED
                )
            }
        } finally {
            if (previousTimeout.isNotEmpty() && previousTimeout != "null") {
                shell("settings put system screen_off_timeout $previousTimeout")
            }
            shell("dumpsys battery reset")
            if (previous != null) settings.writeBytes(previous) else settings.delete()
        }
        println("Screenshots saved under $shotDir: " + shell("ls $shotDir").trim())
    }
}

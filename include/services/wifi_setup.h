#pragma once

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect(bool show_ui = true);
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). When tap_ms is
 *  non-null and a tap is returned, it's set to the millis() timestamp of
 *  the actual button release (ISR time) -- use this instead of millis() at
 *  poll time for double-tap interval math, since polling can be delayed by
 *  blocking work (e.g. the radar page's ADSB HTTP fetch). */
bool bootButtonConsumeTap(unsigned long* tap_ms = nullptr);
/** Latched medium hold (between kAutoCycleToggleHoldMs and kBootResetHoldMs). */
bool bootButtonConsumeAutoCycleToggle();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();

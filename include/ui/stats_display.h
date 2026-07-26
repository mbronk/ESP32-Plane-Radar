#pragma once

namespace ui {

/**
 * Brief single-screen "debug" page: WiFi SSID/signal/IP, radar center
 * lat/lon (plus city + elevation when known), current time (from the last
 * weather fetch), uptime, free heap, and tracked aircraft count. No
 * scrolling — everything must fit on the round 240x240 panel at once.
 */
void statsDisplayDraw();

}  // namespace ui

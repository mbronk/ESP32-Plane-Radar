#pragma once

namespace ui {

/** Draw the weather page (icon, condition, temperature, real feel). */
void weatherDisplayDraw();

/** Redraw only the weather page's bottom metric area when values change. */
void weatherDisplayDrawPartial();

/** Draw the weather loading state while fetching new weather data. */
void weatherDisplayLoading();

void weatherDisplayAdvanceForecast();
void weatherDisplayResetForecast();

/** Toggle the 30s auto-cycle (advances through Teraz/+2h/+6h/Jutro). Off by default. */
void weatherDisplayToggleAutoCycle();
bool weatherDisplayAutoCycleEnabled();
/** Call each loop iteration; advances the forecast slot every 30s while
 *  auto-cycle is on. Returns true when it just advanced (caller should
 *  do a full redraw). */
bool weatherDisplayAutoCycleTick();

}  // namespace ui

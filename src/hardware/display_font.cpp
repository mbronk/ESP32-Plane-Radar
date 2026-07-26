#include "hardware/display_font.h"

#include "hardware/display.h"

extern "C" {
extern const uint8_t _binary_data_ui_font_vlw_start[] asm(
    "_binary_data_ui_font_vlw_start");
extern const uint8_t _binary_data_ui_font_vlw_end[] asm("_binary_data_ui_font_vlw_end");
extern const uint8_t _binary_data_ui_header_font_vlw_start[] asm(
    "_binary_data_ui_header_font_vlw_start");
extern const uint8_t _binary_data_ui_header_font_vlw_end[] asm(
    "_binary_data_ui_header_font_vlw_end");
}

namespace {

// Tracks which VLW font (if any) is currently active, and on which gfx
// instance. This can't be a single flag shared across all callers: the
// radar screen renders into an off-screen sprite (a second, independent
// LGFXBase instance) while the weather/status screens draw straight to
// `tft`. Each LGFXBase/LGFX_Sprite tracks its own loaded font internally,
// so a font loaded onto `tft` is NOT loaded onto the sprite and vice
// versa — the cache below must be keyed by instance pointer, otherwise
// ensureLoaded() on the sprite can wrongly short-circuit (believing the
// font is already active there because it's active on `tft`) and leave
// the sprite on its tiny default font instead.
enum class ActiveVlw { kNone, kBody, kHeader };

bool s_vlw_loaded = false;
bool s_header_vlw_loaded = false;
const void* s_active_gfx = nullptr;
ActiveVlw s_active_vlw = ActiveVlw::kNone;

const uint8_t* vlwData() { return _binary_data_ui_font_vlw_start; }

size_t vlwDataLen() {
  return static_cast<size_t>(_binary_data_ui_font_vlw_end -
                               _binary_data_ui_font_vlw_start);
}

const uint8_t* headerVlwData() { return _binary_data_ui_header_font_vlw_start; }

size_t headerVlwDataLen() {
  return static_cast<size_t>(_binary_data_ui_header_font_vlw_end -
                               _binary_data_ui_header_font_vlw_start);
}

}  // namespace

bool displayFontInit() {
  s_vlw_loaded = vlwDataLen() > 0 &&
                 tft.loadFont(vlwData(), lgfx::IFont::font_type_t::ft_vlw);
  if (s_vlw_loaded) {
    s_active_gfx = &tft;
    s_active_vlw = ActiveVlw::kBody;
  } else {
    Serial.println("Smooth font load failed — using bitmap fallback");
  }

  s_header_vlw_loaded = headerVlwDataLen() > 0;
  if (!s_header_vlw_loaded) {
    Serial.println("Header smooth font missing — using bitmap fallback");
  }
  return s_vlw_loaded;
}

bool displayFontIsSmooth() { return s_vlw_loaded; }

bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx) {
  if (!s_vlw_loaded) {
    return false;
  }
  if (s_active_gfx == &gfx && s_active_vlw == ActiveVlw::kBody) {
    return true;
  }
  if (gfx.loadFont(vlwData(), lgfx::IFont::font_type_t::ft_vlw)) {
    s_active_gfx = &gfx;
    s_active_vlw = ActiveVlw::kBody;
    return true;
  }
  return false;
}

bool displayFontHeaderIsSmooth() { return s_header_vlw_loaded; }

bool displayFontHeaderEnsureLoaded(lgfx::LGFXBase& gfx) {
  if (!s_header_vlw_loaded) {
    return false;
  }
  if (s_active_gfx == &gfx && s_active_vlw == ActiveVlw::kHeader) {
    return true;
  }
  if (gfx.loadFont(headerVlwData(), lgfx::IFont::font_type_t::ft_vlw)) {
    s_active_gfx = &gfx;
    s_active_vlw = ActiveVlw::kHeader;
    return true;
  }
  return false;
}

void displayFontSetSmoothSize(lgfx::LGFXBase& gfx, float size) {
  gfx.setTextSize(size);
}

void displayFontSetBitmap(lgfx::LGFXBase& gfx, const lgfx::GFXfont* font) {
  gfx.setFont(font);
  gfx.setTextSize(1);
  if (s_active_gfx == &gfx) {
    s_active_vlw = ActiveVlw::kNone;
  }
}


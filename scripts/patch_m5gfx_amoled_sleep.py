"""Patch the pinned M5GFX StopWatch framebuffer power forwarding.

The active StopWatch panel is Panel_AMOLED_Framebuffer.  At the pinned M5GFX
revision that wrapper intentionally leaves sleep, wait, and repeat init as
no-ops, so M5.Display.sleep() only changes brightness and a powered-off AMOLED
cannot be initialized again.  Keep this small patch next to the firmware so a
fresh PlatformIO checkout behaves the same as the development machine.
"""

from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


def replace_once(text: str, old: str, new: str, path: Path) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"M5GFX patch expected one matching block in {path}, found {count}"
        )
    return text.replace(old, new, 1)


libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR"))  # type: ignore[name-defined]
pio_env = env.subst("$PIOENV")  # type: ignore[name-defined]
m5gfx_dir = libdeps_dir / pio_env / "M5GFX"
header = m5gfx_dir / "src/lgfx/v1/panel/Panel_AMOLED.hpp"
source = m5gfx_dir / "src/lgfx/v1/panel/Panel_AMOLED.cpp"

if not header.is_file() or not source.is_file():
    raise RuntimeError(
        "Pinned M5GFX dependency is missing; let PlatformIO install lib_deps "
        f"for {pio_env} before compiling"
    )

header_text = header.read_text(encoding="utf-8")
source_text = source.read_text(encoding="utf-8")
original_header = header_text
original_source = source_text

marker = "CODEX_STOPWATCH_AMOLED_POWER_FORWARDING"
if marker not in header_text:
    header_text = replace_once(
        header_text,
        """            void setInvert(bool invert) override;
            void setBrightness(uint8_t brightness) override;
            uint_fast8_t getTouchRaw(touch_point_t* tp, uint_fast8_t count) override;
""",
        """            void setInvert(bool invert) override;
            void setBrightness(uint8_t brightness) override;
            // CODEX_STOPWATCH_AMOLED_POWER_FORWARDING
            void setSleep(bool flg) override;
            void setPowerSave(bool flg) override;
            void waitDisplay(void) override;
            bool displayBusy(void) override;
            uint_fast8_t getTouchRaw(touch_point_t* tp, uint_fast8_t count) override;
""",
        header,
    )

if marker not in source_text:
    source_text = replace_once(
        source_text,
        """        bool Panel_AMOLED_Framebuffer::init(bool use_reset)
        {
            if( _frame_buffer )
              return true;
""",
        """        bool Panel_AMOLED_Framebuffer::init(bool use_reset)
        {
            // CODEX_STOPWATCH_AMOLED_POWER_FORWARDING
            // Re-run the physical controller init after the shared L3B rail
            // has been removed while preserving the allocated framebuffer.
            if( _frame_buffer )
              return _panel && _panel->init(use_reset);
""",
        source,
    )
    source_text = replace_once(
        source_text,
        """        void Panel_AMOLED_Framebuffer::setBrightness(uint8_t brightness)
        {
            _panel->setBrightness(brightness);
        }

        uint_fast8_t Panel_AMOLED_Framebuffer::getTouchRaw(touch_point_t* tp, uint_fast8_t count)
""",
        """        void Panel_AMOLED_Framebuffer::setBrightness(uint8_t brightness)
        {
            _panel->setBrightness(brightness);
        }

        void Panel_AMOLED_Framebuffer::setSleep(bool flg)
        {
            if (_panel) _panel->setSleep(flg);
        }

        void Panel_AMOLED_Framebuffer::setPowerSave(bool flg)
        {
            if (_panel) _panel->setPowerSave(flg);
        }

        void Panel_AMOLED_Framebuffer::waitDisplay(void)
        {
            if (_panel) _panel->waitDisplay();
        }

        bool Panel_AMOLED_Framebuffer::displayBusy(void)
        {
            return _panel && _panel->displayBusy();
        }

        uint_fast8_t Panel_AMOLED_Framebuffer::getTouchRaw(touch_point_t* tp, uint_fast8_t count)
""",
        source,
    )

if header_text != original_header:
    header.write_text(header_text, encoding="utf-8")
if source_text != original_source:
    source.write_text(source_text, encoding="utf-8")
verb = "Applied" if (header_text != original_header or source_text != original_source) else "Verified"
print(f"{verb} StopWatch AMOLED power forwarding in {m5gfx_dir}")

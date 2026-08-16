"""Patch M5GFX for Waveshare ESP32-S3-Touch-AMOLED-1.75C third-party support.

Without this patch a clean PlatformIO checkout of the pinned M5GFX would:
1. Never autodetect the Waveshare board (its CST92xx touch at 0x5A on
   GPIO14/15 is not in M5Stack's probe list), so the CO5300 AMOLED would
   stay at 0x0 and canvas allocation would fail.
2. Reject the CST9217 touch chip in Touch_CST226 (chip id 0x92xx vs 0xa8).

This script keeps those edits reproducible next to the firmware, mirroring
scripts/patch_m5gfx_amoled_sleep.py. It is idempotent: markers guard every
replacement.
"""

from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


def replace_once(text: str, old: str, new: str, path: Path) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"M5GFX waveshare patch expected one matching block in {path}, "
            f"found {count}"
        )
    return text.replace(old, new, 1)


libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR"))  # type: ignore[name-defined]
pio_env = env.subst("$PIOENV")  # type: ignore[name-defined]
m5gfx_dir = libdeps_dir / pio_env / "M5GFX"
source = m5gfx_dir / "src/M5GFX.cpp"
touch = m5gfx_dir / "src/lgfx/v1/touch/Touch_CSTxxx.cpp"

if not source.is_file() or not touch.is_file():
    raise RuntimeError(
        "Pinned M5GFX dependency is missing; let PlatformIO install lib_deps "
        f"for {pio_env} before compiling"
    )

source_text = source.read_text(encoding="utf-8")
touch_text = touch.read_text(encoding="utf-8")
original_source = source_text
original_touch = touch_text

marker = "CODEX_STOPWATCH_THIRD_PARTY"

# 1) Waveshare autodetect branch inserted before the M5Stack CoreS3 probe.
if marker not in source_text:
    source_text = replace_once(
        source_text,
        """    switch (pkg_ver) {
    case 0: // EFUSE_PKG_VERSION_ESP32S3:     // QFN56

      if (board == 0 || board == board_t::board_M5StackCoreS3 || board == board_t::board_M5StackCoreS3SE
          || board == board_t::board_M5StackChan)
""",
        """    switch (pkg_ver) {
    case 0: // EFUSE_PKG_VERSION_ESP32S3:     // QFN56

      // CODEX_STOPWATCH_THIRD_PARTY: Waveshare ESP32-S3-Touch-AMOLED-1.75C.
      // CO5300 466x466 QSPI AMOLED + CST92xx (CST9217) touch at 0x5A on
      // GPIO14(SCL)/15(SDA). Must run before the M5Stack autodetect steps,
      // which can otherwise misdetect this board (e.g. as M5PowerHub).
      if (board == 0)
      {
        static constexpr int_fast16_t ws_i2c_sda = GPIO_NUM_15;
        static constexpr int_fast16_t ws_i2c_scl = GPIO_NUM_14;
        static constexpr uint8_t ws_i2c_addr_list[] = { 0x5A, 0 }; // CST92xx touch
        uint32_t ws_i2c_result = _detect_i2c_device(ws_i2c_sda, ws_i2c_scl, ws_i2c_addr_list);
        if (ws_i2c_result & 1) // found CST92xx at 0x5A
        {
          board = board_t::board_M5StopWatch;
          ESP_LOGI(LIBRARY_NAME, "[Autodetect] board_M5StopWatch (Waveshare 1.75C)");

          // LCD reset (GPIO1) and touch reset (GPIO2) high; Touch_CST226::init
          // later pulses the touch reset itself.
          _pin_level(GPIO_NUM_1, true);
          _pin_level(GPIO_NUM_2, true);
          lgfx::delay(10);

          bus_cfg.pin_mosi = GPIO_NUM_NC;
          bus_cfg.pin_miso = GPIO_NUM_NC;
          bus_cfg.pin_io0 = GPIO_NUM_4;
          bus_cfg.pin_io1 = GPIO_NUM_5;
          bus_cfg.pin_io2 = GPIO_NUM_6;
          bus_cfg.pin_io3 = GPIO_NUM_7;
          bus_cfg.pin_sclk = GPIO_NUM_38;
          bus_cfg.spi_mode = 0;
          bus_cfg.spi_3wire = true;

          bus_cfg.spi_host = SPI2_HOST;
          bus_cfg.freq_write = 80000000;
          bus_cfg.freq_read  = 1000000;
          bus_spi->config(bus_cfg);
          bus_spi->init();

          auto p = new Panel_StopWatch();
          p->bus(bus_spi);
          {
            auto cfg = p->config();
            cfg.pin_cs = GPIO_NUM_12;
            cfg.pin_rst = GPIO_NUM_NC;
            cfg.pin_busy = GPIO_NUM_NC;
            cfg.panel_width = 468;
            cfg.panel_height = 468;
            cfg.offset_x = 6;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = false;
            cfg.bus_shared = false;
            p->config(cfg);
            p->setRotation(0);

            // OLED TE pin
            lgfx::pinMode(GPIO_NUM_NC, lgfx::pin_mode_t::input_pullup);
          }
          _panel_last.reset(p);

          {
            auto t = new lgfx::Touch_CST226();
            _touch_last.reset(t);
            auto cfg = t->config();
            // CODEX_STOPWATCH_THIRD_PARTY: no INT pin. The CST9217 on this
            // board does not drive GPIO11 the way Touch_CST226 expects; with
            // pin_int set, getTouchRaw bails out whenever the line reads high,
            // so coordinates freeze mid-swipe (dx=dy=0). Polling without an
            // interrupt keeps clicks and swipes updating.
            cfg.pin_int  = GPIO_NUM_NC;
            cfg.pin_rst  = GPIO_NUM_2;
            cfg.pin_sda  = GPIO_NUM_15;
            cfg.pin_scl  = GPIO_NUM_14;
            cfg.i2c_port = I2C_NUM_1;

            cfg.freq = 400000;
            cfg.x_min = 0;
            cfg.x_max = 465;
            cfg.y_min = 0;
            cfg.y_max = 465;
            cfg.offset_rotation = 0;
            cfg.bus_shared = false;
            t->config(cfg);
            _panel_last->touch(t);
          }

          goto init_clear;
        }
      }

      if (board == 0 || board == board_t::board_M5StackCoreS3 || board == board_t::board_M5StackCoreS3SE
          || board == board_t::board_M5StackChan)
""",
        source,
    )

    # 2) Relax the StopWatch probe: accept any board with a CST820-family touch
    #    even when M5PM1/M5IOE1 are absent (Waveshare lacks both).
    source_text = replace_once(
        source_text,
        """        const bool is_stopwatch = i2c_result == ~0b0001u; // with CST820, no NFC == StopWatch
        const bool is_papermono = (i2c_result & ~1u) == ~0b0011u; // no CST820, with NFC == PaperMono,PaperMono Pro
        if (is_stopwatch || is_papermono) {
          gpio::pin_backup_t backup_pins[] = { GPIO_NUM_47, GPIO_NUM_48 };
          lgfx::i2c::init(i2c_port, stopwatch_i2c_sda, stopwatch_i2c_scl);
          if (_check_m5pm1(i2c_port) && _check_m5ioe1(i2c_port)) {
""",
        """        const bool is_stopwatch = (i2c_result & 0b0010u) != 0; // with CST820 touch present (third-party C152-compatible boards may lack RX8130/BMI270/NFC)
        const bool is_papermono = (i2c_result & ~1u) == ~0b0011u; // no CST820, with NFC == PaperMono,PaperMono Pro
        if (is_stopwatch || is_papermono) {
          gpio::pin_backup_t backup_pins[] = { GPIO_NUM_47, GPIO_NUM_48 };
          lgfx::i2c::init(i2c_port, stopwatch_i2c_sda, stopwatch_i2c_scl);
          const bool m5_chips_ok = _check_m5pm1(i2c_port) && _check_m5ioe1(i2c_port);
          // CODEX_STOPWATCH_THIRD_PARTY: third-party C152-compatible boards
          // (e.g. ESP32-S3-Touch-AMOLED-1.75C) lack M5PM1/M5IOE1. Register
          // writes below are harmless no-ops over I2C, but the StopWatch
          // panel init must still run so the AMOLED display comes up.
          if (m5_chips_ok || is_stopwatch) {
""",
        source,
    )

# 3) Touch_CST226 chip-id check: also accept CST92xx (0x92xx) family so the
#    CST9217 on Waveshare boards passes init.
if marker not in touch_text:
    touch_text = replace_once(
        touch_text,
        """        if( c_p_info[1] != 0xa8 ) // 0xa8 is chip ID for CST226 and CST226SE
          return false;""",
        """        // CODEX_STOPWATCH_THIRD_PARTY: 0xa8 is CST226/CST226SE; CST92xx
        // (CST9217 on Waveshare 1.75C) reports 0x92xx over the same map.
        if( (c_p_info[1] & 0xff00) != 0xa800 && (c_p_info[1] & 0xff00) != 0x9200 )
          return false;""",
        touch,
    )

if source_text != original_source:
    source.write_text(source_text, encoding="utf-8")
if touch_text != original_touch:
    touch.write_text(touch_text, encoding="utf-8")
verb = "Applied" if (source_text != original_source or touch_text != original_touch) else "Verified"
print(f"{verb} Waveshare 1.75C M5GFX support in {m5gfx_dir}")

"""Gate M5Unified's fixed-rate microphone DC servo behind a build macro.

M5Unified applies the same ``1 / 32`` DC estimate update for every microphone
sample, regardless of sample rate.  At the USB microphone's 48 kHz rate that
acts as an additional high-pass filter with an approximately 121 Hz corner.
The StopWatch ES8311 already provides dynamic DC cancellation in register
0x1C, so the optional USB-mic image disables only this software stage.

The pinned dependency remains unchanged for every build that does not define
``CODEX_STOPWATCH_DISABLE_M5_MIC_DC_SERVO``.  This script is intentionally
strict and idempotent: it either applies the expected one-block patch, verifies
the exact patched block, or stops the build when the pinned source no longer
matches the audited implementation.
"""

from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


libdeps_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR"))  # type: ignore[name-defined]
pio_env = env.subst("$PIOENV")  # type: ignore[name-defined]
source = libdeps_dir / pio_env / "M5Unified" / "src" / "utility" / "Mic_Class.cpp"

if not source.is_file():
    raise RuntimeError(
        "Pinned M5Unified dependency is missing; let PlatformIO install lib_deps "
        f"for {pio_env} before compiling"
    )

original = """        auto value_tmp = (sv0 + sv1) << 3;
        int32_t offset = self->_offset;
        // Automatic zero level adjustment
        offset -= (value_tmp + offset + 16) >> 5;
        self->_offset = offset;
        offset = (offset + 8) >> 4;
        sum_value[0] = sv0 + offset;
        sum_value[1] = sv1 + offset;
"""

patched = """#if defined(CODEX_STOPWATCH_DISABLE_M5_MIC_DC_SERVO)
        // CODEX_STOPWATCH_OPTIONAL_MIC_DC_SERVO
        // The ES8311 hardware keeps dynamic DC cancellation enabled.  Avoid
        // stacking M5Unified's fixed-per-sample servo in the 48 kHz USB path.
        sum_value[0] = sv0;
        sum_value[1] = sv1;
#else
        auto value_tmp = (sv0 + sv1) << 3;
        int32_t offset = self->_offset;
        // Automatic zero level adjustment
        offset -= (value_tmp + offset + 16) >> 5;
        self->_offset = offset;
        offset = (offset + 8) >> 4;
        sum_value[0] = sv0 + offset;
        sum_value[1] = sv1 + offset;
#endif
"""

text = source.read_text(encoding="utf-8")
original_count = text.count(original)
patched_count = text.count(patched)

if patched_count == 1:
    # The complete upstream block intentionally remains inside #else, so the
    # smaller `original` string is also present after a successful patch.
    verb = "Verified"
elif original_count == 1:
    source.write_text(text.replace(original, patched, 1), encoding="utf-8")
    verb = "Applied"
else:
    raise RuntimeError(
        "M5Unified microphone patch source changed unexpectedly: "
        f"original={original_count}, patched={patched_count}, source={source}"
    )

print(f"{verb} optional USB-mic DC-servo gate in {source.parent.parent.parent}")

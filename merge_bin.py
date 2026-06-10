"""PlatformIO post-build script: merge bootloader + partitions + app into one flashable .bin"""
import os

Import("env")

def merge_bin_callback(source, target, env):
    chip = env.get("BOARD_MCU", "esp32")
    flash_size = env.get("BOARD_BUILD_FLASH_SIZE", "4MB")
    flash_mode = env.get("BOARD_FLASH_MODE", "dio")

    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    env_name = env.subst("$PIOENV")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware   = os.path.join(build_dir, "firmware.bin")
    merged     = os.path.join(project_dir, f"merged-firmware-{env_name}.bin")

    if not os.path.isfile(bootloader):
        print(f"merge_bin: SKIP — {bootloader} not found")
        return
    if not os.path.isfile(partitions):
        print(f"merge_bin: SKIP — {partitions} not found")
        return
    if not os.path.isfile(firmware):
        print(f"merge_bin: SKIP — {firmware} not found")
        return

    # esptool is installed alongside PlatformIO
    esptool = os.path.join(
        env.subst("$PROJECT_CORE_DIR"), "packages", "tool-esptoolpy", "esptool.py"
    )
    if not os.path.isfile(esptool):
        # Fallback: search PATH
        esptool = "esptool.py"

    cmd = (
        f'python "{esptool}" --chip {chip} merge_bin '
        f'-o "{merged}" '
        f'--flash_mode {flash_mode} '
        f'--flash_size {flash_size} '
        f'0x1000 "{bootloader}" '
        f'0x8000 "{partitions}" '
        f'0x10000 "{firmware}"'
    )
    print(f"merge_bin: {cmd}")
    ret = os.system(cmd)
    if ret == 0:
        size_mb = os.path.getsize(merged) / (1024 * 1024)
        print(f"merge_bin: OK → {merged} ({size_mb:.2f} MB)")
    else:
        print(f"merge_bin: FAILED (exit code {ret})")

# Hook into the firmware.bin target rather than buildprog —
# on ESP32, buildprog may fire before firmware.bin is generated
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_bin_callback)

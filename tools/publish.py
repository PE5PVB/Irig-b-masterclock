# PlatformIO post-build script: copies the flash images to publish/ so they
# can be flashed with publish/flash.bat.
#
# The images are published separately (not as one merged factory image), so
# flashing leaves the NVS partition with the WiFi settings untouched.
# flash_args.txt lists "offset file" per image for flash.bat.
import shutil
from pathlib import Path

Import("env")


def copy_to_publish(source, target, env):
    out_dir = Path(env.subst("$PROJECT_DIR")) / "publish"
    out_dir.mkdir(exist_ok=True)

    images = [(str(offset), Path(env.subst(path))) for offset, path in env.get("FLASH_EXTRA_IMAGES", [])]
    images.append((env.subst("$ESP32_APP_OFFSET") or "0x10000",
                   Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))))

    args = []
    for offset, src in images:
        name = "IRIG_B_Masterclock.bin" if src.name == env.subst("${PROGNAME}.bin") else src.name
        shutil.copyfile(src, out_dir / name)
        args.append(f"{offset} {name}")
        print(f"publish: {offset.ljust(8)} {name}")

    (out_dir / "flash_args.txt").write_text("\n".join(args) + "\n")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin",
                  env.VerboseAction(copy_to_publish, "Copying firmware to publish/"))

Import("env")
import shutil, os

def copy_firmware(source, target, env):
    src = str(target[0])
    dst = os.path.join(env.subst("$PROJECT_DIR"), "build", "firmware.bin")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print(f"Firmware copied to build/firmware.bin")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_firmware)

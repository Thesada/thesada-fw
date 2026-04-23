Import("env")
import os, glob

# 1. Add Arduino framework library include paths globally.
# PlatformIO LDF can't chain-discover framework libs (SPI, Wire, etc)
# when they're only referenced by transitive dependencies in lib/.
#
# pioarduino envs (esp32-cyd, esp32-wroom, esp32-eth) sometimes return
# None from get_package_dir even though the package is installed globally,
# so fall back to the default ~/.platformio path before giving up.
framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
if not framework_dir:
    fallback = os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32")
    if os.path.isdir(fallback):
        framework_dir = fallback

if framework_dir:
    framework_libs = os.path.join(framework_dir, "libraries")
    needed = ["SPI", "Wire", "FS", "SD", "SD_MMC", "LittleFS",
              "WiFi", "WiFiClientSecure", "HTTPClient", "Update", "Ethernet"]
    for lib in needed:
        lib_src = os.path.join(framework_libs, lib, "src")
        if os.path.isdir(lib_src):
            env.Append(CCFLAGS=["-isystem", lib_src])
        else:
            lib_dir = os.path.join(framework_libs, lib)
            if os.path.isdir(lib_dir):
                env.Append(CCFLAGS=["-isystem", lib_dir])
else:
    print("WARN: framework-arduinoespressif32 package_dir not resolvable; "
          "LDF fallback only - transitive framework headers may fail to resolve.")

# 2. Force linker to include all .o files from thesada-mod-* archives.
# MODULE_REGISTER creates static constructors that self-register modules
# before setup(). But the linker only pulls .o files from .a archives
# when they resolve undefined symbols. Since main.cpp has zero module
# includes, the linker skips all module .o files and the static
# constructors never run.
# --whole-archive forces the linker to include every .o from the archive.
def force_whole_archive(source, target, env):
    # Find all thesada-mod-* .a files in the build directory
    build_dir = env.subst("$BUILD_DIR")
    link_cmd = env["LINKCOM"]

    # Get all .a files from our local libs
    archives = []
    for d in glob.glob(os.path.join(build_dir, "lib*")):
        for a in glob.glob(os.path.join(d, "*.a")):
            basename = os.path.basename(a)
            if "thesada-mod" in basename or "thesada-core" in basename:
                archives.append(a)

    if archives:
        # Inject --whole-archive around our lib archives
        whole = ["-Wl,--whole-archive"] + archives + ["-Wl,--no-whole-archive"]
        env.Append(LINKFLAGS=whole)

env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", force_whole_archive)

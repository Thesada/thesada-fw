Import("env")
import hashlib, json, re, os

def generate_manifest(source, target, env):
    """Generate firmware.json manifest with SHA256 hash after build.

    Reads FIRMWARE_VERSION from config.h, computes SHA256 of the compiled
    binary, and writes a manifest JSON file. This file is what nodes fetch
    to check for updates.

    Output: build/firmware.json alongside build/firmware.bin
    """
    project_dir = env.subst("$PROJECT_DIR")
    bin_path = os.path.join(project_dir, "build", "firmware.bin")
    manifest_path = os.path.join(project_dir, "build", "firmware.json")
    config_h = os.path.join(project_dir, "src", "thesada_config.h")

    if not os.path.exists(bin_path):
        print("WARNING: build/firmware.bin not found - skipping manifest generation")
        return

    # Read version from config.h
    version = "0.0.0"
    if os.path.exists(config_h):
        with open(config_h, "r") as f:
            for line in f:
                match = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', line)
                if match:
                    version = match.group(1)
                    break

    # Compute SHA256
    sha256 = hashlib.sha256()
    with open(bin_path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            sha256.update(chunk)
    digest = sha256.hexdigest()

    # Get binary size
    bin_size = os.path.getsize(bin_path)

    # Write manifest
    # "url" points to the specific release asset for this version so the
    # SHA256 check matches exactly. The manifest itself is published as the
    # "latest/download" permalink so devices always fetch the newest manifest.
    GITHUB_REPO = "Thesada/thesada-fw"
    url = f"https://github.com/{GITHUB_REPO}/releases/download/v{version}/firmware.bin"
    manifest = {
        "version": version,
        "url": url,
        "sha256": digest,
        "size": bin_size
    }

    os.makedirs(os.path.dirname(manifest_path), exist_ok=True)
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"Manifest: build/firmware.json")
    print(f"  version: {version}")
    print(f"  sha256:  {digest}")
    print(f"  size:    {bin_size} bytes")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", generate_manifest)

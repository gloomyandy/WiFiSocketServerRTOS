# Generates a CycloneDX 1.6 SBOM describing the firmware images in the build output directories.
#
# The component list is derived from the linker map of each image, so only libraries that are
# actually linked in appear in the SBOM. Versions are read out of the SDK and toolchain trees
# rather than hard-coded, which keeps the SBOM honest when the SDK forks move on.

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import uuid

from datetime import datetime, timezone
from pathlib import Path

TARGETS = [
    {"dir": "ESP8266", "image": "DuetWiFiServer.bin", "chip": "ESP8266", "sdk": "esp8266"},
    {"dir": "ESP32", "image": "DuetWiFiModule_32.bin", "chip": "ESP32", "sdk": "idf"},
    {"dir": "ESP32S3", "image": "DuetWiFiModule_32S3.bin", "chip": "ESP32-S3", "sdk": "idf"},
    {"dir": "ESP32C3", "image": "DuetWiFiModule_32C3.bin", "chip": "ESP32-C3", "sdk": "idf"},
]

MAP_NAME = "WiFiSocketServerRTOS.map"

# Archive basename -> package key. Anything not listed here is part of the SDK itself
ARCHIVES = {
    "liblwip.a": "lwip",
    "libmbedtls.a": "mbedtls",
    "libmbedcrypto.a": "mbedtls",
    "libmbedx509.a": "mbedtls",
    "libfreertos.a": "freertos",
    "libspiffs.a": "spiffs",
    "libwpa_supplicant.a": "wpa_supplicant",
    "libmdns.a": "mdns",
    "libindicator.a": "led_indicator",
    "libc_nano.a": "newlib",
    "libm.a": "newlib",
    "libgcc.a": "gcc",
    "libgcov.a": "gcc",
    "libstdc++.a": "gcc",
    "libcore.a": "wifi_lib",
    "libnet80211.a": "wifi_lib",
    "libpp.a": "wifi_lib",
    "libmesh.a": "wifi_lib",
    "libcoexist.a": "wifi_lib",
    "libespnow.a": "wifi_lib",
    "libsmartconfig.a": "wifi_lib",
    "libwapi.a": "wifi_lib",
    "libssc.a": "wifi_lib",
    "libclk.a": "wifi_lib",
    "libphy.a": "phy_lib",
    "librtc.a": "phy_lib",
}

# Everything the ESP8266 SDK ships prebuilt lives here, including archives that are built from
# source on the ESP32 targets
ESP8266_BLOB_DIR = "/components/esp8266/lib/"

# Sources vendored into src/ that are not ours
VENDORED = [
    {
        "key": "hspi",
        "name": "esp8266-arduino-spi",
        "description": "SPI library from the esp8266 core for Arduino, adapted as HSPI",
        "licenses": [{"license": {"id": "LGPL-2.1-or-later"}}],
        "externalReferences": [{"type": "website", "url": "https://github.com/esp8266/Arduino"}],
    },
    {
        "key": "ecv",
        "name": "ecv",
        "description": "Escher C Verifier annotation header",
        "licenses": [{"license": {"name": "Escher Technologies eCv annotation header licence, free use and distribution for annotating C/C++ programs"}}],
        "externalReferences": [{"type": "website", "url": "https://eschertech.com"}],
    },
]


def run(cmd, cwd=None):
    try:
        return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def read(path):
    try:
        return Path(path).read_text(errors="replace")
    except OSError:
        return None


def project_version(project):
    text = read(project / "src/Config.h") or ""
    match = re.search(r'#define\s+VERSION_MAIN\s+"([^"]+)"', text)
    if not match:
        sys.exit("could not read VERSION_MAIN from src/Config.h")
    return match.group(1)


def linked_archives(map_file):
    # The map starts with "Archive member included to satisfy reference by file", one archive
    # member per line, and ends at the common symbol allocation
    archives = {}
    for line in read(map_file).splitlines():
        if line.startswith("Allocating common symbols"):
            break
        match = re.match(r"^(\S+\.a)\(", line)
        if match:
            archives.setdefault(os.path.basename(match.group(1)), match.group(1))
    return archives


def classify(archives):
    classified = {}
    for archive, path in archives.items():
        key = ARCHIVES.get(archive)
        if not key and ESP8266_BLOB_DIR in path:
            key = "wifi_lib"
        if key:
            classified[archive] = key
    return classified


def git_describe(repo):
    return run(["git", "describe", "--tags", "--dirty"], cwd=repo) or "unknown"


def git_commit(repo):
    return run(["git", "rev-parse", "HEAD"], cwd=repo)


def git_remote(repo):
    url = run(["git", "remote", "get-url", "origin"], cwd=repo)
    if url and url.startswith("git@github.com:"):
        url = "https://github.com/" + url[len("git@github.com:"):]
    if url and url.endswith(".git"):
        url = url[:-len(".git")]
    return url


def submodule_commit(repo, path):
    status = run(["git", "submodule", "status", path], cwd=repo)
    return status.split()[0].lstrip("-+") if status else None


def release_tag(describe):
    return re.sub(r"^v|-\d+-g[0-9a-f]+(-dirty)?$", "", describe)


def lwip_version(sdk):
    text = read(sdk / "components/lwip/lwip/src/include/lwip/init.h") or ""
    parts = [re.search(rf"#define\s+LWIP_VERSION_{field}\s+(\d+)", text) for field in ("MAJOR", "MINOR", "REVISION")]
    return ".".join(part.group(1) for part in parts) if all(parts) else None


def mbedtls_version(sdk):
    text = read(sdk / "components/mbedtls/mbedtls/include/mbedtls/version.h") or ""
    match = re.search(r'#define\s+MBEDTLS_VERSION_STRING\s+"([^"]+)"', text)
    return match.group(1) if match else None


def freertos_version(sdk):
    text = read(sdk / "components/freertos/include/freertos/task.h") or ""
    match = re.search(r'#define\s+tskKERNEL_VERSION_NUMBER\s+"V?([^"]+)"', text)
    return match.group(1) if match else None


def toolchain_info(archives):
    # Toolchain archives sit under .espressif/tools/<triple>/<release>/ and the GCC version is a
    # path component of the libgcc/libstdc++ search path
    info = {}
    for path in archives.values():
        match = re.search(r"/tools/([^/]+)/([^/]+)/", path)
        if match:
            info["triple"], info["release"] = match.group(1), match.group(2)
        match = re.search(r"/lib/gcc/[^/]+/([0-9][0-9.]*)/", path)
        if match:
            info["gcc"] = match.group(1)
        if "libc_nano.a" in path or "libstdc++.a" in path:
            info["newlib"] = newlib_version(path)
    return info


def newlib_version(archive):
    directory = Path(os.path.realpath(archive)).parent
    for _ in range(8):
        for header in directory.glob("*/include/_newlib_version.h"):
            match = re.search(r'#define\s+_NEWLIB_VERSION\s+"([^"]+)"', read(header) or "")
            if match:
                return match.group(1)
        directory = directory.parent
    return None


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as image:
        for chunk in iter(lambda: image.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sdk_component(sdk, kind):
    describe = git_describe(sdk)
    tag = release_tag(describe)
    upstream = "espressif/esp-idf" if kind == "idf" else "espressif/ESP8266_RTOS_SDK"
    component = {
        "bom-ref": f"{sdk.name}@{describe}",
        "type": "framework",
        "name": "esp-idf" if kind == "idf" else "ESP8266_RTOS_SDK",
        "version": describe,
        "description": f"Espressif SDK, built from the Duet3D fork at {git_commit(sdk)}",
        "supplier": {"name": "Espressif Systems"},
        "licenses": [{"license": {"id": "Apache-2.0"}}],
        "purl": f"pkg:github/{upstream}@v{tag}",
        "externalReferences": [
            {"type": "vcs", "url": f"{git_remote(sdk)}.git"},
            {"type": "website", "url": f"https://github.com/{upstream}"},
        ],
    }
    if kind == "idf":
        component["cpe"] = f"cpe:2.3:a:espressif:esp-idf:{tag}:*:*:*:*:*:*:*"
    return component


def library(key, sdk, kind, classified, toolchain):
    sdk_version = git_describe(sdk)
    fork = f"vendored in the Duet3D {sdk.name} fork"

    if key == "lwip":
        version = lwip_version(sdk)
        return {
            "name": "lwip",
            "version": version,
            "description": f"Espressif esp-lwip fork, {fork}",
            "licenses": [{"license": {"id": "BSD-3-Clause"}}],
            "purl": f"pkg:github/espressif/esp-lwip@{version}",
            "cpe": f"cpe:2.3:a:lwip_project:lwip:{version}:*:*:*:*:*:*:*",
            "externalReferences": [{"type": "website", "url": "https://savannah.nongnu.org/projects/lwip/"}],
        }

    if key == "mbedtls":
        version = mbedtls_version(sdk)
        return {
            "name": "mbedtls",
            "version": version,
            "description": f"Mbed TLS, {fork}",
            "licenses": [{"license": {"id": "Apache-2.0"}}],
            "purl": f"pkg:github/Mbed-TLS/mbedtls@mbedtls-{version}",
            "cpe": f"cpe:2.3:a:arm:mbed_tls:{version}:*:*:*:*:*:*:*",
            "externalReferences": [{"type": "website", "url": "https://www.trustedfirmware.org/projects/mbed-tls/"}],
        }

    if key == "freertos":
        version = freertos_version(sdk)
        return {
            "name": "freertos-kernel",
            "version": version,
            "description": f"FreeRTOS kernel with the Espressif SMP port, {fork}",
            "licenses": [{"license": {"id": "MIT"}}],
            "purl": f"pkg:github/FreeRTOS/FreeRTOS-Kernel@V{version}",
            "externalReferences": [{"type": "website", "url": "https://www.freertos.org/"}],
        }

    if key == "spiffs":
        version = submodule_commit(sdk, "components/spiffs/spiffs") or sdk_version
        return {
            "name": "spiffs",
            "version": version,
            "description": f"SPI flash file system, {fork}",
            "licenses": [{"license": {"id": "MIT"}}],
            "purl": f"pkg:github/pellepl/spiffs@{version}",
            "externalReferences": [{"type": "website", "url": "https://github.com/pellepl/spiffs"}],
        }

    if key == "wpa_supplicant":
        return {
            "name": "wpa_supplicant",
            "version": sdk_version,
            "description": f"Espressif WPA supplicant port, no upstream hostap version is recorded in the tree, {fork}",
            "licenses": [{"license": {"id": "BSD-3-Clause"}}],
            "externalReferences": [{"type": "website", "url": "https://w1.fi/wpa_supplicant/"}],
        }

    if key == "mdns":
        return {
            "name": "mdns",
            "version": sdk_version,
            "description": f"Espressif mDNS responder carrying the Duet3D NSEC patch, {fork}",
            "licenses": [{"license": {"id": "Apache-2.0"}}],
            "externalReferences": [{"type": "website", "url": "https://github.com/espressif/esp-protocols"}],
        }

    if key == "led_indicator":
        return {
            "name": "led_indicator",
            "description": "LED indicator component from ESP-IoT-Solution, vendored in components/indicator",
            "licenses": [{"license": {"id": "Apache-2.0"}}],
            "externalReferences": [{"type": "website", "url": "https://github.com/espressif/esp-iot-solution"}],
        }

    if key == "newlib":
        return {
            "name": "newlib",
            "version": toolchain.get("newlib"),
            "description": f"newlib-nano C library from the {toolchain.get('triple', 'Espressif')} toolchain {toolchain.get('release', '')}".strip(),
            "licenses": [{"license": {"name": "Newlib licence, a collection of BSD-style and public domain licences, see COPYING.NEWLIB"}}],
            "externalReferences": [{"type": "website", "url": "https://sourceware.org/newlib/"}],
        }

    if key == "gcc":
        return {
            "name": "gcc-runtime",
            "version": toolchain.get("gcc"),
            "description": f"libgcc and libstdc++ from the {toolchain.get('triple', 'Espressif')} toolchain {toolchain.get('release', '')}".strip(),
            "licenses": [{"expression": "GPL-3.0-or-later WITH GCC-exception-3.1"}],
            "externalReferences": [{"type": "website", "url": "https://gcc.gnu.org/"}],
        }

    if key in ("wifi_lib", "phy_lib"):
        name = "esp-wifi-lib" if key == "wifi_lib" else "esp-phy-lib"
        linked = sorted(archive for archive, package in classified.items() if package == key)
        blobs = ", ".join(linked)
        if kind == "idf":
            path = "components/esp_wifi/lib" if key == "wifi_lib" else "components/esp_phy/lib"
            return {
                "name": name,
                "version": submodule_commit(sdk, path) or sdk_version,
                "description": f"Closed-source Espressif {'Wi-Fi' if key == 'wifi_lib' else 'PHY and RF calibration'} libraries ({blobs})",
                "licenses": [{"license": {"id": "Apache-2.0"}}],
                "externalReferences": [{"type": "website", "url": f"https://github.com/espressif/{name}"}],
            }
        return {
            "name": f"esp8266-{'wifi' if key == 'wifi_lib' else 'phy'}-lib",
            "version": sdk_version,
            "description": f"Closed-source Espressif libraries shipped as prebuilt binaries in {sdk.name}, with no separate licence text ({blobs})",
            "externalReferences": [{"type": "website", "url": "https://github.com/espressif/ESP8266_RTOS_SDK"}],
        }

    return None


def build_sbom(project, idf_path, esp8266_path, output):
    version = project_version(project)
    components = {}
    firmware = []
    dependencies = []
    repository = git_remote(project) or "https://github.com/Duet3D/WiFiSocketServerRTOS"

    for target in TARGETS:
        build_dir = project / target["dir"]
        image = build_dir / target["image"]
        map_file = build_dir / MAP_NAME
        if not image.is_file() or not map_file.is_file():
            print(f"  SKIP    {target['dir']} (not built)", file=sys.stderr)
            continue

        sdk = idf_path if target["sdk"] == "idf" else esp8266_path
        if not (sdk / ".git").exists():
            sys.exit(f"{sdk} is not a git checkout, cannot determine SDK version")

        archives = linked_archives(map_file)
        toolchain = toolchain_info(archives)
        ref = f"{target['image']}@{version}"
        firmware.append({
            "bom-ref": ref,
            "type": "firmware",
            "name": target["image"],
            "version": version,
            "description": f"WiFi module firmware for {target['chip']}",
            "supplier": {"name": "Duet3D"},
            "hashes": [{"alg": "SHA-256", "content": sha256(image)}],
            "properties": [
                {"name": "duet3d:chip", "value": target["chip"]},
                {"name": "duet3d:toolchain", "value": f"{toolchain.get('triple', '')} {toolchain.get('release', '')}".strip()},
            ],
        })

        used = []
        sdk_entry = sdk_component(sdk, target["sdk"])
        components.setdefault(sdk_entry["bom-ref"], sdk_entry)
        used.append(sdk_entry["bom-ref"])

        classified = classify(archives)
        for key in dict.fromkeys(classified.values()):
            entry = library(key, sdk, target["sdk"], classified, toolchain)
            if not entry:
                continue
            entry = {name: value for name, value in entry.items() if value is not None}
            entry.setdefault("type", "library")
            entry["bom-ref"] = f"{entry['name']}@{entry.get('version', 'unversioned')}"
            components.setdefault(entry["bom-ref"], entry)
            used.append(entry["bom-ref"])

        for entry in VENDORED:
            vendored = {k: v for k, v in entry.items() if k != "key"}
            vendored["type"] = "library"
            vendored["bom-ref"] = entry["key"]
            components.setdefault(entry["key"], vendored)
            used.append(entry["key"])

        dependencies.append({"ref": ref, "dependsOn": sorted(set(used))})

    if not firmware:
        sys.exit("no firmware images found, build the targets first")

    # The root is identified by its purl so the Duet3D BOMs that reference this repo as a source
    # component deduplicate against it on a merge
    commit = git_commit(project)
    purl = f"pkg:github/{repository.split('github.com/')[-1]}@{commit}" if commit and "github.com/" in repository else None
    root = purl or f"WiFiSocketServerRTOS@{version}"
    dependencies.insert(0, {"ref": root, "dependsOn": [entry["bom-ref"] for entry in firmware]})
    for ref in components:
        dependencies.append({"ref": ref, "dependsOn": []})

    seed = root + "".join(sorted(entry["hashes"][0]["content"] for entry in firmware))
    sbom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, seed)}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.fromtimestamp(int(os.environ.get("SOURCE_DATE_EPOCH", datetime.now(timezone.utc).timestamp())), timezone.utc).isoformat().replace("+00:00", "Z"),
            "tools": {"components": [{"type": "application", "name": "generate-sbom.py", "manufacturer": {"name": "Duet3D"}}]},
            "component": {k: v for k, v in {
                "bom-ref": root,
                "type": "application",
                "name": "WiFiSocketServerRTOS",
                "version": version,
                "description": "Firmware for the Espressif Wi-Fi modules on Duet boards",
                "supplier": {"name": "Duet3D", "url": ["https://www.duet3d.com"]},
                "licenses": [{"license": {"id": "GPL-3.0-only"}}],
                "purl": purl,
                "externalReferences": [
                    {"type": "vcs", "url": f"{repository}.git"},
                    {"type": "website", "url": repository},
                ],
            }.items() if v is not None},
        },
        "components": firmware + sorted(components.values(), key=lambda entry: entry["name"]),
        "dependencies": dependencies,
    }

    Path(output).write_text(json.dumps(sbom, indent=2) + "\n")
    print(f"  SBOM    {output} ({len(firmware)} images, {len(components)} components)")


def main():
    project = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Generate a CycloneDX SBOM for the built firmware images")
    parser.add_argument("--esp-idf", default=os.environ.get("ESP_IDF", project.parent / "esp-idf"), help="path to the ESP-IDF checkout")
    parser.add_argument("--esp8266-sdk", default=os.environ.get("ESP8266_SDK", project.parent / "ESP8266_RTOS_SDK"), help="path to the ESP8266 RTOS SDK checkout")
    parser.add_argument("--output", help="output file (default: WiFiSocketServerRTOS-<version>-sbom.json in the project root)")
    args = parser.parse_args()

    output = args.output or project / f"WiFiSocketServerRTOS-{project_version(project)}-sbom.json"
    build_sbom(project, Path(args.esp_idf), Path(args.esp8266_sdk), output)


if __name__ == "__main__":
    main()

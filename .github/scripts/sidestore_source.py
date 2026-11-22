#!/usr/bin/env python3
"""Writes the SideStore/AltStore source (apps.json) for a release (R11, ios-companion.md).

Users add https://github.com/doff256/cabled/releases/latest/download/apps.json in SideStore once;
every later release then shows up there as an update.

usage: sidestore_source.py <version> <ipa path> <out path>
"""
import datetime
import json
import os
import re
import sys

REPO = os.environ.get("GITHUB_REPOSITORY", "doff256/cabled")
RAW = f"https://raw.githubusercontent.com/{REPO}/main"


def build_number() -> str:
    text = open("ios/project.yml", encoding="utf-8").read()
    match = re.search(r'CURRENT_PROJECT_VERSION:\s*"?(\d+)"?', text)
    return match.group(1) if match else "1"


def main() -> None:
    version, ipa, out = sys.argv[1], sys.argv[2], sys.argv[3]
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()
    description = (
        "The iPhone half of Cabled: turns your iPhone into a wired webcam for Windows. "
        "Plug it into the PC with a USB cable, open Cabled on both, and apps list \"Cabled Camera\". "
        "USB only: no Wi-Fi, no accounts. Needs the Cabled app on Windows."
    )
    source = {
        "name": "Cabled",
        "identifier": "dev.cabled.source",
        "subtitle": "iPhone as a wired webcam for Windows",
        "description": description,
        "iconURL": f"{RAW}/app/Cabled.App/Assets/Cabled-256.png",
        "website": f"https://github.com/{REPO}",
        "apps": [
            {
                "name": "Cabled",
                "bundleIdentifier": "dev.cabled.companion",
                "developerName": "Cabled contributors",
                "subtitle": "Wired webcam for Windows",
                "localizedDescription": description,
                "iconURL": f"{RAW}/app/Cabled.App/Assets/Cabled-256.png",
                "tintColor": "2E4A96",
                "category": "utilities",
                "appPermissions": {
                    "entitlements": [],
                    "privacy": {
                        "NSCameraUsageDescription": "Cabled sends your camera to your PC over the USB cable, "
                        "so the PC can use it as a webcam."
                    },
                },
                "versions": [
                    {
                        "version": version,
                        "buildVersion": build_number(),
                        "date": now,
                        "localizedDescription": f"Cabled {version}. Release notes: "
                        f"https://github.com/{REPO}/releases/tag/v{version}",
                        "downloadURL": f"https://github.com/{REPO}/releases/download/v{version}/Cabled.ipa",
                        "size": os.path.getsize(ipa),
                        "minOSVersion": "17.0",
                    }
                ],
            }
        ],
        "news": [],
    }
    with open(out, "w", encoding="utf-8") as f:
        json.dump(source, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()

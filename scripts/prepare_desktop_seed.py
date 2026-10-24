#!/usr/bin/env python3
"""Update a mounted Data seed's home layout and stage the PNG wallpaper."""
import argparse
import json
import plistlib
import shutil
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mount', type=Path)
    parser.add_argument('wallpaper', type=Path)
    args = parser.parse_args()
    library = args.mount / 'mobile/Library'
    path = library / 'SpringBoard/IconState.plist'
    state = plistlib.loads(path.read_bytes())
    removed = []
    for icons in state['iconLists']:
        retained = []
        for icon in icons:
            if isinstance(icon, dict) and icon.get('iconType') == 'custom':
                removed.append(icon['displayIdentifier'])
            else:
                retained.append(icon)
        icons[:] = retained
    for metadata in state.get('listMetadata', {}).values():
        for key in ('fixedLocations', 'rotatedFixedLocations'):
            locations = metadata.get(key, {})
            for identifier in removed:
                locations.pop(identifier, None)
        if 'rotatedOrder' in metadata:
            metadata['rotatedOrder'] = [identifier for identifier in metadata['rotatedOrder']
                                        if identifier not in removed]
    path.write_bytes(plistlib.dumps(state, fmt=plistlib.FMT_BINARY))
    path = library / 'Preferences/com.apple.springboard.plist'
    preferences = plistlib.loads(path.read_bytes())
    preferences['SBShouldAddDefaultWidgetsToHomeScreen'] = False
    preferences['SBShouldUpgradeEnableWidgetSuggestions'] = False
    path.write_bytes(plistlib.dumps(preferences, fmt=plistlib.FMT_BINARY))
    destination = args.mount / 'ngpu/wallpaper'
    shutil.copytree(args.wallpaper, destination, dirs_exist_ok=True)
    for file in destination.iterdir():
        file.chmod(0o644)
    destination.chmod(0o755)
    print(json.dumps({'removed_home_widgets': removed, 'wallpaper_files':
                      sorted(file.name for file in destination.iterdir()),
                      'boot_arg': 'sheng-gpu-addon=1'}, indent=2))


if __name__ == '__main__':
    main()

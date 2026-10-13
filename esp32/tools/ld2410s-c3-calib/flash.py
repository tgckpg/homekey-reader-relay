#!/usr/bin/env python3
"""Flash the included default ESP32-C3 binaries without erasing the NVS gap."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True,help='e.g. /dev/cu.usbmodem101')
    p.add_argument('--baud',type=int,default=460800)
    a=p.parse_args()
    if a.baud<=0:p.error('Baud must be positive')
    root=Path(__file__).resolve().parent
    manifest=json.loads((root/'firmware'/'manifest.json').read_text())
    if importlib.util.find_spec('esptool') is None:
        p.error("Activate your ESP-IDF environment, or install: python3 -m pip install 'esptool>=4.8,<5'")
    cmd=[sys.executable,'-m','esptool','--chip',manifest['chip'],'--port',a.port,
         '--baud',str(a.baud),'--before','default_reset','--after','hard_reset']
    if manifest.get('no_stub'):cmd.append('--no-stub')
    cmd.append('write_flash')
    for flag,value in manifest['flash_settings'].items():cmd.extend(['--'+flag,str(value)])
    for item in manifest['files']:
        path=root/'firmware'/item['name']
        if hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:
            p.error('Firmware checksum mismatch: '+item['name'])
        cmd.extend([item['offset'],str(path)])
    print('Flashing LD2410S bench firmware to '+a.port+'. This replaces the running ESP32 application.',flush=True)
    return subprocess.run(cmd,check=False).returncode

if __name__=='__main__':sys.exit(main())

#!/usr/bin/env python3
# Regenerate src/openevse_bl.h + src/openevse_pt.h (the embedded 16MB bootloader
# and partition table) from a built openevse_wifi_v1_16mb.
#
#   python3 scripts/gen_embeds.py [path/to/.pio/build/openevse_wifi_v1_16mb]
#
import os, sys

build = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    '~/oevse/openevse_esp32_firmware/.pio/build/openevse_wifi_v1_16mb')
src = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src')

def emit(binpath, hdr, name):
    data = open(binpath, 'rb').read()
    out = ['// auto-generated from %s by scripts/gen_embeds.py' % os.path.basename(binpath),
           'static const unsigned char %s[] = {' % name]
    for i in range(0, len(data), 12):
        out.append('  ' + ''.join('0x%02x,' % b for b in data[i:i+12]))
    out += ['};', 'static const unsigned int %s_len = %d;' % (name, len(data))]
    open(os.path.join(src, hdr), 'w').write('\n'.join(out) + '\n')
    print('wrote src/%s (%d bytes)' % (hdr, len(data)))

emit(os.path.join(build, 'bootloader.bin'), 'openevse_bl.h', 'openevse_bl')
emit(os.path.join(build, 'partitions.bin'), 'openevse_pt.h', 'openevse_pt')

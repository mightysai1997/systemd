#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import re
import sys

OVERRIDES = {
    'autoneg' : 'autonegotiation',
}

xml = sys.argv[1] == '--xml'

f = open(sys.argv[-1])
for line in f:
    if line.startswith('enum ethtool_link_mode_bit_indices {'):
        break

entries = []
for line in f:
    if line.startswith('}'):
        break
    # ETHTOOL_LINK_MODE_10baseT_Half_BIT	= 0,
    m = re.match(r'^\s*(ETHTOOL_LINK_MODE_((\d*).*)_BIT)\s*=\s*(\d+),', line)
    if not m:
        continue
    enum, name, speed, value = m.groups()

    name = name.lower().replace('_', '-')
    name = OVERRIDES.get(name, name)

    duplex = name.split('-')[-1].lower()
    if duplex not in {'half', 'full'}:
        duplex = ''

    entries += [(enum, name, speed, value, duplex)]

if xml:
    print('              <tbody>')

    entries.sort(key=lambda entry: (int(entry[2]) if entry[2] else 1e20, entry[4], entry[1], entry[3]))

for enum, name, speed, value, duplex in entries:
    if xml:
        print(f'''\
                <row><entry><option>{name}</option></entry>
                <entry>{speed}</entry><entry>{duplex}</entry></row>
        ''')
    else:
        enum = f'[{enum}]'
        print(f'        {enum:50} = "{name}",')

if xml:
    print('              </tbody>')

assert len(entries) >= 99

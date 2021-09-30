#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import jinja2
import re
import sys

def parse_config_h(filename):
    # Parse config.h file generated by meson.
    ans = {}
    for line in open(filename):
        m = re.match(r'#define\s+(\w+)\s+(.*)', line)
        if not m:
            continue
        a, b = m.groups()
        if b and b[0] in '0123456789"':
            b = eval(b)
        ans[a] = b
    return ans

def render(filename, defines):
    text = open(filename).read()
    template = jinja2.Template(text, trim_blocks=True, undefined=jinja2.StrictUndefined)
    return template.render(defines)

if __name__ == '__main__':
    defines = parse_config_h(sys.argv[1])
    print(render(sys.argv[2], defines))

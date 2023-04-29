#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import sys, re

print('#include <stdio.h>')
for header in sys.argv[3:]:
    print('#include "{}"'.format(header.split('/')[-1]))

print('#include "{}"'.format(sys.argv[2]))

print('''
/* We want to check deprecated symbols too, without complaining */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

const struct {
        const char *name;
        const void *symbol;
} symbols[] = {''')

count = 0
for line in open(sys.argv[1]):
    match = re.search('^ +([a-zA-Z0-9_]+);', line)
    if match:
        s = match.group(1)
        if s == 'sd_bus_object_vtable_format':
            print(f'    {{"{s}", &{s}}},')
        else:
            print(f'    {{"{s}", {s}}},')
        count += 1

print(f'''}};

int main(void) {{
    puts("from symbol file");
    for (size_t i = 0; i < {count}; i++)
         printf("%p: %s\\n", symbols[i].symbol, symbols[i].name);
    puts("");
    puts("from source files");
    for (size_t i = 0; symbols_from_source[i].name; i++)
         printf("%p: %s\\n", symbols_from_source[i].symbol, symbols_from_source[i].name);
    return 0;
}}''')

#!/usr/bin/env python3
"""Generate axion build properties .prop file from make variable values.

Usage: gen_axion_props.py <output_path> [key=value ...]
"""

import sys
import os


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <output_path> <key=value> ...", file=sys.stderr)
        sys.exit(1)

    output_path = sys.argv[1]

    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    with open(output_path, 'w') as f:
        for pair in sys.argv[2:]:
            if '=' in pair:
                key, value = pair.split('=', 1)
                f.write(f"{key}={value}\n")

    sys.exit(0)


if __name__ == '__main__':
    main()

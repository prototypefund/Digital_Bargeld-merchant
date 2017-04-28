#!/usr/bin/env python3

"""
Stand-alone script to manage the merchant's MITM
error generator.
"""

import argparse
import sys

parser = argparse.ArgumentParser()

parser.add_argument('--exchange',
                    '-e',
                    help="Exchange URL",
                    metavar="URL",
                    type=str,
                    dest="exchange_url",
                    default=None)


parser.add_argument("--port",
                    "-p",
                    help="Port where the MITM listens",
                    dest="port",
                    type=int,
                    default=5000,
                    metavar="PORT")

args = parser.parse_args()

if getattr(args, 'exchange_url', None) is None:
    parser.print_help()
    sys.exit(1)

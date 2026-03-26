#!/bin/bash
if [ -z "$1" ]; then
    echo "Usage: $0 <GB>"
    exit 1
fi
sed -i "s/#define TOTAL_GB  [0-9]*/#define TOTAL_GB  $1/" $(dirname "$0")/*.c
echo "TOTAL_GB set to $1 in all .c files"

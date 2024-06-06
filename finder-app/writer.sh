#!/bin/sh

writefile="$1"
writestr="$2"

if [ $# -lt 2 ]; then
  echo "Usage: $0 <file> <string>"
  exit 1
else
  writefile="$1"
  writestr="$2"
fi

mkdir -p "$(dirname "$writefile")"
echo "$writestr" >"$writefile"

if [ $? -eq 0 ]; then
  exit 0
else
  echo "failure"
  exit 1
fi

#!/bin/sh

writefile="$1"
writestr="$2"

if [ $# -lt 2 ]; then
  echo "Using default value ${writestr} for string to write"
  if [ $# -lt 1 ]; then
    echo "Using default value ${writefile} for file to write"
  else
    writefile="$1"
  fi
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

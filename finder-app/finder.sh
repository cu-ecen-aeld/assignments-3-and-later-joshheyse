#!/bin/sh

filesdir="$1"
searchstr="$2"

if [ $# -lt 2 ]; then
  echo "Usage: $0 <filesdir> <searchstr>"
  exit 1
fi

if [ ! -d "$filesdir" ]; then
  echo "Directory $filesdir does not exist"
  exit 1
fi

if [ "$searchstr" = "" ]; then
  echo "Search string is empty"
  exit 1
fi

echo "The number of files are $(ls -1 "$filesdir" | wc -l) and the number of matching lines are $(grep -r "$searchstr" "$filesdir" | wc -l)"

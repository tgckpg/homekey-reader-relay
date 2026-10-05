#!/bin/sh
set -eu
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for name in pn532 relay; do
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iinclude "${name}_test.c" -o "$build_dir/$name"
	"$build_dir/$name"
done

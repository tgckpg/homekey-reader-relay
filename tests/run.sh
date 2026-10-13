#!/bin/sh
set -eu
cd "$(dirname "$0")"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for name in pn532 relay presence; do
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iinclude "${name}_test.c" -o "$build_dir/$name"
	"$build_dir/$name"
done
for config in '-DESP_IDF_VERSION=0x050401' '-DTEST_PRESENCE_DISABLED'; do
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Iinclude "$config" presence_test.c -o "$build_dir/presence-config"
	"$build_dir/presence-config"
done

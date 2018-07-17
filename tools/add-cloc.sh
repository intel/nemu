#!/bin/bash

if [ $(git show --stat  | grep -c '\.[ch] ') -gt 0 ]; then
git show --no-patch --format="%B" | grep -v "Signed-off" > /tmp/1;
tools/cloc-change.sh >/tmp/2;
echo >>/tmp/2;
git show --no-patch --format="%B" | grep "Signed-off">/tmp/3;
cat /tmp/1 /tmp/2 /tmp/3 | git commit --amend --file "-"
fi

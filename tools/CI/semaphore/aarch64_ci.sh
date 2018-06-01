#!/bin/bash
set -e

if [[ "$SEMAPHORE_BRANCH_ID" == "" ]]; then
  SEMAPHORE_BRANCH_ID="test"
fi

if [[ "$SEMAPHORE_BUILD_NUMBER" == "" ]]; then
  SEMAPHORE_BUILD_NUMBER="123"
fi

if [[ "$BUILD_HOST" == "" ]]; then
  echo "\$BUILD_HOST environment variable required"
  exit
fi

if [[ "$SSH_KEY" == "" ]]; then
  echo "\$SSH_KEY environment variable required"
  exit
fi

if [[ "$SSH_USER" == "" ]]; then
  echo "\$SSH_USER environment variable required"
  exit
fi

BUILD="$SEMAPHORE_BRANCH_ID-$SEMAPHORE_BUILD_NUMBER"
SRC_DIR="ci/$BUILD/src"
BUILD_DIR="ci/$BUILD/build"

export GIT_SSH_COMMAND="ssh -i $SSH_KEY -F /dev/null -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o IdentitiesOnly=yes -q"

chmod og-rwX $SSH_KEY

$GIT_SSH_COMMAND $SSH_USER@$BUILD_HOST git init $SRC_DIR
git remote add ci $SSH_USER@$BUILD_HOST:$SRC_DIR
git push ci HEAD:ci
$GIT_SSH_COMMAND $SSH_USER@$BUILD_HOST "cd $SRC_DIR && git checkout ci && BUILD_DIR=\$HOME/$BUILD_DIR SRCDIR=\$PWD tools/build_aarch64.sh"
$GIT_SSH_COMMAND $SSH_USER@$BUILD_HOST "sudo -E $SRC_DIR/tools/CI/minimal_ci.sh -hypervisor \$HOME/$BUILD_DIR/aarch64-softmmu/qemu-system-aarch64 -builddir \$HOME/$BUILD_DIR -vsock false"
$GIT_SSH_COMMAND $SSH_USER@$BUILD_HOST "rm -rf ci/$BUILD"

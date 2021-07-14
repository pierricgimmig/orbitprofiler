#!/bin/bash
#
# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Fail on any error.
set -euo pipefail

readonly REPO_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../../../" >/dev/null 2>&1 && pwd )"
readonly SCRIPT="/mnt/kokoro/checks/clang_format/check.sh"

if [ "$0" == "$SCRIPT" ]; then
  # We are inside the docker container

  echo -e "\n\nThe following files don't match our code-formatting standard:"
  echo -e "> This list includes all files in the repo that need formatting,"
  echo -e "> but changes outside of the scope of your PR won't affect the outcome"
  echo -e "> of this presubmit check.\n"
  while read line; do
    if clang-format --output-replacements-xml $line | grep '<replacement ' > /dev/null; then
      echo $line
    fi
  done <<< $(find /mnt -name '*.cpp' -o -name '*.h' \
        | grep -v "third_party/" \
        | grep -v "/build" )
  echo -e "--\n"

  cd /mnt
  # Use origin/main as reference branch, if not specified by kokoro
  REFERENCE="origin/${KOKORO_GITHUB_PULL_REQUEST_TARGET_BRANCH:-main}"
  MERGE_BASE="$(git merge-base $REFERENCE HEAD)" # Merge base is the commit on main this PR was branched from.
  FORMATTING_DIFF="$(git diff -U0 --no-color --relative --diff-filter=r $MERGE_BASE | clang-format-diff-9 -p1)"

  if [ -n "$FORMATTING_DIFF" ]; then
    echo "clang-format determined the following necessary changes to your PR:"
    echo "$FORMATTING_DIFF"
    echo -e "\n\nHere is the list of files that need formatting:"
    echo "$(echo "$FORMATTING_DIFF" | egrep '^\+\+\+' | cut -d' ' -f2 | cut -f1 )"
    echo -e "--\n\nNote: We recommend you to use git clang-format to format your changes!"
    exit 1
  else
    echo "All your changes fulfill our code formatting requirements!"
    exit 0
  fi

else
  source "$(cd "$( dirname "${BASH_SOURCE[0]}" )/../../../" >/dev/null 2>&1 && pwd)/third_party/conan/docker/utils.sh"

  gcloud auth configure-docker --quiet
  docker run --rm --network host -v ${REPO_ROOT}:/mnt -e KOKORO_GITHUB_PULL_REQUEST_TARGET_BRANCH \
    `find_container_for_conan_profile clang_format` $SCRIPT
fi


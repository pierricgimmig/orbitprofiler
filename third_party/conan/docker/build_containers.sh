#!/bin/bash
# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -euo pipefail

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

declare -A profile_to_dockerfile=( \
  ["clang7_debug"]="clang7_opengl_qt" \
  ["clang7_release"]="clang7_opengl_qt" \
  ["clang7_relwithdebinfo"]="clang7_opengl_qt" \
  ["clang8_debug"]="clang8_opengl_qt" \
  ["clang8_release"]="clang8_opengl_qt" \
  ["clang8_relwithdebinfo"]="clang8_opengl_qt" \
  ["clang9_debug"]="clang9_opengl_qt" \
  ["clang9_release"]="clang9_opengl_qt" \
  ["clang9_relwithdebinfo"]="clang9_opengl_qt" \
  ["ggp_debug"]="ggp" \
  ["ggp_release"]="ggp" \
  ["ggp_relwithdebinfo"]="ggp" \
  ["gcc9_debug"]="gcc9" \
  ["gcc9_release"]="gcc9" \
  ["gcc9_relwithdebinfo"]="gcc9" \
  ["gcc10_debug"]="gcc10" \
  ["gcc10_release"]="gcc10" \
  ["gcc10_relwithdebinfo"]="gcc10" \
  ["msvc2019_release"]="msvc2019" \
  ["msvc2019_relwithdebinfo"]="msvc2019" \
  ["msvc2019_debug"]="msvc2019" \
  ["msvc2019_relwithdebinfo_x86"]="msvc2019" \
  ["clang_format"]="clang_format" \
  ["license_headers"]="license_headers" \
  ["iwyu"]="iwyu" \
  ["coverage_clang9"]="coverage_clang9" \
)

if [ "$#" -eq 0 ]; then
  if [ "$(uname -s)" == "Linux" ]; then
    profiles=( {clang{7,8,9},gcc{9,10},ggp}_{release,relwithdebinfo,debug} clang_format license_headers iwyu coverage_clang9 )
  else
    profiles=( msvc2019_{release,relwithdebinfo,debug} )
  fi
else
  profiles="$@"
fi

for profile in ${profiles[@]}; do
  tag="$(date -u '+%Y%m%dT%H%M%S.%N')"
  docker build "${DIR}" -f "${DIR}/Dockerfile.${profile_to_dockerfile[$profile]}" -t gcr.io/orbitprofiler/${profile}:${tag} || exit $?
  sed -i "s/\\[${profile}\\]=\".*\"/[${profile}]=\"${tag}\"/" "${DIR}/tags.sh"
done

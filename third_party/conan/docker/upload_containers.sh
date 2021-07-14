#!/bin/bash
# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -euo pipefail

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

source "${DIR}/tags.sh"

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
  tag="${docker_image_tag_mapping[${profile}]-latest}"
  docker push gcr.io/orbitprofiler/$profile:${tag} || exit $?

  digest="$(docker inspect --format='{{index .RepoDigests 0}}' gcr.io/orbitprofiler/${profile}:${tag})"
  sed -i "s|\\[${profile}\\]=\".*\"|[${profile}]=\"${digest}\"|" "${DIR}/digests.sh"
done
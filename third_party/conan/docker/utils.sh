#!/bin/bash
# Copyright (c) 2021 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Obtain current docker image digests
source "$(cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd)/digests.sh"

function find_container_for_conan_profile {
    echo "${docker_image_digest_mapping[$1]}"
}
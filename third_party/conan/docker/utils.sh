#!/bin/bash
# Copyright (c) 2021 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Obtain current docker image tags and digests
source "$(cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd)/tags.sh"
source "$(cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd)/digests.sh"

function find_container_for_conan_profile {
    readonly CONAN_PROFILE="$1"

    # Use a specific tag from the mapping. If none is specified, fall back to `latest`.
    readonly DOCKER_IMAGE_TAG="${docker_image_tag_mapping[${CONAN_PROFILE}]-latest}"

    CONTAINER="gcr.io/orbitprofiler/${CONAN_PROFILE}:${DOCKER_IMAGE_TAG}"

    if [ ${docker_image_digest_mapping[${CONAN_PROFILE}]+abc} ]; then
        echo "Found a docker image digest. Using that to pin the container to an exact version." >&2
        CONTAINER="${docker_image_digest_mapping[${CONAN_PROFILE}]}"
    fi

    echo "${CONTAINER}"
}
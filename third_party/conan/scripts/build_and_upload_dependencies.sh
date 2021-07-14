#!/bin/bash

set -euo pipefail

function conan_profile_exists {
  conan profile show $profile >/dev/null 2>&1
  return $?
}


readonly REPO_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../../../" >/dev/null 2>&1 && pwd )"
readonly REPO_ROOT_WIN="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../../../" >/dev/null 2>&1 && pwd -W 2>/dev/null)"

source "${REPO_ROOT}/third_party/conan/docker/utils.sh"

# Path to script inside the docker container
readonly SCRIPT="/mnt/third_party/conan/scripts/build_and_upload_dependencies.sh"

export CONAN_USE_ALWAYS_SHORT_PATHS=1

if [[ -v IN_DOCKER ]]; then
  pip3 install conan==1.36.0
  export QT_QPA_PLATFORM=offscreen

  if [[ -v ORBIT_PUBLIC_BUILD ]]; then
    $REPO_ROOT/third_party/conan/configs/install.sh --force-public-remotes
    conan user -r bintray $BINTRAY_USERNAME -p $BINTRAY_API_KEY
  else
    $REPO_ROOT/third_party/conan/configs/install.sh
    conan user -r artifactory $ARTIFACTORY_USERNAME -p $ARTIFACTORY_API_KEY
  fi

  for profile in $@; do
    conan_profile_exists "$profile" || exit 128
    echo "Checking profile $profile..."

    if [ $(uname -s) == "Linux" ]; then
      platform="linux"
    else
      platform="windows"
    fi

    mkdir -p build_$profile/ || exit $?
    conan lock create "$REPO_ROOT/conanfile.py" --user=orbitdeps --channel=stable \
      --build=outdated \
      --lockfile="$REPO_ROOT/third_party/conan/lockfiles/base.lock" -pr $profile \
      --lockfile-out=build_$profile/conan.lock || exit $?

    LOCKFILE="$(pwd)/build_$profile/conan.lock"

    PACKAGES=$(conan info -l $LOCKFILE $REPO_ROOT -j 2>/dev/null \
               | grep build_id \
               | jq '.[] | select(.is_ref) | select(.binary != "Download" and .binary != "Cache" and .binary != "Skip") | .reference + ":" + .id' \
               | { grep -v 'llvm/' || true; } \
               | { grep -v 'ggp_sdk/' || true; } \
               | tr -d '"')

    if [ $(echo -n "$PACKAGES" | wc -c) -eq 0 ]; then
      echo -n "No binary packages are missing or outdated for profile $profile."
      echo " Skipping this configuration."
    else
      echo -e "The following binary packages need to be uploaded:\n$PACKAGES"

      conan install -if build_$profile/ --build=outdated -l $LOCKFILE $REPO_ROOT || exit $?
      conan build -bf build_$profile/ $REPO_ROOT || exit $?

      echo "$PACKAGES" | while read package; do
        if [[ -v ORBIT_PUBLIC_BUILD ]]; then
          conan upload -r bintray -c $package
        else
          conan upload -r artifactory -c $package
        fi
      done
    fi
  done
else
  if [ "$#" -eq 0 ]; then
    if [ $(uname -s) == "Linux" ]; then
      readonly PROFILES=( {clang{7,9},gcc9,ggp}_{release,relwithdebinfo,debug} )
    else
      readonly PROFILES=( msvc2019_{release,relwithdebinfo,debug} )
    fi
  else
    readonly PROFILES="$@"
  fi

  if [ $(uname -s) == "Linux" ]; then

    curl -s http://artifactory.internal/ >/dev/null 2>&1 || true
    if [[ ! -v ORBIT_OVERRIDE_ARTIFACTORY_URL ]] && [[ $? -ne 0 ]]; then
      echo "Note: SSH port forwarding for artifactory set up?"
      export ORBIT_OVERRIDE_ARTIFACTORY_URL="http://localhost:8080/artifactory/api/conan/conan"
    fi

    for profile in ${PROFILES[@]}; do
      IN_DOCKER=yes \
      docker run --network host --rm -it -v $REPO_ROOT:/mnt \
             -e ARTIFACTORY_USERNAME -e ARTIFACTORY_API_KEY \
             -e BINTRAY_USERNAME -e BINTRAY_API_KEY \
             -e ORBIT_OVERRIDE_ARTIFACTORY_URL \
             -e IN_DOCKER \
             -e ORBIT_PUBLIC_BUILD \
             `find_container_for_conan_profile $profile` \
             $SCRIPT $profile || exit $?
    done
  else # Windows
    curl -s http://artifactory.internal/ >/dev/null 2>&1 || true
    if [[ ! -v ORBIT_OVERRIDE_ARTIFACTORY_URL ]] && [[ $? -ne 0 ]]; then
      echo "Note: SSH port forwarding for artifactory set up?"
      PUBLIC_IP="$(ipconfig | grep -A20 "Ethernet adapter Ethernet:" | grep "IPv4 Address" | head -n1 | cut -d ':' -f 2 | tr -d ' \r\n')"
      export ORBIT_OVERRIDE_ARTIFACTORY_URL="http://${PUBLIC_IP}:8080/artifactory/api/conan/conan"
    fi

    for profile in ${PROFILES[@]}; do
      IN_DOCKER=yes \
      docker run --isolation=process --rm -v $REPO_ROOT_WIN:C:/mnt \
       --storage-opt "size=50GB" \
       -e ARTIFACTORY_USERNAME -e ARTIFACTORY_API_KEY \
       -e BINTRAY_USERNAME -e BINTRAY_API_KEY \
       -e ORBIT_OVERRIDE_ARTIFACTORY_URL \
       -e IN_DOCKER \
       -e ORBIT_PUBLIC_BUILD \
       `find_container_for_conan_profile $profile` \
       "C:/Program Files/Git/bin/bash.exe" \
       -c "/c$SCRIPT $profile" || exit $?
    done
  fi
fi

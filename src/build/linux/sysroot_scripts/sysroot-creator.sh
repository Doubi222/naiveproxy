# Copyright 2014 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# This script should not be run directly but sourced by the other
# scripts (e.g. sysroot-creator-bullseye.sh).  Its up to the parent scripts
# to define certain environment variables: e.g.
#  DISTRO=debian
#  DIST=bullseye
#  # Similar in syntax to /etc/apt/sources.list
#  APT_SOURCES_LIST=( "http://ftp.us.debian.org/debian/ bullseye main" )
#  KEYRING_FILE=debian-archive-bullseye-stable.gpg
#  DEBIAN_PACKAGES="gcc libz libssl"

#@ This script builds Debian/Ubuntu sysroot images for building Google Chrome.
#@
#@  Generally this script is invoked as:
#@  sysroot-creator-<flavour>.sh <mode> <args>*
#@  Available modes are shown below.
#@
#@ List of modes:

######################################################################
# Config
######################################################################

set -o nounset
set -o errexit

SCRIPT_DIR=$(cd $(dirname $0) && pwd)

if [ -z "${DIST:-}" ]; then
  echo "error: DIST not defined"
  exit 1
fi

if [ -z "${KEYRING_FILE:-}" ]; then
  echo "error: KEYRING_FILE not defined"
  exit 1
fi

if [ -z "${DEBIAN_PACKAGES:-}" ]; then
  echo "error: DEBIAN_PACKAGES not defined"
  exit 1
fi

readonly HAS_ARCH_AMD64=${HAS_ARCH_AMD64:=0}
readonly HAS_ARCH_I386=${HAS_ARCH_I386:=0}
readonly HAS_ARCH_ARM=${HAS_ARCH_ARM:=0}
readonly HAS_ARCH_ARM64=${HAS_ARCH_ARM64:=0}
readonly HAS_ARCH_ARMEL=${HAS_ARCH_ARMEL:=0}
readonly HAS_ARCH_MIPS=${HAS_ARCH_MIPS:=0}
readonly HAS_ARCH_MIPS64EL=${HAS_ARCH_MIPS64EL:=0}

readonly REQUIRED_TOOLS="curl xzcat"

######################################################################
# Package Config
######################################################################

readonly PACKAGES_EXT=xz
readonly RELEASE_FILE="Release"
readonly RELEASE_FILE_GPG="Release.gpg"

readonly DEBIAN_DEP_LIST_AMD64="generated_package_lists/${DIST}.amd64"
readonly DEBIAN_DEP_LIST_I386="generated_package_lists/${DIST}.i386"
readonly DEBIAN_DEP_LIST_ARM="generated_package_lists/${DIST}.arm"
readonly DEBIAN_DEP_LIST_ARM64="generated_package_lists/${DIST}.arm64"
readonly DEBIAN_DEP_LIST_ARMEL="generated_package_lists/${DIST}.armel"
readonly DEBIAN_DEP_LIST_MIPS="generated_package_lists/${DIST}.mipsel"
readonly DEBIAN_DEP_LIST_MIPS64EL="generated_package_lists/${DIST}.mips64el"


######################################################################
# Helper
######################################################################

Banner() {
  echo "######################################################################"
  echo $*
  echo "######################################################################"
}


SubBanner() {
  echo "----------------------------------------------------------------------"
  echo $*
  echo "----------------------------------------------------------------------"
}


Usage() {
  egrep "^#@" "${BASH_SOURCE[0]}" | cut --bytes=3-
}


DownloadOrCopyNonUniqueFilename() {
  # Use this function instead of DownloadOrCopy when the url uniquely
  # identifies the file, but the filename (excluding the directory)
  # does not.
  local url="$1"
  local dest="$2"

  local hash="$(echo "$url" | sha256sum | cut -d' ' -f1)"

  DownloadOrCopy "${url}" "${dest}.${hash}"
  # cp the file to prevent having to redownload it, but mv it to the
  # final location so that it's atomic.
  cp "${dest}.${hash}" "${dest}.$$"
  mv "${dest}.$$" "${dest}"
}

DownloadOrCopy() {
  if [ -f "$2" ] ; then
    echo "$2 already in place"
    return
  fi

  HTTP=0
  echo "$1" | grep -Eqs '^https?://' && HTTP=1
  if [ "$HTTP" = "1" ]; then
    SubBanner "downloading from $1 -> $2"
    # Appending the "$$" shell pid is necessary here to prevent concurrent
    # instances of sysroot-creator.sh from trying to write to the same file.
    local temp_file="${2}.partial.$$"
    # curl --retry doesn't retry when the page gives a 4XX error, so we need to
    # manually rerun.
    for i in {1..10}; do
      # --create-dirs is added in case there are slashes in the filename, as can
      # happen with the "debian/security" release class.
      local http_code=$(curl -L "$1" --create-dirs -o "${temp_file}" \
                        -w "%{http_code}")
      if [ ${http_code} -eq 200 ]; then
        break
      fi
      echo "Bad HTTP code ${http_code} when downloading $1"
      rm -f "${temp_file}"
      sleep $i
    done
    if [ ! -f "${temp_file}" ]; then
      exit 1
    fi
    mv "${temp_file}" $2
  else
    SubBanner "copying from $1"
    cp "$1" "$2"
  fi
}


SetEnvironmentVariables() {
  case $1 in
    *Amd64)
      ARCH=AMD64
      ;;
    *I386)
      ARCH=I386
      ;;
    *Mips64el)
      ARCH=MIPS64EL
      ;;
    *Mips)
      ARCH=MIPS
      ;;
    *ARM)
      ARCH=ARM
      ;;
    *ARM64)
      ARCH=ARM64
      ;;
    *ARMEL)
      ARCH=ARMEL
      ;;
    *)
      echo "ERROR: Unable to determine architecture based on: $1"
      exit 1
      ;;
  esac
  ARCH_LOWER=$(echo $ARCH | tr '[:upper:]' '[:lower:]')
}


# some sanity checks to make sure this script is run from the right place
# with the right tools
SanityCheck() {
  Banner "Sanity Checks"

  local chrome_dir=$(cd "${SCRIPT_DIR}/../../.." && pwd)
  BUILD_DIR="${chrome_dir}/out/sysroot-build/${DIST}"
  mkdir -p ${BUILD_DIR}
  echo "Using build directory: ${BUILD_DIR}"

  for tool in ${REQUIRED_TOOLS} ; do
    if ! which ${tool} > /dev/null ; then
      echo "Required binary $tool not found."
      echo "Exiting."
      exit 1
    fi
  done

  # This is where the staging sysroot is.
  INSTALL_ROOT="${BUILD_DIR}/${DIST}_${ARCH_LOWER}_staging"
  TARBALL="${BUILD_DIR}/${DISTRO}_${DIST}_${ARCH_LOWER}_sysroot.tar.xz"

  if ! mkdir -p "${INSTALL_ROOT}" ; then
    echo "ERROR: ${INSTALL_ROOT} can't be created."
    exit 1
  fi
}


ChangeDirectory() {
  # Change directory to where this script is.
  cd ${SCRIPT_DIR}
}


ClearInstallDir() {
  Banner "Clearing dirs in ${INSTALL_ROOT}"
  rm -rf ${INSTALL_ROOT}/*
}


CreateTarBall() {
  Banner "Creating tarball ${TARBALL}"
  tar -I "xz -9 -T0" -cf ${TARBALL} -C ${INSTALL_ROOT} .
}

ExtractPackageXz() {
  local src_file="$1"
  local dst_file="$2"
  local repo="$3"
  xzcat "${src_file}" | egrep '^(Package:|Filename:|SHA256:) ' |
    sed "s|Filename: |Filename: ${repo}|" > "${dst_file}"
}

GeneratePackageListDistRepo() {
  local arch="$1"
  local repo="$2"
  local dist="$3"
  local repo_name="$4"

  local tmp_package_list="${BUILD_DIR}/Packages.${dist}_${repo_name}_${arch}"
  local repo_basedir="${repo}/dists/${dist}"
  local package_list="${BUILD_DIR}/Packages.${dist}_${repo_name}_${arch}.${PACKAGES_EXT}"
  local package_file_arch="${repo_name}/binary-${arch}/Packages.${PACKAGES_EXT}"
  local package_list_arch="${repo_basedir}/${package_file_arch}"

  DownloadOrCopyNonUniqueFilename "${package_list_arch}" "${package_list}"
  VerifyPackageListing "${package_file_arch}" "${package_list}" ${repo} ${dist}
  ExtractPackageXz "${package_list}" "${tmp_package_list}" ${repo}
  cat "${tmp_package_list}" | ./merge-package-lists.py "${list_base}"
}

GeneratePackageListDist() {
  local arch="$1"
  set -- $2
  local repo="$1"
  local dist="$2"
  shift 2
  while (( "$#" )); do
      GeneratePackageListDistRepo "$arch" "$repo" "$dist" "$1"
      shift
  done
}

GeneratePackageListCommon() {
  local output_file="$1"
  local arch="$2"
  local packages="$3"

  local list_base="${BUILD_DIR}/Packages.${DIST}_${arch}"
  > "${list_base}"  # Create (or truncate) a zero-length file.
  printf '%s\n' "${APT_SOURCES_LIST[@]}" | while read source; do
    GeneratePackageListDist "${arch}" "${source}"
  done

  GeneratePackageList "${list_base}" "${output_file}" "${packages}"
}

GeneratePackageListAmd64() {
  GeneratePackageListCommon "$1" amd64 "${DEBIAN_PACKAGES}
    ${DEBIAN_PACKAGES_X86:=} ${DEBIAN_PACKAGES_AMD64:=}"
}

GeneratePackageListI386() {
  GeneratePackageListCommon "$1" i386 "${DEBIAN_PACKAGES}
    ${DEBIAN_PACKAGES_X86:=}"
}

GeneratePackageListARM() {
  GeneratePackageListCommon "$1" armhf "${DEBIAN_PACKAGES}
    ${DEBIAN_PACKAGES_ARM:=}"
}

GeneratePackageListARM64() {
  GeneratePackageListCommon "$1" arm64 "${DEBIAN_PACKAGES}
    ${DEBIAN_PACKAGES_ARM64:=}"
}

GeneratePackageListARMEL() {
  GeneratePackageListCommon "$1" armel "${DEBIAN_PACKAGES}
    ${DEBIAN_PACKAGES_ARMEL:=}"
}

GeneratePackageListMips() {
  GeneratePackageListCommon "$1" mipsel "${DEBIAN_PACKAGES}"
}

GeneratePackageListMips64el() {
  GeneratePackageListCommon "$1" mips64el "${DEBIAN_PACKAGES}
  ${DEBIAN_PACKAGES_MIPS64EL:=}"
}

StripChecksumsFromPackageList() {
  local package_file="$1"
  sed -i 's/ [a-f0-9]\{64\}$//' "$package_file"
}

######################################################################
#
######################################################################

HacksAndPatchesCommon() {
  local arch=$1
  local os=$2
  local strip=$3
  Banner "Misc Hacks & Patches"

  # Remove an unnecessary dependency on qtchooser.
  rm "${INSTALL_ROOT}/usr/lib/${arch}-${os}/qt-default/qtchooser/default.conf"

  # Unversion libdbus and libxkbcommon symbols.  This is required because
  # libdbus-1-3 and libxkbcommon0 switched from unversioned symbols to versioned
  # ones, and we must still support distros using the unversioned library.  This
  # hack can be removed once support for Ubuntu Trusty and Debian Jessie are
  # dropped.
  ${strip} -R .gnu.version_d -R .gnu.version \
    "${INSTALL_ROOT}/lib/${arch}-${os}/libdbus-1.so.3"
  cp "${SCRIPT_DIR}/libdbus-1-3-symbols" \
    "${INSTALL_ROOT}/debian/libdbus-1-3/DEBIAN/symbols"

  ${strip} -R .gnu.version_d -R .gnu.version \
    "${INSTALL_ROOT}/usr/lib/${arch}-${os}/libxkbcommon.so.0.0.0"
  cp "${SCRIPT_DIR}/libxkbcommon0-symbols" \
    "${INSTALL_ROOT}/debian/libxkbcommon0/DEBIAN/symbols"

  # libxcomposite1 is missing a symbols file.
  cp "${SCRIPT_DIR}/libxcomposite1-symbols" \
    "${INSTALL_ROOT}/debian/libxcomposite1/DEBIAN/symbols"

  # Shared objects depending on libdbus-1.so.3 have unsatisfied undefined
  # versioned symbols. To avoid LLD --no-allow-shlib-undefined errors, rewrite
  # DT_NEEDED entries from libdbus-1.so.3 to a different string. LLD will
  # suppress --no-allow-shlib-undefined diagnostics for such shared objects.
  set +e
  for f in "${INSTALL_ROOT}/lib/${arch}-${os}"/*.so \
           "${INSTALL_ROOT}/usr/lib/${arch}-${os}"/*.so; do
    echo "$f" | grep -q 'libdbus-1.so$' && continue
    # In a dependent shared object, the only occurrence of "libdbus-1.so.3" is
    # the string referenced by the DT_NEEDED entry.
    offset=$(LANG=C grep -abo libdbus-1.so.3 "$f")
    [ -n "$offset" ] || continue
    echo -n 'libdbus-1.so.0' | dd of="$f" conv=notrunc bs=1 \
      seek="$(echo -n "$offset" | cut -d : -f 1)" status=none
  done
  set -e

  # fcntl64() was introduced in glibc 2.28.  Make sure to use fcntl() instead.
  local fcntl_h="${INSTALL_ROOT}/usr/include/fcntl.h"
  sed -i '{N; s/#ifndef __USE_FILE_OFFSET64\(\nextern int fcntl\)/#if 1\1/}' \
      "${fcntl_h}"

  # __GLIBC_MINOR__ is used as a feature test macro.  Replace it with the
  # earliest supported version of glibc (2.17, https://crbug.com/376567).
  local usr_include="${INSTALL_ROOT}/usr/include"
  local features_h="${usr_include}/features.h"
  sed -i 's|\(#define\s\+__GLIBC_MINOR__\)|\1 17 //|' "${features_h}"
  # Do not use pthread_cond_clockwait as it was introduced in glibc 2.30.
  local cppconfig_h="${usr_include}/${arch}-${os}/c++/10/bits/c++config.h"
  sed -i 's|\(#define\s\+_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT\)|// \1|' \
    "${cppconfig_h}"

  # This is for chrome's ./build/linux/pkg-config-wrapper
  # which overwrites PKG_CONFIG_LIBDIR internally
  SubBanner "Move pkgconfig scripts"
  mkdir -p ${INSTALL_ROOT}/usr/lib/pkgconfig
  mv ${INSTALL_ROOT}/usr/lib/${arch}-${os}/pkgconfig/* \
      ${INSTALL_ROOT}/usr/lib/pkgconfig
}


ReversionGlibc() {
  local arch=$1
  local os=$2

  # Avoid requiring unsupported glibc versions.
  "${SCRIPT_DIR}/reversion_glibc.py" \
    "${INSTALL_ROOT}/lib/${arch}-${os}/libc.so.6"
  "${SCRIPT_DIR}/reversion_glibc.py" \
    "${INSTALL_ROOT}/lib/${arch}-${os}/libm.so.6"
  "${SCRIPT_DIR}/reversion_glibc.py" \
    "${INSTALL_ROOT}/lib/${arch}-${os}/libcrypt.so.1"
}


HacksAndPatchesAmd64() {
  HacksAndPatchesCommon x86_64 linux-gnu strip
  ReversionGlibc x86_64 linux-gnu
}


HacksAndPatchesI386() {
  HacksAndPatchesCommon i386 linux-gnu strip
  ReversionGlibc i386 linux-gnu
}


HacksAndPatchesARM() {
  HacksAndPatchesCommon arm linux-gnueabihf arm-linux-gnueabihf-strip
  ReversionGlibc arm linux-gnueabihf
}

HacksAndPatchesARM64() {
  # Use the unstripped libdbus for arm64 to prevent linker errors.
  # https://bugs.chromium.org/p/webrtc/issues/detail?id=8535
  HacksAndPatchesCommon aarch64 linux-gnu true
  # Skip reversion_glibc.py. Glibc is compiled in a way where many libm math
  # functions do not have compatibility symbols for versions <= 2.17.
  ReversionGlibc aarch64 linux-gnu
}

HacksAndPatchesARMEL() {
  HacksAndPatchesCommon arm linux-gnueabi arm-linux-gnueabi-strip
  ReversionGlibc arm linux-gnueabi
}

HacksAndPatchesMips() {
  HacksAndPatchesCommon mipsel linux-gnu mipsel-linux-gnu-strip
  ReversionGlibc mipsel linux-gnu
}


HacksAndPatchesMips64el() {
  HacksAndPatchesCommon mips64el linux-gnuabi64 mips64el-linux-gnuabi64-strip
  ReversionGlibc mips64el linux-gnuabi64
}


InstallIntoSysroot() {
  Banner "Install Libs And Headers Into Jail"

  mkdir -p ${BUILD_DIR}/debian-packages
  # The /debian directory is an implementation detail that's used to cd into
  # when running dpkg-shlibdeps.
  mkdir -p ${INSTALL_ROOT}/debian
  # An empty control file is necessary to run dpkg-shlibdeps.
  touch ${INSTALL_ROOT}/debian/control
  while (( "$#" )); do
    local file="$1"
    local package="${BUILD_DIR}/debian-packages/${file##*/}"
    shift
    local sha256sum="$1"
    shift
    if [ "${#sha256sum}" -ne "64" ]; then
      echo "Bad sha256sum from package list"
      exit 1
    fi

    Banner "Installing $(basename ${file})"
    DownloadOrCopy ${file} ${package}
    if [ ! -s "${package}" ] ; then
      echo
      echo "ERROR: bad package ${package}"
      exit 1
    fi
    echo "${sha256sum}  ${package}" | sha256sum --quiet -c

    SubBanner "Extracting to ${INSTALL_ROOT}"
    dpkg-deb -x ${package} ${INSTALL_ROOT}

    base_package=$(dpkg-deb --field ${package} Package)
    mkdir -p ${INSTALL_ROOT}/debian/${base_package}/DEBIAN
    dpkg-deb -e ${package} ${INSTALL_ROOT}/debian/${base_package}/DEBIAN
  done

  # Prune /usr/share, leaving only pkgconfig.
  ls -d ${INSTALL_ROOT}/usr/share/* | grep -v "/pkgconfig$" | xargs rm -r
}


CleanupJailSymlinks() {
  Banner "Jail symlink cleanup"

  SAVEDPWD=$(pwd)
  cd ${INSTALL_ROOT}
  local libdirs="lib usr/lib"
  if [ "${ARCH}" != "MIPS" ]; then
    libdirs="${libdirs} lib64"
  fi

  find $libdirs -type l -printf '%p %l\n' | while read link target; do
    # skip links with non-absolute paths
    echo "${target}" | grep -qs ^/ || continue
    echo "${link}: ${target}"
    # Relativize the symlink.
    prefix=$(echo "${link}" | sed -e 's/[^/]//g' | sed -e 's|/|../|g')
    ln -snfv "${prefix}${target}" "${link}"
  done

  failed=0
  while read link target; do
    # Make sure we catch new bad links.
    if [ ! -r "${link}" ]; then
      echo "ERROR: FOUND BAD LINK ${link}"
      ls -l ${link}
      failed=1
    fi
  done < <(find $libdirs -type l -printf '%p %l\n')
  if [ $failed -eq 1 ]; then
      exit 1
  fi
  cd "$SAVEDPWD"
}


VerifyLibraryDepsCommon() {
  local arch=$1
  local os=$2
  local find_dirs=(
    "${INSTALL_ROOT}/lib/"
    "${INSTALL_ROOT}/lib/${arch}-${os}/"
    "${INSTALL_ROOT}/usr/lib/${arch}-${os}/"
  )
  local needed_libs="$(
    find ${find_dirs[*]} -name "*\.so*" -type f -exec file {} \; | \
      grep ': ELF' | sed 's/^\(.*\): .*$/\1/' | xargs readelf -d | \
      grep NEEDED | sort | uniq | sed 's/^.*Shared library: \[\(.*\)\]$/\1/g')"
  local all_libs="$(find ${find_dirs[*]} -printf '%f\n')"
  # Ignore missing libdbus-1.so.0
  all_libs+="$(echo -e '\nlibdbus-1.so.0')"
  local missing_libs="$(grep -vFxf <(echo "${all_libs}") \
    <(echo "${needed_libs}"))"
  if [ ! -z "${missing_libs}" ]; then
    echo "Missing libraries:"
    echo "${missing_libs}"
    exit 1
  fi
}


VerifyLibraryDepsAmd64() {
  VerifyLibraryDepsCommon x86_64 linux-gnu
}


VerifyLibraryDepsI386() {
  VerifyLibraryDepsCommon i386 linux-gnu
}


VerifyLibraryDepsARM() {
  VerifyLibraryDepsCommon arm linux-gnueabihf
}


VerifyLibraryDepsARM64() {
  VerifyLibraryDepsCommon aarch64 linux-gnu
}

VerifyLibraryDepsARMEL() {
  VerifyLibraryDepsCommon arm linux-gnueabi
}

VerifyLibraryDepsMips() {
  VerifyLibraryDepsCommon mipsel linux-gnu
}


VerifyLibraryDepsMips64el() {
  VerifyLibraryDepsCommon mips64el linux-gnuabi64
}


#@
#@ BuildSysrootAmd64
#@
#@    Build everything and package it
BuildSysrootAmd64() {
  if [ "$HAS_ARCH_AMD64" = "0" ]; then
    return
  fi
  ClearInstallDir
  local package_file="${DEBIAN_DEP_LIST_AMD64}"
  GeneratePackageListAmd64 "$package_file"
  local files_and_sha256sums="$(cat ${package_file})"
  StripChecksumsFromPackageList "$package_file"
  InstallIntoSysroot ${files_and_sha256sums}
  HacksAndPatchesAmd64
  CleanupJailSymlinks
  VerifyLibraryDepsAmd64
  CreateTarBall
}

#@
#@ BuildSysrootI386
#@
#@    Build everything and package it
BuildSysrootI386() {
  if [ "$HAS_ARCH_I386" = "0" ]; then
    return
  fi
  ClearInstallDir
  local package_file="${DEBIAN_DEP_LIST_I386}"
  GeneratePackageListI386 "$package_file"
  local files_and_sha256sums="$(cat ${package_file})"
  StripChecksumsFromPackageList "$package_file"
  InstallIntoSysroot ${files_and_sha256sums}
  HacksAndPatchesI386
  CleanupJailSymlinks
  VerifyLibraryDepsI386
  CreateTarBall
}

#@
#@ BuildSysrootARM
#@
#@    Build everything and package it
BuildSysrootARM() {
  if [ "$HAS_ARCH_ARM" = "0" ]; then
    return
  fi
  ClearInstallDir
  local package_file="${DEBIAN_DEP_LIST_ARM}"
  GeneratePackageListARM "$package_file"
  local files_and_sha256sums="$(cat ${package_file})"
  StripChecksumsFromPackageList "$package_file"
  InstallIntoSysroot ${files_and_sha256sums}
  HacksAndPatchesARM
  CleanupJailSymlinks
  VerifyLibraryDepsARM
  CreateTarBall
}

#@
#@ BuildSysrootARM64
#@
#@    Build everything and package it
BuildSysrootARM64() {
  if [ "$HAS_ARCH_ARM64" = "0" ]; then
    return
  fi
  ClearInstallDir
  local package_file="${DEBIAN_DEP_LIST_ARM64}"
  GeneratePackageListARM64 "$package_file"
  local files_and_sha256sums="$(cat ${package_file})"
  StripChecksumsFromPackageList "$package_file"
  InstallIntoSysroot ${files_and_sha256sums}
  HacksAndPatchesARM64
  CleanupJailSymlinks
  VerifyLibraryDepsARM64
  CreateTarBall
}

#@
#@ BuildSysrootARMEL
#@
#@    Build everything and package it
BuildSysrootARMEL() {
  if [ "$HAS_ARCH_ARMEL" = "0" ]; then
    return
  fi
  ClearInstallDir
  local package_file="${DEBIAN_DEP_LIST_ARMEL}"
  GeneratePackageListARMEL "$package_file"
  local files_and_sha256sums="$(cat ${package_file})"
  StripChecksumsFromPackageList "$package_file"
  InstallIntoSysroot ${files_and_sha256sums}
  HacksAndPatchesARMEL
  CleanupJailSymlinks
  VerifyLibraryDepsARMEL
  CreateTarBall
}

#@
#@ BuildSysrootMips
#@
#@    Build everything and package it
BuildSysrootMips() {
  if [ "$HAS_ARCH_MIPS" = "0" ]; then
    return
  fi
  ClearInstallDir
  local package_file="${DEBIAN_DEP_LIST_MIPS}"
  GeneratePackageListMips "$package_file"
  local files_and_sha256sums="$(cat ${package_file})"
  StripChecksumsFromPackageList "$package_file"
  InstallIntoSysroot ${files_and_sha256sums}
  HacksAndPatchesMips
  CleanupJailSymlinks
  VerifyLibraryDepsMips
  CreateTarBall
}

#@
#@ BuildSysrootMips64el
#@
#@    Build everything and package it
BuildSysrootMips64el() {
  if [ "$HAS_ARCH_MIPS64EL" = "0" ]; then
    return
  fi
  ClearInstallDir
  local package_file="${DEBIAN_DEP_LIST_MIPS64EL}"
  GeneratePackageListMips64el "$package_file"
  local files_and_sha256sums="$(cat ${package_file})"
  StripChecksumsFromPackageList "$package_file"
  InstallIntoSysroot ${files_and_sha256sums}
  HacksAndPatchesMips64el
  CleanupJailSymlinks
  VerifyLibraryDepsMips64el
  CreateTarBall
}

#@
#@ BuildSysrootAll
#@
#@    Build sysroot images for all architectures
BuildSysrootAll() {
  RunCommand BuildSysrootAmd64
  RunCommand BuildSysrootI386
  RunCommand BuildSysrootARM
  RunCommand BuildSysrootARM64
  RunCommand BuildSysrootARMEL
  RunCommand BuildSysrootMips
  RunCommand BuildSysrootMips64el
}

UploadSysroot() {
  local sha=$(sha1sum "${TARBALL}" | awk '{print $1;}')
  set -x
  gsutil.py cp -a public-read "${TARBALL}" \
      "gs://chrome-linux-sysroot/toolchain/$sha/"
  set +x
}

#@
#@ UploadSysrootAmd64
#@
UploadSysrootAmd64() {
  if [ "$HAS_ARCH_AMD64" = "0" ]; then
    return
  fi
  UploadSysroot "$@"
}

#@
#@ UploadSysrootI386
#@
UploadSysrootI386() {
  if [ "$HAS_ARCH_I386" = "0" ]; then
    return
  fi
  UploadSysroot "$@"
}

#@
#@ UploadSysrootARM
#@
UploadSysrootARM() {
  if [ "$HAS_ARCH_ARM" = "0" ]; then
    return
  fi
  UploadSysroot "$@"
}

#@
#@ UploadSysrootARM64
#@
UploadSysrootARM64() {
  if [ "$HAS_ARCH_ARM64" = "0" ]; then
    return
  fi
  UploadSysroot "$@"
}

#@
#@ UploadSysrootARMEL
#@
UploadSysrootARMEL() {
  if [ "$HAS_ARCH_ARMEL" = "0" ]; then
    return
  fi
  UploadSysroot "$@"
}

#@
#@ UploadSysrootMips
#@
UploadSysrootMips() {
  if [ "$HAS_ARCH_MIPS" = "0" ]; then
    return
  fi
  UploadSysroot "$@"
}

#@
#@ UploadSysrootMips64el
#@
UploadSysrootMips64el() {
  if [ "$HAS_ARCH_MIPS64EL" = "0" ]; then
    return
  fi
  UploadSysroot "$@"
}

#@
#@ UploadSysrootAll
#@
#@    Upload sysroot image for all architectures
UploadSysrootAll() {
  RunCommand UploadSysrootAmd64 "$@"
  RunCommand UploadSysrootI386 "$@"
  RunCommand UploadSysrootARM "$@"
  RunCommand UploadSysrootARM64 "$@"
  RunCommand UploadSysrootARMEL "$@"
  RunCommand UploadSysrootMips "$@"
  RunCommand UploadSysrootMips64el "$@"

}

#
# CheckForDebianGPGKeyring
#
#     Make sure the Debian GPG keys exist. Otherwise print a helpful message.
#
CheckForDebianGPGKeyring() {
  if [ ! -e "$KEYRING_FILE" ]; then
    echo "KEYRING_FILE not found: ${KEYRING_FILE}"
    echo "Debian GPG keys missing. Install the debian-archive-keyring package."
    exit 1
  fi
}

#
# VerifyPackageListing
#
#     Verifies the downloaded Packages.xz file has the right checksums.
#
VerifyPackageListing() {
  local file_path="$1"
  local output_file="$2"
  local repo="$3"
  local dist="$4"

  local repo_basedir="${repo}/dists/${dist}"
  local release_list="${repo_basedir}/${RELEASE_FILE}"
  local release_list_gpg="${repo_basedir}/${RELEASE_FILE_GPG}"

  local release_file="${BUILD_DIR}/${dist}-${RELEASE_FILE}"
  local release_file_gpg="${BUILD_DIR}/${dist}-${RELEASE_FILE_GPG}"

  CheckForDebianGPGKeyring

  DownloadOrCopyNonUniqueFilename ${release_list} ${release_file}
  DownloadOrCopyNonUniqueFilename ${release_list_gpg} ${release_file_gpg}
  echo "Verifying: ${release_file} with ${release_file_gpg}"
  set -x
  gpgv --keyring "${KEYRING_FILE}" "${release_file_gpg}" "${release_file}"
  set +x

  echo "Verifying: ${output_file}"
  local sha256sum=$(grep -E "${file_path}\$|:\$" "${release_file}" | \
    grep "SHA256:" -A 1 | xargs echo | awk '{print $2;}')

  if [ "${#sha256sum}" -ne "64" ]; then
    echo "Bad sha256sum from ${release_list}"
    exit 1
  fi

  echo "${sha256sum}  ${output_file}" | sha256sum --quiet -c
}

#
# GeneratePackageList
#
#     Looks up package names in ${BUILD_DIR}/Packages and write list of URLs
#     to output file.
#
GeneratePackageList() {
  local input_file="$1"
  local output_file="$2"
  echo "Updating: ${output_file} from ${input_file}"
  /bin/rm -f "${output_file}"
  shift
  shift
  local failed=0
  for pkg in $@ ; do
    local pkg_full=$(grep -A 1 " ${pkg}\$" "$input_file" | \
      egrep "pool/.*" | sed 's/.*Filename: //')
    if [ -z "${pkg_full}" ]; then
      echo "ERROR: missing package: $pkg"
      local failed=1
    else
      local sha256sum=$(grep -A 4 " ${pkg}\$" "$input_file" | \
        grep ^SHA256: | sed 's/^SHA256: //')
      if [ "${#sha256sum}" -ne "64" ]; then
        echo "Bad sha256sum from Packages"
        local failed=1
      fi
      echo $pkg_full $sha256sum >> "$output_file"
    fi
  done
  if [ $failed -eq 1 ]; then
    exit 1
  fi
  # sort -o does an in-place sort of this file
  sort "$output_file" -o "$output_file"
}

#@
#@ PrintArchitectures
#@
#@    Prints supported architectures.
PrintArchitectures() {
  if [ "$HAS_ARCH_AMD64" = "1" ]; then
    echo Amd64
  fi
  if [ "$HAS_ARCH_I386" = "1" ]; then
    echo I386
  fi
  if [ "$HAS_ARCH_ARM" = "1" ]; then
    echo ARM
  fi
  if [ "$HAS_ARCH_ARM64" = "1" ]; then
    echo ARM64
  fi
  if [ "$HAS_ARCH_ARMEL" = "1" ]; then
    echo ARMEL
  fi
  if [ "$HAS_ARCH_MIPS" = "1" ]; then
    echo Mips
  fi
  if [ "$HAS_ARCH_MIPS64EL" = "1" ]; then
    echo Mips64el
  fi
}

#@
#@ PrintDistro
#@
#@    Prints distro.  eg: ubuntu
PrintDistro() {
  echo ${DISTRO}
}

#@
#@ DumpRelease
#@
#@    Prints disto release.  eg: jessie
PrintRelease() {
  echo ${DIST}
}

RunCommand() {
  SetEnvironmentVariables "$1"
  SanityCheck
  "$@"
}

if [ $# -eq 0 ] ; then
  echo "ERROR: you must specify a mode on the commandline"
  echo
  Usage
  exit 1
elif [ "$(type -t $1)" != "function" ]; then
  echo "ERROR: unknown function '$1'." >&2
  echo "For help, try:"
  echo "    $0 help"
  exit 1
else
  ChangeDirectory
  if echo $1 | grep -qs --regexp='\(^Print\)\|\(All$\)'; then
    "$@"
  else
    RunCommand "$@"
  fi
fi

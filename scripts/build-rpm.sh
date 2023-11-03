#!/bin/sh
#
# build-rpm.sh is meant to be run inside a `dev` or `compile` container
# (as defined in Dockerfile in the root of this repo).
#
# It should be started from the root of the repo.
#
# For historic reasons, the final package/RPM is named `fusedav-release`.
# In the past, alternate package "channels" (`dev`, `stage`, and `yolo`) could
# be installed simultaneously, allowing use of a different binary by merely
# calling fusedav with a different path.  This is no longer supported.
#
# Upon build success, the fusedav RPM will be written to the pkg/28/fusedav
# directory.
#
set -euo pipefail

if [ ! -r new-version.sh ] ; then
  echo "new-version.sh is missing, try: \"scripts/compute-version.sh | tee new-version.sh\""
  exit 1
fi

eval $(cat new-version.sh)

# capture latest RPM version/release to simplify later RPM extraction
echo "${RPM_VERSION}-${RPM_RELEASE}" > LATEST-RPM-VER-REL

# create VERSION for autoconf
echo "${SEMVER}" > VERSION

mkdir -p "${HOME}/rpmbuild/SOURCES" "${HOME}/rpmbuild/SPECS"

spec_path="${HOME}/rpmbuild/SPECS/fusedav.spec"
if [ -e $spec_path ] ; then
  echo "Cowardly refusing to overwrite ${spec_path}"
  exit 1
fi

# build vars from environment
fedora_release=$(rpm -q --queryformat '%{VERSION}\n' fedora-release)

echo "CREATE SOURCE ARCHIVE"
tar czf "${HOME}/rpmbuild/SOURCES/fusedav-${RPM_VERSION}.tar.gz" \
  --transform "s,^,fusedav-${RPM_VERSION}/," \
  LICENSE \
  Makefile.am \
  VERSION \
  autogen.sh \
  configure.ac \
  scripts/exec_wrapper/mount.fusedav_chan \
  src/*.c \
  src/*.h \
  src/Makefile.am

sed -e "s/RPM_VERSION/${RPM_VERSION}/" \
  -e "s/RPM_RELEASE/${RPM_RELEASE}/" \
  fusedav-template.spec \
  > $spec_path

echo "BUILD RPM"
echo "SEMVER=${SEMVER}"
echo "RPM_VERSION=${RPM_VERSION}"
echo "RPM_RELEASE=${RPM_RELEASE}"
rpmbuild -ba $spec_path

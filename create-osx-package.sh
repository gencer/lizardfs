#!/usr/bin/env bash
set -eux

# Directories used by this script
output_dir=$(pwd)
source_dir=$(dirname "$0")
working_dir=/tmp/lizardfs_osx_working_directory
install_dir=${working_dir}/lizardfs/
lizard_version=$(grep "set(PACKAGE_VERSION_"  CMakeLists.txt | awk '{print $2}' | cut -d")" -f1 |xargs | sed -e 's/\ /./g')

# Create an empty working directory and clone sources there to make
# sure there are no additional files included in the source package
rm -rf "$working_dir"
mkdir "$working_dir"
git clone "$source_dir" "$working_dir/lizardfs"

# Build packages.
cd "$working_dir/lizardfs"
if [[ ${BUILD_NUMBER:-} && ${OFFICIAL_RELEASE:-} == "false" ]] ; then
	# Jenkins has called us. Add build number to the package version
	# and add information about commit to changelog.
	version="${lizard_version}.${BUILD_NUMBER}"
else
	version=$lizard_version
fi

mkdir build-osx
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX=${working_dir}/lizardfs/build-osx/usr/local/ && make -j4 && make install
pkgbuild --root ${working_dir}/lizardfs/build-osx/ --identifier com.lizardfs --version $version --ownership recommended ../lizardfs-${version}.pkg

# Copy all the created files and clean up
cp "$working_dir"/lizardfs/lizardfs?* "$output_dir"
rm -rf "$working_dir"

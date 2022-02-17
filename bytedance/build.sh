#!/bin/bash

set -e

declare -A distributions=(
	["9"]="stretch"
	["10"]="buster"
	["11"]="bullseye"
	["stretch"]="stretch"
	["buster"]="buster"
	["bullseye"]="bullseye"
)

declare -A archs=(
	["x86_64"]="amd64"
	["aarch64"]="arm64"
)

function usage {
	echo "$0 [-d stretch|buster|bullseye|9|10|11] [-o OUTPUT] [-p PATTERN]"
	echo ""
	echo "Build Linux kernel based on distribution of the current machine or specified by the option of -d"
	echo ""
	echo "OPTIONS:"
	echo "      -d distribution. The valid parameter are 'stretch'(aka '9') or 'buster'(aka '10') or 'bullseye'(aka '11')"
	echo "      -o output. The location to store deb packages (defaults to current directory)."
	echo "      -p specify a tag (git) pattern to search for a specific tag to build kernel. Defaults to v[0-9].[0-9]*.[0-9]*.bsk.[0-9]*"
	echo "      -h help"
	echo ""
}

while getopts ":d:hj:o:p:" opt
do
	case $opt in
		o) output=$OPTARG
			;;
		d) distribution=$OPTARG
			;;
		h) usage
			exit
			;;
		j)
			# Ignore
			;;
		p) pattern=$OPTARG
			;;
		\?) echo "Unknown option: -$OPTARG" >&2
			usage
			exit 1
			;;
	esac

	if [ "$opt" != "d" ]; then
		parameter="$parameter -$opt $OPTARG"
	fi
done

arch=${archs[$(uname -m)]}
if [ -z $arch ]; then
	echo "Unknown arch: $(uname -m)"
	exit 1
fi

toplevel=$(git rev-parse --show-toplevel)

if [ ! -z $distribution ]; then
	distribution=${distributions[$distribution]}
	if [ -z $distribution ]; then
		usage
		exit 1
	fi

	image=kernel-docker-build-$distribution-$(arch)-latest
	docker build --network=host							\
		     -f $(dirname $0)/dockerfiles/$distribution-$(arch).dockerfile	\
		     -t $image $(dirname $0)

	tmp=$(mktemp -d /var/tmp/linux-build.XXXXXX)
	build="$tmp/$distribution"
	trap "rm -rf $tmp" SIGHUP SIGINT SIGQUIT SIGTERM ERR EXIT
	mkdir $build

	echo "Building Linux kernel by using image: $image"
	docker run --network=host -ti --rm 					\
		   -v $build:/linux						\
		   -v $toplevel:/linux/src					\
		   $image							\
		   bash -c "set -e;						\
			    export DEBIAN_FRONTEND=noninteractive;		\
			    cd /linux/src/$(git rev-parse --show-prefix);	\
			    $0 $parameter"

	output="$toplevel/$output/$distribution"
	if test -e $output; then
		rm -rf $output
	fi
	mkdir -p $output
	mv $build/*.deb $output
	exit 0
fi

pattern=${pattern:="v[0-9].[0-9]*.[0-9]*.bsk.[0-9]*"}
version=$(git describe --dirty --tags --match $pattern | cut -c 2-)
if [ -z $version ]; then
	echo "Unknown tag pattern: $pattern"
	exit 1
fi

timestamp="Debian $version $(date)"

cd $toplevel
# The source tree should be clean
git clean -idfxq
cp config.$(uname -m) .config
make olddefconfig

make deb-pkg					\
     BUILD_TOOLS=y				\
     KDEB_PKGVERSION=$version			\
     KERNELRELEASE=$version-$arch		\
     LOCALVERSION=_$version			\
     KBUILD_BUILD_TIMESTAMP="$timestamp"	\
     KBUILD_BUILD_USER="STE-Kernel"		\
     KBUILD_BUILD_HOST="ByteDance"		\
     DPKG_FLAGS="-sn"				\
     CFLAGS_KERNEL=-Werror			\
     CFLAGS_MODULE=-Werror			\
     -j$(nproc)

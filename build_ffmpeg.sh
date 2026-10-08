#!/bin/sh

arch=x86_64
archdir=x64
clean_build=true
cross_prefix=x86_64-w64-mingw32-

CV2PDB=../thirdparty/contrib/cv2pdb.exe

for opt in "$@"
do
    case "$opt" in
    x64 | amd64)
            ;;
    quick)
            clean_build=false
            ;;
    *)
            echo "Unknown Option $opt"
            exit 1
    esac
done

make_dirs() (
  mkdir -p bin_${archdir}/lib
  mkdir -p bin_${archdir}d/lib
)

# Fail when an FFmpeg DLL imports a MinGW runtime DLL that is not shipped.
# Only the FFmpeg DLLs themselves and libwinpthread-1.dll go into the package.
# The DLLs use the Universal C Runtime; msvcrt.dll means the build ran in a
# MINGW64 shell instead of UCRT64.
check_imports() (
  status=0
  for file in lib*/*-lav-*.dll; do
    for dep in $(objdump -p "$file" | sed -n 's/^[[:space:]]*DLL Name: //p'); do
      case "$(echo "$dep" | tr 'A-Z' 'a-z')" in
      *-lav-*.dll | libwinpthread-1.dll)
        ;;
      lib*.dll | zlib1.dll)
        echo "$file imports $dep, which is not part of the release package"
        status=1
        ;;
      msvcrt.dll)
        echo "$file imports msvcrt.dll; build FFmpeg from an MSYS2 UCRT64 shell"
        status=1
        ;;
      esac
    done
  done
  exit $status
)

copy_libs() (
  # copy and process .dll/.pdb
  for file in lib*/*-lav-*.dll; do
    file_basename=$(basename $file)
    file_pdb=$(basename $file .dll).pdb
    ${CV2PDB} -p${file_pdb} ${file} ../bin_${archdir}d/${file_basename}
    cp ../bin_${archdir}d/${file_basename} ../bin_${archdir}/
    cp ../bin_${archdir}d/${file_pdb} ../bin_${archdir}/
  done

  # copy lib files
  cp -u lib*/*.lib ../bin_${archdir}/lib
  cp -u lib*/*.lib ../bin_${archdir}d/lib
)

clean() (
  make distclean > /dev/null 2>&1
)

configure() (
  OPTIONS="
    --enable-shared                 \
    --disable-static                \
    --enable-gpl                    \
    --enable-version3               \
    --disable-autodetect            \
    --enable-w32threads             \
    --disable-demuxer=matroska      \
    --disable-filters               \
    --enable-filter=scale,yadif,w3fdif,bwdif \
    --disable-protocol=async,cache,concat,httpproxy,icecast,md5,subfile \
    --disable-muxers                \
    --enable-muxer=spdif            \
    --disable-bsfs                  \
    --enable-bsf=extract_extradata  \
    --disable-avdevice              \
    --disable-encoders              \
    --disable-devices               \
    --disable-programs              \
    --disable-debug                 \
    --disable-doc                   \
    --enable-avisynth               \
    --enable-bzlib                  \
    --enable-d3d11va                \
    --enable-dxva2                  \
    --enable-gnutls                 \
    --enable-gmp                    \
    --enable-libdav1d               \
    --enable-libspeex               \
    --enable-libopencore-amrnb      \
    --enable-libopencore-amrwb      \
    --enable-libxml2                \
    --enable-zlib                   \
    --build-suffix=-lav             \
    --disable-stripping             \
    --arch=${arch}"

  EXTRA_CFLAGS="-fno-tree-vectorize -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -gdwarf-5"
  # -static-libgcc embeds the GCC SEH runtime into each DLL so the output does
  # not depend on libgcc_s_seh-1.dll.
  EXTRA_LDFLAGS="-static-libgcc"

  # Link zlib statically so the DLLs do not depend on zlib1.dll. ld takes -lz
  # from the first directory holding libz.dll.a or libz.a, so search one that
  # holds only the static archive ahead of the toolchain's own lib directory.
  STATIC_LIBS_DIR="$(pwd)/ffbuild/static-libs"
  ZLIB_ARCHIVE="$(${cross_prefix}gcc -print-file-name=libz.a)"
  if [ ! -f "${ZLIB_ARCHIVE}" ]; then
    echo "Static zlib (libz.a) was not found; install mingw-w64-ucrt-x86_64-zlib"
    exit 1
  fi
  mkdir -p "${STATIC_LIBS_DIR}"
  cp -f "${ZLIB_ARCHIVE}" "${STATIC_LIBS_DIR}/"
  EXTRA_LDFLAGS="${EXTRA_LDFLAGS} -L${STATIC_LIBS_DIR}"
  THIRDPARTY_ABS="$(cd ../thirdparty/64 && pwd)"
  export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:${THIRDPARTY_ABS}/lib/pkgconfig/"
  OPTIONS="${OPTIONS} --enable-cross-compile --cross-prefix=${cross_prefix} --target-os=mingw32 --pkg-config=pkg-config"
  EXTRA_CFLAGS="${EXTRA_CFLAGS} -I${THIRDPARTY_ABS}/include -fno-omit-frame-pointer"
  EXTRA_LDFLAGS="${EXTRA_LDFLAGS} -L${THIRDPARTY_ABS}/lib"
  PKG_CONFIG_PREFIX_DIR="--define-variable=prefix=${THIRDPARTY_ABS}"

  sh configure --extra-ldflags="${EXTRA_LDFLAGS}" --extra-cflags="${EXTRA_CFLAGS}" --pkg-config-flags="--static ${PKG_CONFIG_PREFIX_DIR}" ${OPTIONS}
)

build() (
  make -j$NUMBER_OF_PROCESSORS
)

make_dirs

echo
echo Building ffmpeg in GCC ${arch} Release config...
echo

cd ffmpeg

if $clean_build ; then
    clean

    ## run configure, redirect to file because of a msys bug
    configure > ffbuild/config.out 2>&1
    CONFIGRETVAL=$?

    ## show configure output
    cat ffbuild/config.out
fi

## Only if configure succeeded, actually build
if ! $clean_build || [ ${CONFIGRETVAL} -eq 0 ]; then
  build &&
  check_imports &&
  copy_libs || exit 1
fi

cd ..

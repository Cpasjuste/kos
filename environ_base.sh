# KallistiOS environment variable settings. These are the shared pieces
# that are generated from the user config. Configure if you like.

# Default the kos-ports path if it isn't already set.
if [ -z "${KOS_PORTS}" ] ; then
    export KOS_PORTS="${KOS_BASE}/../kos-ports"
fi

# Pull in the arch environ file
. ${KOS_BASE}/environ_${KOS_ARCH}.sh

# Add the gnu wrappers dir to the path
export PATH="${PATH}:${KOS_BASE}/utils/gnu_wrappers"

# Our includes
export KOS_INC_PATHS="${KOS_INC_PATHS} -I${KOS_BASE}/include \
-I${KOS_BASE}/kernel/arch/${KOS_ARCH}/include -I${KOS_BASE}/addons/include \
-I${KOS_PORTS}/include"

# "System" libraries
export KOS_LIB_PATHS="-L${KOS_BASE}/lib/${KOS_ARCH} -L${KOS_BASE}/addons/lib/${KOS_ARCH} -L${KOS_PORTS}/lib"
export KOS_LIBS="-Wl,--start-group -lkallisti -lc -lgcc -Wl,--end-group"

# Main arch compiler paths
export KOS_CC="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-gcc"
export KOS_CCPLUS="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-g++"
export KOS_AS="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-as"
export KOS_AR="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-ar"
export KOS_OBJCOPY="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-objcopy"
export KOS_LD="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-ld"
export KOS_RANLIB="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-ranlib"
export KOS_STRIP="${KOS_CC_BASE}/bin/${KOS_CC_PREFIX}-strip"
export KOS_CFLAGS="${KOS_CFLAGS} ${KOS_INC_PATHS} -D_arch_${KOS_ARCH} -D_arch_sub_${KOS_SUBARCH} -Wall -g -fno-builtin"
export KOS_CPPFLAGS="${KOS_CPPFLAGS} ${KOS_INC_PATHS_CPP} -fno-operator-names -fno-rtti -fno-exceptions"

# Which standards modes we want to compile for
# Note that this only covers KOS itself, not necessarily anything else compiled
# with kos-cc or kos-c++.
export KOS_CSTD="-std=c99"
export KOS_CPPSTD="-std=gnu++98"

export KOS_GCCVER="`kos-cc -dumpversion`"

case $KOS_GCCVER in
  2* | 3*)
    export KOS_LDFLAGS="${KOS_LDFLAGS} -nostartfiles -nostdlib ${KOS_LIB_PATHS}" ;;
  *)
    export KOS_LDFLAGS="${KOS_LDFLAGS} ${KOS_LD_SCRIPT} -nodefaultlibs ${KOS_LIB_PATHS}" ;;
esac

# Some extra vars based on architecture
export KOS_ARCH_DIR="${KOS_BASE}/kernel/arch/${KOS_ARCH}"

case $KOS_GCCVER in
  2* | 3*)
    export KOS_START="${KOS_ARCH_DIR}/kernel/startup.o" ;;
  *)
    export KOS_START="" ;;
esac

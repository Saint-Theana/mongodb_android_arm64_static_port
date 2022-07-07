#!/bin/sh

set -e
set -v

rm -rf extract include

mkdir extract
mkdir extract/js
mkdir -p extract/intl/icu/source/common/unicode

# We need these even without ICU
cp mozilla-release/intl/icu/source/common/unicode/platform.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/ptypes.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/uconfig.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/umachine.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/urename.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/utypes.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/uvernum.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/uversion.h extract/intl/icu/source/common/unicode

cd mozilla-release/js/src

# skipping icu and relying on posix nspr emulation all helps.  After that we
# only need js/src, js/public and mfbt.  Well, we also need 8 of the icu
# headers, but only to stub out functions that fail at runtime
PYTHON=python ./configure --without-intl-api --enable-posix-nspr-emulation --disable-js-shell --disable-tests

# we have to run make to generate a byte code version of the self hosted js and
# a switch table
make

cd ../../..

cp -r mozilla-release/mfbt extract/

cp -r mozilla-release/js/src mozilla-release/js/public extract/js/

cp mozilla-release/js/src/js/src/selfhosted.out.h extract/js/src
mkdir -p extract/js/src/frontend
cp mozilla-release/js/src/js/src/frontend/ReservedWordsGenerated.h extract/js/src/frontend

mkdir -p include/gc
cp extract/js/src/js/src/gc/StatsPhasesGenerated.h include/gc
mkdir -p extract/js/src/gc
cp extract/js/src/js/src/gc/StatsPhasesGenerated.cpp extract/js/src/gc

# mfbt doesn't change by arch or platform, so keep the same unified cpp
mkdir -p extract/js/src/mfbt
cp mozilla-release/js/src/mfbt/Unified_cpp_mfbt0.cpp extract/js/src/mfbt

sed 's/#include ".*\/mfbt\//#include "/' < extract/js/src/mfbt/Unified_cpp_mfbt0.cpp > t1
sed 's/#error ".*\/mfbt\//#error "/' < t1 > extract/js/src/mfbt/Unified_cpp_mfbt0.cpp
rm t1

# stuff we can toss
rm -rf \
    extract/js/src/all-tests.json \
    extract/js/src/backend.mk \
    extract/js/src/backend.RecursiveMakeBackend \
    extract/js/src/backend.RecursiveMakeBackend.pp \
    extract/js/src/_build_manifests \
    extract/js/src/config \
    extract/js/src/config.cache \
    extract/js/src/config.log \
    extract/js/src/config.status \
    extract/js/src/configure \
    extract/js/src/configure.in \
    extract/js/src/ctypes \
    extract/js/src/doc \
    extract/js/src/editline \
    extract/js/src/gdb \
    extract/js/src/ipc \
    extract/js/src/jit-test \
    extract/js/src/jsapi-tests \
    extract/js/src/Makefile \
    extract/js/src/Makefile.in \
    extract/js/src/make-source-package.sh \
    extract/js/src/octane \
    extract/js/src/python \
    extract/js/src/README.html \
    extract/js/src/root-deps.mk \
    extract/js/src/root.mk \
    extract/js/src/shell \
    extract/js/src/skip_subconfigures \
    extract/js/src/subconfigures \
    extract/js/src/tests \
    extract/js/src/_virtualenv

# stuff we have to replace
rm -rf \
    extract/js/src/vm/PosixNSPR.cpp \
    extract/js/src/vm/PosixNSPR.h \

# stuff we don't want to deal with due to licensing
rm -rf \
    extract/mfbt/decimal \
    extract/mfbt/tests \
    extract/js/src/util/make_unicode.py \
    extract/js/src/vtune

# this is all of the EXPORTS files from the moz.build
mkdir -p include
for i in 'js.msg' 'jsapi.h' 'jsfriendapi.h' 'jspubtd.h' 'jstypes.h' 'perf/jsperf.h' ; do
    cp extract/js/src/$i include
done

# this is all of the EXPORTS.js files from the moz.build
mkdir -p include/js
for i in 'AllocPolicy.h' 'CallArgs.h' 'CallNonGenericMethod.h' 'CharacterEncoding.h' 'Class.h' 'Conversions.h' 'Date.h' 'Debug.h' 'GCAnnotations.h' 'GCAPI.h' 'GCHashTable.h' 'GCPolicyAPI.h' 'GCVariant.h' 'GCVector.h' 'HashTable.h' 'HeapAPI.h' 'Id.h' 'Initialization.h' 'MemoryMetrics.h' 'Principals.h' 'Printf.h' 'ProfilingFrameIterator.h' 'ProfilingStack.h' 'ProtoKey.h' 'Proxy.h' 'Realm.h' 'RefCounted.h' 'RequiredDefines.h' 'Result.h' 'RootingAPI.h' 'SliceBudget.h' 'Stream.h' 'StructuredClone.h' 'SweepingAPI.h' 'TraceKind.h' 'TracingAPI.h' 'TrackedOptimizationInfo.h' 'TypeDecls.h' 'UbiNode.h' 'UbiNodeBreadthFirst.h' 'UbiNodeCensus.h' 'UbiNodeDominatorTree.h' 'UbiNodePostOrder.h' 'UbiNodeShortestPaths.h' 'UniquePtr.h' 'Utility.h' 'Value.h' 'Vector.h' 'WeakMapPtr.h' 'Wrapper.h' ; do
    cp extract/js/public/$i include/js
done

# this is all of the EXPORTS.mozilla files from the moz.build's
mkdir -p include/mozilla
for i in 'Alignment.h' 'AllocPolicy.h' 'AlreadyAddRefed.h' 'Array.h' 'ArrayUtils.h' 'Assertions.h' 'Atomics.h' 'Attributes.h' 'BinarySearch.h' 'BloomFilter.h' 'BufferList.h' 'Casting.h' 'ChaosMode.h' 'Char16.h' 'CheckedInt.h' 'Compiler.h' 'Compression.h' 'DebugOnly.h' 'DefineEnum.h' 'DoublyLinkedList.h' 'EndianUtils.h' 'EnumeratedArray.h' 'EnumeratedRange.h' 'EnumSet.h' 'EnumTypeTraits.h' 'FastBernoulliTrial.h' 'FloatingPoint.h' 'FStream.h' 'GuardObjects.h' 'HashFunctions.h' 'IndexSequence.h' 'IntegerPrintfMacros.h' 'IntegerRange.h' 'IntegerTypeTraits.h' 'JSONWriter.h' 'Likely.h' 'LinkedList.h' 'MacroArgs.h' 'MacroForEach.h' 'MathAlgorithms.h' 'Maybe.h' 'MaybeOneOf.h' 'MemoryChecking.h' 'MemoryReporting.h' 'Move.h' 'NotNull.h' 'NullPtr.h' 'Opaque.h' 'OperatorNewExtensions.h' 'Pair.h' 'Path.h' 'PodOperations.h' 'Poison.h' 'Range.h' 'RangedArray.h' 'RangedPtr.h' 'ReentrancyGuard.h' 'RefCounted.h' 'RefCountType.h' 'RefPtr.h' 'Result.h' 'ResultExtensions.h' 'ReverseIterator.h' 'RollingMean.h' 'Saturate.h' 'Scoped.h' 'ScopeExit.h' 'SegmentedVector.h' 'SHA1.h' 'SmallPointerArray.h' 'Span.h' 'SplayTree.h' 'Sprintf.h' 'StaticAnalysisFunctions.h' 'TaggedAnonymousMemory.h' 'TemplateLib.h' 'TextUtils.h' 'ThreadLocal.h' 'ThreadSafeWeakPtr.h' 'ToString.h' 'Tuple.h' 'TypedEnumBits.h' 'Types.h' 'TypeTraits.h' 'UniquePtr.h' 'UniquePtrExtensions.h' 'Unused.h' 'Variant.h' 'Vector.h' 'WeakPtr.h' 'WrappingOperations.h' 'XorShift128PlusRNG.h' 'WindowsVersion.h' ; do
    cp extract/mfbt/$i include/mozilla
done

for i in 'malloc_decls.h' 'mozjemalloc_types.h' 'mozmemory.h' 'mozmemory_wrap.h' ; do
    cp extract/js/src/dist/include/$i include
done

mkdir -p include/double-conversion
cp -r extract/mfbt/double-conversion/double-conversion/*.h include/double-conversion

for i in 'AutoProfilerLabel.h' 'PlatformConditionVariable.h' 'PlatformMutex.h' 'Printf.h' 'StackWalk.h' 'TimeStamp.h' 'fallible.h' ; do
    cp extract/js/src/dist/include/mozilla/$i include/mozilla
done

cp extract/js/src/dist/include/fdlibm.h include
mkdir -p extract/modules/fdlibm
cp mozilla-release/modules/fdlibm/src/* extract/modules/fdlibm/

cp mozilla-release/mozglue/misc/*.h include
mkdir -p extract/mozglue/misc
cp mozilla-release/mozglue/misc/*.cpp extract/mozglue/misc
cp mozilla-release/mozglue/misc/StackWalk_windows.h include/mozilla/

mkdir -p include/vtune
touch include/vtune/VTuneWrapper.h

xargs rm -r<<__XARGS_RM__
extract/js/src/backend.FasterMakeBackend.in
extract/js/src/backend.RecursiveMakeBackend.in
extract/js/src/build/js-config.in
extract/js/src/build/js.pc.in
extract/js/src/build/Makefile.in
extract/js/src/build/symverscript.in
extract/js/src/build/unix/
extract/js/src/.cargo/
extract/js/src/config.statusd/
extract/js/src/configure.d
extract/js/src/devtools/gctrace/Makefile
extract/js/src/devtools/rootAnalysis/Makefile.in
extract/js/src/devtools/vprof/manifest.mk
extract/js/src/dist/bin/
extract/js/src/dist/include/double-conversion/
extract/js/src/dist/include/fdlibm.h
extract/js/src/dist/include/js/
extract/js/src/dist/include/jsapi.h
extract/js/src/dist/include/js-config.h
extract/js/src/dist/include/jsfriendapi.h
extract/js/src/dist/include/js.msg
extract/js/src/dist/include/jsperf.h
extract/js/src/dist/include/jspubtd.h
extract/js/src/dist/include/jstypes.h
extract/js/src/dist/include/malloc_decls.h
extract/js/src/dist/include/mozilla/
extract/js/src/dist/include/mozjemalloc_types.h
extract/js/src/dist/include/mozmemory.h
extract/js/src/dist/include/mozmemory_wrap.h
extract/js/src/dist/system_wrappers/
extract/js/src/faster/
extract/js/src/install_dist_bin.track
extract/js/src/install_dist_include.track
extract/js/src/install_dist_private.track
extract/js/src/install_dist_public.track
extract/js/src/install__tests.track
extract/js/src/js/
extract/js/src/js-confdefs.h.in
extract/js/src/js-config.h.in
extract/js/src/memory/backend.mk
extract/js/src/memory/build/
extract/js/src/memory/fallible/
extract/js/src/memory/Makefile
extract/js/src/memory/mozalloc/
extract/js/src/mfbt/backend.mk
extract/js/src/mfbt/.deps/
extract/js/src/mfbt/libmfbt.a.desc
extract/js/src/mfbt/Makefile
extract/js/src/mfbt/Unified_cpp_mfbt1.cpp
extract/js/src/modules/fdlibm/backend.mk
extract/js/src/modules/fdlibm/Makefile
extract/js/src/modules/fdlibm/src/backend.mk
extract/js/src/modules/fdlibm/src/.deps/
extract/js/src/modules/fdlibm/src/libmodules_fdlibm_src.a.desc
extract/js/src/modules/fdlibm/src/Makefile
extract/js/src/mozglue/backend.mk
extract/js/src/mozglue/build/backend.mk
extract/js/src/mozglue/build/libmozglue.a
extract/js/src/mozglue/build/libmozglue.a.desc
extract/js/src/mozglue/build/Makefile
extract/js/src/mozglue/Makefile
extract/js/src/mozglue/misc/backend.mk
extract/js/src/mozglue/misc/.deps/
extract/js/src/mozglue/misc/libmozglue_misc.a.desc
extract/js/src/mozglue/misc/Makefile
extract/js/src/mozinfo.json
extract/js/src/old-configure.in
extract/js/src/old-configure.vars
extract/js/src/taskcluster/
extract/js/src/testing/
extract/js/src/_tests/mozbase/
extract/js/src/third_party/
extract/js/src/wasm/Makefile
__XARGS_RM__

patch -p4 < patches/big-endian-fixes.patch
patch -p4 < patches/windows-Time.cpp-GetModuleHandle.patch
patch -p4 < patches/JSGCConfig.patch
patch -p4 < patches/struct-hasher.patch
patch -p4 < patches/freebsd-powerpc64le-fix.patch
patch -p4 < patches/moz-bug-1442583.patch

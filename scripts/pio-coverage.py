# gcov instrumentation for [env:native-cov].
#
# build_flags reaches the compiler only, so --coverage has to be on the link
# line too or the gcov symbols go unresolved.
#
# GCC drops the out-of-line copy of an inline function it fully inlined, and a
# header whose every function got inlined then produces no gcov entry at all -
# the unit reads as 100% or vanishes. -fkeep-inline-functions forces the copy so
# uncovered lines stay visible. Clang does not take the flag and does not need
# it: it emits the copies anyway.
Import("env")

env.Append(LINKFLAGS=["--coverage"])

if "clang" not in env.subst("$CXX"):
    env.Append(CCFLAGS=["-fkeep-inline-functions"])

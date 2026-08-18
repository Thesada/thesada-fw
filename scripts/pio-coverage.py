# gcov instrumentation for [env:native-cov]. build_flags reaches the compiler
# only; --coverage has to be on the link line too or the gcov symbols go unresolved.
Import("env")

env.Append(LINKFLAGS=["--coverage"])

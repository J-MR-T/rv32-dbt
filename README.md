# The project
A very basic dynamic binary translator based on LLVM. The granularity is generally fixed (basic block level), but single instruction stepping is supported. See `main --help`.

# Structure and Building
The source is contained in [`main.cc`](main.cc), the fadec encoder library is included via a git submodule in [`fadec/`](fadec), build instructions can be found in the [`Makefile`](Makefile).

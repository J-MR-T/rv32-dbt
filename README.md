This originally was coursework for the [Code Generation for Data Processing](https://db.in.tum.de/teaching/ws2223/codegen/?lang=en) course at TUM.
After finishing the course, I found myself wanting to more work on the translator. This has resulted in additional support for RV32M, and various fixes, and more samples.

# The project
A very basic dynamic binary translator based on LLVM. The granularity is generally fixed (basic block level), but single instruction stepping is supported. See `main --help`.

# Structure and Building
The source is contained in [`main.cc`](main.cc), the frvdec decoder library is included via a git submodule in `frvdec/`, build instructions can be found in the [`Makefile`](Makefile).

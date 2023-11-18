mkdir -p target

warnings="-Wall -Wextra -pedantic -Werror -Wno-unused-parameter -Wno-uninitialized -Wno-overloaded-virtual -Wl,-z,notext"

clang++ $warnings -m32 -static-libstdc++ -shared -O2 -std=c++2a -o target/main.so main.cpp

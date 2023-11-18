mkdir -p target

warnings="-Wall -Wextra -pedantic -Werror -Wno-unused-parameter -Wno-uninitialized -Wno-overloaded-virtual -Wl,-z,notext"

g++ $warnings -fno-gnu-unique -m32 -static-libstdc++ -shared -O2 -std=c++2a -o target/main.so main.cpp

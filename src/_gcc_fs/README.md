# GCC 8 "std::filesystem" Port

C++17 introduced the [`std::filesystem` library](https://en.cppreference.com/w/cpp/filesystem),
which greatly simplifies FS related operations, and especially for WebDAV handler implementations.

The latest official compiler for ESP8266 RTOS SDK is GCC 8.4, which supports C++17. However,
the binaries was built into a separate "libstdc++fs" module, which was not offered as a part of
Espressif's distribution.

I attempted to build directly from
[the source code](https://github.com/gcc-mirror/gcc/tree/releases/gcc-8/libstdc%2B%2B-v3/src/filesystem)
but it turned into a more involved effort.

## Changes made

1. RTOS SDK is **NOT** a 100% POSIX compliant environment, I had to:
   - Redirect and/or substitute some missing functions, such as `chmod()`, `symlink()`, `readlink()`;
   - Block out code involving non-existent system features, such as sockets, FIFO, char device, etc;
2. The directory iterator implementation uses `shared_ptr` which suffers a
   [known bug](https://github.com/espressif/ESP8266_RTOS_SDK/issues/991) and always crashes.
   - Luckily the use is just for the convenience of comparison, not critical to the core feature;
   - So I replaced all use of `shared_ptr` with `unique_ptr`, and updated related code to provide
     comparably equivalent implementations;
   - Note that it is *not* a 100% compatible replacement, due to the use of `unique_ptr` made the
     iterator non-copyable. So instead of `for (auto entry : directory_iterator(from)) {...}`,
     this pattern must be used: `for (directory_iterator iter(from);iter;++iter) {...}`;
     - Not so bad, huh? :P
3. Finally, somehow, linking to the `std::filesystem` implementation adds 170KB+ to the firmware!
   - I tried to reduce the size by blocking out code handling unneeded features, such as symlinks
     hard links and permissions. That approximately shaved off about 20KB;
   - It is still unclear to me why such a big size jump.

## How to use

1. `#include "filesystem.h` (as opposed to `#include <filesystem>`).
2. Directory iteration pattern is different, due to a compiler bug (see the section above).
3. Everything else work the same as [documented](https://en.cppreference.com/w/cpp/filesystem).

## License concerns?

GCC is released under
[GPL v3, with "Runtime Library Exception"](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html).
And this part of the feature *supposedly* should be released as a compiled library, along with the
compiler tool, which unfortunately didn't happen.

Had Espressif provided a functional library to link with, there will be no need for any of the
source code in this directory.

If anyone is concerned about licensing ambiguity or incompatibility, please let me know.
I will figure out how to build and release this part as a compiled library, under GCC's original
license (GPL v3, with "Runtime Library Exception").
# Vorbis decoder sources

The Vorbis mapping and decoder use the core sources from libvorbis 1.3.7 and the bit-packing routines
from libogg 1.3.6. The Ogg container uses Lib-Media's own page and stream handling. The CMake target
builds the reference sources from this directory, with fixed-width Ogg types on platforms that need
`config_types.h`. Each upstream license is in its corresponding `COPYING` file.
[Xiph releases and checksums](https://xiph.org/downloads/)

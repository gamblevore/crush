# blush

Fast and tight LZ Compression from small code. 

Original is named "crush", http://compressme.net . This is a beta. It is also not compatible with crush, but it shares the same compression ratio.

The idea is just to make the original crush compressor, more generally accessible from a programming environment. This way, I can use it in a programming language as the default compressor, or something like this.

I also sped it up quite a lot. The decompress is 2x-3x faster. Compression speed is almost unchanged, but slightly faster. Probably cos most of the time is spent finding matches, so the optimisations I made are swamped out by match-finding.

     compile: g++ -O3 blush.cpp -o blush -std=c++17

* Todo: make a proper C interface
    * Also make memory free-able, to avoid bloat.
* Make decomp share the buffer

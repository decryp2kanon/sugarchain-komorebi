# Official Sugarchain visual assets

Source: https://github.com/sugarchain-project/sugarchain
Commit: `64bc05ccc1dc2dcd4db9e86715c14dee12be6460`

The following files are copied without image modifications. In destination
filenames only, `bitcoin` is replaced with `sugarchain`:

- `src/qt/res/icons/bitcoin.png`, `bitcoin.ico`, `bitcoin_testnet.ico`, `bitcoin.icns`
- `share/pixmaps/bitcoin.ico`
- `share/pixmaps/bitcoin{16,32,64,128,256}.{png,xpm}`
- `share/pixmaps/nsis-header.bmp`, `nsis-wizard.bmp` (names unchanged)

The source Qt qrc/networkstyle and Windows RC use the PNG/ICO files;
its Makefile.am and macOS Info.plist use the ICNS; its NSIS script uses
share/pixmaps/bitcoin.ico and both installer bitmaps. The PNG/XPM sizes
are the source repository's platform icon variants.

The unused Bitcoin SVG in the source repository is not a Sugarchain logo
and is not used here. Qt resource aliases remain internal identifiers.

## Source repository license

The MIT License (MIT)

Copyright (c) 2009-2010 Satoshi Nakamoto
Copyright (c) 2009-2018 The Bitcoin Core developers
Copyright (c) 2013-2019 Alexander Peslyak - Yespower 1.0.1
Copyright (c) 2016-2018 The Zcash developers - DigiShieldZEC
Copyright (c) 2018-2020 The Sugarchain Yumekawa developers

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

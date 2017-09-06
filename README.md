# `bricks` C++ utilities library

`bricks` is a header-only C++ library of miscellaneous utilities developed with
the [DIVINE model checker](https://divine.fi.muni.cz). It contains data
structures, algorithms, and utilities which were general enough to not be
specific to DIVINE. The library is mostly C++11 compatible, but contains C++14
and C++17 code (which should be conditionally compiled).

## Using `bricks`

You can either embed all of bricks, or use just some of our libraries, they are
all header-only and not widely inter-dependent.

For embedding using version control, there two possibilities:

*   this git repository
*   darcs repository at https://paradise.fi.muni.cz/~xstill/code/bricks

If you are using CMake, including `bricks/support.cmake` will allow some
feature auto-detection and will allow you to use our unit testing framework.
Nevertheless, it should mostly be enough to add `bricks` directory to your
include path and include appropriate libraries.

## Contributing

The upstream of bricks is the DIVINE darcs repository (linked on the project
page). There is also a derived bricks-only darcs repository:

    darcs get https://paradise.fi.muni.cz/~xstill/code/bricks

If you want to contribute, the preferred way is to contribute using darcs:
create a patch in one of our repositories and send it to the DIVINE developers
using e-mail shown on the web page. Pull requests to this git repository might
eventually get upstream, but it will usually take longer (and they will not be
merged directly).

# Third-party software

The released package (`ChiakiTv.wgt`) contains a Native Client module that is built from this repository and statically links the following software. Their sources are not copied into this repository; see [docs/BUILDING.md](docs/BUILDING.md) for the exact versions.

| Component | License |
|---|---|
| chiaki-ng protocol library (`lib/`) | AGPL-3.0-only with an OpenSSL linking exception |
| Mbed TLS 2.28 | Apache-2.0 |
| curl | curl license (MIT style) |
| Opus | BSD-3-Clause |
| json-c | MIT |
| nanopb | zlib |
| Jerasure, GF-Complete | BSD-3-Clause |
| Native Client SDK libraries (nacl_io, PPAPI C++ wrappers, newlib) | BSD style |
| CA certificates extracted from Mozilla, as published by curl.se | MPL-2.0 |

chiaki-tv itself is licensed under the GNU Affero General Public License v3.0; see [LICENSE](LICENSE). The complete source for a released package is the tagged commit of this repository together with the versions listed above.

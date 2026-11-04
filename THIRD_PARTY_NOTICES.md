# Third-party notices

BonDriverRelay is licensed under the MIT License in LICENSE. The following
third-party components retain their respective copyrights and licenses.

| Component | Version / source | License | Distribution notice |
|---|---|---|---|
| spdlog | [v1.15.3](https://github.com/gabime/spdlog/tree/v1.15.3) | MIT | licenses/spdlog/LICENSE |
| fmt | [11.2.0, bundled with spdlog v1.15.3](https://github.com/gabime/spdlog/tree/v1.15.3/include/spdlog/fmt/bundled) | MIT | licenses/fmt/fmt.license.rst |
| toml++ | [v3.4.0](https://github.com/marzer/tomlplusplus/tree/v3.4.0) | MIT | licenses/tomlplusplus/LICENSE |
| libaribb25 ABI header | [v0.2.10, c30cbd1357f08129da59c2fcd01a0b1be50220c5](https://github.com/tsukumijima/libaribb25/tree/c30cbd1357f08129da59c2fcd01a0b1be50220c5) | Apache-2.0 | licenses/libaribb25/LICENSE and NOTICE |

Source distributions include the official libaribb25 IB25Decoder.h header and its
LICENSE, NOTICE, and SOURCE.md under third_party/libaribb25. Line endings in the
header are normalized to LF; interface declarations are unchanged. Its runtime
DLL and implementation are not included. The other components are fetched at
fixed versions and SHA-256 hashes by cmake/Dependencies.cmake; their notices are
included in binary distributions under licenses/.

The BonDriver ABI declarations describe the public IBonDriver / IBonDriver2
interfaces, following the public interface definitions in
[TVTest IBonDriver.h](https://github.com/DBCTRADO/TVTest/blob/master/src/Include/IBonDriver.h)
and [IBonDriver2.h](https://github.com/DBCTRADO/TVTest/blob/master/src/Include/IBonDriver2.h).
The reference Git blob IDs are 610d8c80b779acdd43391f0cacb9ac59975ee38f (v1)
and f6d6bf69aeb122cc23b2d7f7dad4240d224fcff5 (v2).
BonDriverRelay is an independent implementation. No BonDriverProxyExD
implementation code is included.

Tuner BonDriver DLLs, libaribb25.dll, winscard.dll, and the Microsoft Visual C++
Redistributable are external dependencies and are not bundled. Obtain them from
their respective providers and observe their licenses. BonDriverRelay's MIT
License does not relicense those dependencies.

# sherpa-onnx C API header

`c-api.h` is copied unchanged from sherpa-onnx v1.13.8
(`sherpa-onnx-v1.13.8-win-x64-shared-MD-Release.tar.bz2`), the same release
`src/ui/Speech.cpp` downloads and loads at run time. Only its declarations are
used: the library is loaded with QLibrary and nothing links against it.

Copyright (c) 2023 Xiaomi Corporation, licensed under the Apache License 2.0
(https://github.com/k2-fsa/sherpa-onnx/blob/master/LICENSE).

Keep it in step with the version in Speech.cpp: the structs it declares are
handed straight to that library, so a header from another release would
describe a different layout.

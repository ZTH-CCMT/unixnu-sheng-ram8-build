// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
// Diagnostic provenance only. Scopes never change a pipeline descriptor or result.
enum { NBOriginClient=0, NBOriginPrime=1, NBOriginBenchmark=2, NBOriginSelfTest=3 };
extern "C" uint32_t native_bridge_metal_diagnostic_origin(uint32_t);
struct NBMetalOriginScope {
 uint32_t saved;
 explicit NBMetalOriginScope(uint32_t origin):saved(native_bridge_metal_diagnostic_origin(origin)){}
 ~NBMetalOriginScope(){native_bridge_metal_diagnostic_origin(saved);}
 NBMetalOriginScope(const NBMetalOriginScope &)=delete;
 NBMetalOriginScope &operator=(const NBMetalOriginScope &)=delete;
};

# V3.1 touch modifications

This contribution extends the upstream NT36532 implementation with typed frame decoding, timed contact tracking, qualified partial observations and coverage-aware release decisions. Changed existing files are NvtProtocol.c, NvtProtocol.h, NvtTouchCore.c, NvtTouchCore.h, Test/nvt-protocol.c and README.md. Added files are NvtCoverage.c/.h, NvtPartialObservation.c/.h, NvtPartialMathInline.h and NvtShapeMetricsInline.h.

The original LICENSE and NOTICE are retained. The original NOTICE describes the earlier implementation; the added observer/tracker behavior is documented in README.md and the coordinated UniXNU documentation update.

Algorithm design, implementation, tests and documentation were prepared with AI assistance and human device feedback. No Linux driver implementation, controller firmware or raw device capture is included. Full stylus support is outside this change.

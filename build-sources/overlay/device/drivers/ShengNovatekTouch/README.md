# ShengNovatekTouch

NT36532 touch protocol and sheng sensor configuration.

The finger tracker confirms a new contact on its second scan and reports the
first scan's position at tip-down. A quiet matrix ends the contact. During a
pinch, established fingers retain separate resolvable peaks. The host tests
replay bottom-edge swipes, rapid taps and four- and five-finger pinch-and-hold.

The device repository selects this component with its board inputs.
Build requirements and scope are documented in the [UniXNU site](https://unixnu.com/drivers/sheng-novatek-touch)
and [component catalog](https://unixnu.com/reference/components).

## Typed observations and timed finger tracking

THP data type 0x1d uses a special P/stylus format and is separated from full-M decoding. Its actual current mutual rows may provide either a complete local blob or a qualified masked partial observation of an existing contact. Unseen rows are UNKNOWN, not zeros or proof of release. Pen fields never enter the ordinary finger matrix; this is not full stylus support.

The timed API preserves actual observation intervals, generation-bound contact lifetimes, bounded prediction with last-position fallback, qualified weak-point continuation and separate position/presence evidence. Legacy nvt_touch_process remains available. The partial observer takes explicit configuration and an immutable shape model; the board's numeric profile is experimental rather than controller calibration.

The sheng consumer wires this API to its Engine and ordered report Timeline. Compile NvtPartialObservation.c and NvtCoverage.c and include their objects in the final input archive. Run the component regressions and the coordinated host fixtures; recorded host/native/device results have separate scopes.

# ShengHidInput

sheng touch input through IOHIDDevice.

used by the sheng touch path.

Build and interface details: [documentation](https://unixnu.com/drivers/sheng-hid-input). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

The legacy builder is limited to arm64 iOS 17 / IOHIDFamily 2031.120.4.
For arm64e source-interface compilation, use UniXNU's
`scripts/build-sheng-source-drivers.py`. It selects `IOHIDDevice.h`, uses the
report-type enum and omits the legacy virtual-table edge-event patch.
The generated report distinguishes compilation from family linking and device
qualification; HID startup and edge gestures remain pending.
See the [migration procedure](https://unixnu.com/porting/migration#source-drivers).

## Ordered timestamped finger reports

The source-IOHID path consumes the board's coherent contact snapshot, original observation ticks and sequence number. It sends historical positions with handleReportWithTime, explicitly releases vanished contact IDs at their last acknowledged coordinates, and acknowledges a snapshot only after successful report submission. A split transition commits each successful packet independently.
The legacy binary ABI submits through handleReport and uses submission-time timestamps.

Pair NativeHidInput.cpp and NativeHidTouchPlan.h with the board timestamp/ACK producer and Timeline; replacing only one side does not supply the contract. Report success is an API acknowledgement, not proof that a user-space event consumer handled the event. Internal core slots and the ten-contact report capacity remain distinct limits.

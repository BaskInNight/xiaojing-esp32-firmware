# UV startup observation — 2026-10-31

After the boot-animation duration update, the user reported the actual UV lamp
remaining illuminated after the main UI appeared. A subsequent user-initiated
restart did not reproduce it; the lamp stayed off.

Read-only BLE get_status after that restart returned revision 61:
`state=1, phase=0, program_id=0, uv_on=false, uv_self_test=0, heater_on=false,
fault_code=0, fault_detail=0`.
These fields are software status, not an independent measurement of lamp
current or GPA2 pad voltage. They agree with the user's current observation.

Source review: UV/MCP/self-test driver files have no workspace diff. MCP init
writes OLAT low before IODIR; UV maps to MCP GPA2 with active-high enable;
the UV task requests OFF at entry. uv_self_test_start only enables the service,
and requests must pass the existing gates to energize UV. Recent audio/display
changes did not add an automatic UV request. Startup timing was changed, so
source review alone does not exclude an interaction; no cause is established.

No UV polarity, defaults, safety gates, firmware, or configuration were changed
in response to this observation. Unexpected illumination remains unexplained.
If it recurs, isolate UV lamp power first and preserve ESP state for read-only
status/serial collection. Record timing and compare software state with physical
output before changing polarity or conducting any controlled hardware test.

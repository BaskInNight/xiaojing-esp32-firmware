# Phase 9 Round 2.1 — BLE Advertising Investigation Status

## BLE_ERR_MEM_CAPACITY (0x207) Root Cause Analysis

### Evidence
```
adv_set_fields rc=0          ← NimBLE host encodes data OK (9 bytes)
BLE_ERR_MEM_CAPACITY (0x207) ← Controller rejects HCI LE Set Advertising Data
```

### Heap Profile
```
boot_start:           int_free=175,035  int_largest=110,592
before_ble:           int_free=114,707  int_largest=57,344
post_nimble_port_init: int_free=58,679  int_largest=31,744
pre_adv_start:        int_free=54,151   int_largest=31,744
```

### Analysis
1. **NimBLE host allocates from PSRAM** (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`)
2. **BLE controller allocates from internal RAM** (binary blob, not configurable)
3. **Controller's internal pool**: ~56KB consumed by `nimble_port_init()`
4. **Advertising data**: 9 bytes (flags + UUID16) — well under 31-byte limit
5. **Error at HCI layer**: `ocf=0x000a` (LE Set Advertising Data)

### Hypothesis
The BLE controller's internal memory pool (allocated by `esp_bt_controller_init()`) is fragmented or undersized for the advertising buffer allocation. The controller uses its own memory allocator separate from the system heap.

### What We Tried
| Change | Result |
|--------|--------|
| MSYS 1-block 6→12 | Still fails |
| MSYS 2-block 6→12 | Still fails |
| ACL count 4→8 | Still fails |
| HCI event count 8→12 | Still fails |
| Disable Central/Observer roles | Still fails |
| Reduce BLE_MAX_ACT to 1 | Still fails |
| Split adv/scan_rsp data | Still fails |
| BLE init before WiFi | Controller init OK, adv fails |

### What Works
- BLE controller initializes (MAC assigned)
- NimBLE host starts
- GATT services registered
- BLE transport starts (non-fatal adv failure)
- All voice components work
- System boots and runs stably

### What Doesn't Work
- BLE advertising (device not visible to phones)
- BLE connections (no advertising = no connections)

## Pending Investigation
1. Check if `CONFIG_BT_CTRL_ADV_DUP_FILT_MAX` affects advertising buffer allocation
2. Try `CONFIG_BT_CTRL_DTM_ENABLE=n` to reduce controller memory
3. Check if `CONFIG_BT_CTRL_LE_PING_EN=n` saves memory
4. Try `CONFIG_BT_CTRL_CHAN_ASS_EN=n` to reduce controller features
5. Check ESP-IDF BLE controller Kconfig for memory-related options
6. Consider custom partition table with larger app partition for more controller memory

## Current Gate Status
- BLE_ADVERTISING: FAIL
- BLE_CONTROLLER_INIT: PASS
- BLE_TRANSPORT_START: PASS (non-fatal)
- EDGE_MODEL_PRESENT: PASS
- VOICE_FRONTEND: PASS
- WIFI: FAIL (buffer allocation)

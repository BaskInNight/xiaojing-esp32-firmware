from pathlib import Path
from saleae import automation


DEVICE_ID = "808A9FACF5F8E9BD"
OUT = Path(__file__).resolve().parents[1] / "diagnostics" / "saleae_i2s"
OUT.mkdir(parents=True, exist_ok=True)

with automation.Manager.connect(port=10430, connect_timeout_seconds=10) as manager:
    device = automation.LogicDeviceConfiguration(
        enabled_digital_channels=[0, 1, 2, 3, 4],
        digital_sample_rate=24_000_000,
    )
    trigger = automation.CaptureConfiguration(
        capture_mode=automation.DigitalTriggerCaptureMode(
            # Trigger when the audio peripheral starts producing BCLK after
            # EN/reset, then retain enough time to include the boot tone.
            trigger_type=automation.DigitalTriggerType.RISING,
            trigger_channel_index=0,
            after_trigger_seconds=6.0,
        )
    )
    with manager.start_capture(
        device_id=DEVICE_ID,
        device_configuration=device,
        capture_configuration=trigger,
    ) as capture:
        print("CAPTURE_ARMED: press ESP32 EN now", flush=True)
        capture.wait()
        print("CAPTURE_COMPLETE", flush=True)
        capture.save_capture(filepath=str(OUT / "max98357_i2s.sal"))
        capture.export_raw_data_csv(
            directory=str(OUT), digital_channels=[0, 1, 2, 3, 4]
        )
        print(f"EXPORTED: {OUT}", flush=True)

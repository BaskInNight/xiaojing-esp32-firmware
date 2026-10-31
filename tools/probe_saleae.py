from saleae import automation
with automation.Manager.connect(port=10430, connect_timeout_seconds=10) as m:
    info=m.get_app_info()
    print('APP',info.app_version,'API',info.api_version,'PID',info.app_pid)
    ds=m.get_devices(include_simulation_devices=False)
    print('DEVICES',len(ds))
    for d in ds: print('DEVICE',d.device_id,d.device_type,d.is_simulation)

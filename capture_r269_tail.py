import serial,time
ser=serial.Serial('COM8',115200,timeout=.25)
start=time.time(); out='phase9_r269_20cycle_tail.log'
with open(out,'w',encoding='utf-8',errors='replace') as f:
    while time.time()-start<180:
        b=ser.read(4096)
        if b:
            f.write(b.decode('utf-8','replace')); f.flush()
ser.close(); print('CAPTURE_SECONDS',round(time.time()-start,1))

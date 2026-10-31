import serial,time
p='COM8'; out='phase9_r268_autotest.log'
ser=serial.Serial(p,115200,timeout=0.25)
start=time.time();
with open(out,'w',encoding='utf-8',errors='replace') as f:
    while time.time()-start<85:
        b=ser.read(4096)
        if b:
            s=b.decode('utf-8','replace'); f.write(s); f.flush()
ser.close()
print('CAPTURE_SECONDS',round(time.time()-start,1))

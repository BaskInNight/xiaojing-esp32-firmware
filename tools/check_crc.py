def sht_crc8(data):
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x31) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

buf = [0x66, 0x66, 0x9C, 0x7F, 0xFF, 0xAC]
temp_crc = sht_crc8(bytes(buf[0:2]))
hum_crc = sht_crc8(bytes(buf[3:5]))
print(f"Temp CRC: expected=0x{buf[2]:02X} computed=0x{temp_crc:02X} match={buf[2]==temp_crc}")
print(f"Hum CRC: expected=0x{buf[5]:02X} computed=0x{hum_crc:02X} match={buf[5]==hum_crc}")

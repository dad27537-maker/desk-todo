"""Carve CRC-validated embedded 7z archives without executing the installer."""
from pathlib import Path
import hashlib, json, struct, sys, zlib

source = Path(sys.argv[1] if len(sys.argv)>1 else r'E:\download\DeskGo_4_3_1832_127_full_S20000.exe')
out = Path(__file__).resolve().parent
data = source.read_bytes()
magic = b"7z\xbc\xaf\x27\x1c"
entries = []
start = 0
while True:
    offset = data.find(magic, start)
    if offset < 0: break
    start = offset+1
    header = data[offset:offset+32]
    if len(header)!=32: continue
    crc = struct.unpack_from('<I',header,8)[0]
    if (zlib.crc32(header[12:32])&0xffffffff)!=crc: continue
    next_offset,next_size,next_crc = struct.unpack_from('<QQI',header,12)
    end = offset+32+next_offset+next_size
    if end>len(data): continue
    if (zlib.crc32(data[offset+32+next_offset:end])&0xffffffff)!=next_crc: continue
    target = out/f'payload-{offset}.7z'
    payload = data[offset:end]
    target.write_bytes(payload)
    entries.append({'offset':offset,'end':end,'bytes':len(payload),'start_valid':True,'next_valid':True,
                    'sha256':hashlib.sha256(payload).hexdigest(),'path':target.name})
result={'source':str(source),'source_sha256':hashlib.sha256(data).hexdigest(),'archives':entries}
(out/'archive-offsets.json').write_text(json.dumps(result,indent=2),encoding='utf8')
print(json.dumps(result,indent=2))

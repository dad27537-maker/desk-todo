from pathlib import Path
import struct, json, re, bisect, hashlib, csv

root = Path(__file__).resolve().parent
binary = root / 'extracted/Install_BinFinal/Features64.dll'
b = binary.read_bytes()
pe = struct.unpack_from('<I', b, 60)[0]
opt = pe + 24
base = struct.unpack_from('<Q', b, opt + 24)[0]
sects = []
for i in range(struct.unpack_from('<H', b, pe + 6)[0]):
    n, vs, va, rs, off = struct.unpack_from('<8sIIII', b, opt + struct.unpack_from('<H', b, pe + 20)[0] + 40*i)
    sects.append((n.rstrip(b'\0').decode(), va, vs, off, rs))

def rvaoff(rva):
    for n, va, vs, off, rs in sects:
        if va <= rva < va + rs: return off + rva - va
    raise ValueError(hex(rva))

def offva(offset):
    for n, va, vs, off, rs in sects:
        if off <= offset < off+rs: return base + va + offset-off
    return None

prva, plen = struct.unpack_from('<II', b, opt + 112 + 3*8)
poff = rvaoff(prva)
funcs = []
for o in range(poff, poff+plen, 12):
    start, end, unwind = struct.unpack_from('<III', b, o)
    if start: funcs.append((base+start,base+end,unwind))
starts = [f[0] for f in funcs]

def containing(va):
    i = bisect.bisect_right(starts, va)-1
    if i>=0 and funcs[i][0]<=va<funcs[i][1]: return funcs[i][0]
    return None

imports = json.loads((root/'imports.json').read_text())['Features64.dll']
iat = {base+f['iat_rva']: d['dll']+'!'+str(f['name'] or '#'+str(f['ordinal']))
       for d in imports['imports'] for f in d['functions']}
strings = [{'offset':m.start(),'text':m.group().decode('ascii')}
           for m in re.finditer(rb'[\x20-\x7e]{6,}', b)]
seeds = {}
for s in strings:
    if (s['text'].startswith(('CDeskDropHelper::', 'CDeskOleDropTarget::', 'CDropHandler::', 'CDesktopPropertyMonitor::'))):
        va = offva(s['offset'])
        if va: seeds[va] = s
for m in re.finditer(rb'(?:[\x20-\x7e]\0){6,}', b):
    text = m.group().decode('utf-16le')
    if text.startswith(('CDeskDropHelper::', 'CMultiScreenHelper::', 'Frame::CBoxMultScreenLayoutMgr::', 'CDeskDesk::MoveItem', 'Frame::CBaseBoxContainer::ResetBoxAttachScreenInfo', 'Frame::CBaseBoxContainer::GetScreenReferenceSize', 'Frame::CDeskFrameWndUI::DragBox')):
        va = offva(m.start())
        if va: seeds[va] = {'offset': m.start(), 'text': text, 'encoding': 'utf-16le'}

ins = {}; refs = []; calls = []; perfunc = {}
linepat = re.compile(r'^\s*([0-9A-F]{16}):\s+(\w+)\s*(.*)$')
with (root/'Features64.disasm.txt').open() as stream:
    for lineno,line in enumerate(stream,1):
        m=linepat.match(line)
        if not m: continue
        addr=int(m[1],16); op=m[2]; operands=m[3]; fn=containing(addr)
        if not fn: continue
        # Keep instruction text only for selected functions after seed pass.
        perfunc.setdefault(fn,[]).append((addr,op,operands,lineno))
        for r in re.findall(r'\[([0-9A-F]{16})h\]',operands):
            target=int(r,16)
            if target in seeds:refs.append({'instruction':addr,'function':fn,'target':target,'text':seeds[target]['text'],'line':lineno})
        if op in ('call','jmp'):
            direct=re.fullmatch(r'([0-9A-F]{16})',operands)
            indirect=re.fullmatch(r'qword ptr \[([0-9A-F]{16})h\]',operands)
            if direct:
                target=int(direct[1],16)
                callee = containing(target)
                if callee != fn: calls.append({'instruction':addr,'caller':fn,'target':target,'callee':callee,'kind':op})
            elif indirect and int(indirect[1],16) in iat:
                calls.append({'instruction':addr,'caller':fn,'target':int(indirect[1],16),'import':iat[int(indirect[1],16)],'kind':op})

selected={r['function'] for r in refs}
labels={}
for r in refs:
    labels.setdefault(r['function'],[])
    if r['text'] not in labels[r['function']]:labels[r['function']].append(r['text'])

result={'binary':str(binary),'sha256':hashlib.sha256(b).hexdigest(),'image_base':hex(base),
        'procedure_boundary_source':'PE exception directory RUNTIME_FUNCTION records (.pdata)',
        'instruction_source':'Microsoft dumpbin /disasm:nobytes',
        'limitations':['Only static direct call/jump edges and explicit IAT call/jump instructions are resolved.',
                       'Log strings identify containing functions by static cross references; labels are inferred names, not recovered debug symbols.',
                       'No execution, timing, hotplug replay or decompilation was performed.',
                       'Indirect virtual calls, callbacks and leaf functions without unwind records remain incomplete.'],
        'functions':[]}
for f in sorted(selected):
    info={'address':hex(f),'end':hex(funcs[starts.index(f)][1]),'labels':labels[f],
          'string_xrefs':[r for r in refs if r['function']==f],
          'callees':[c for c in calls if c['caller']==f],
          'callers':[c for c in calls if c.get('callee')==f]}
    result['functions'].append(info)
(root/'focused-call-graph.json').write_text(json.dumps(result,indent=2),encoding='utf8')
with (root/'functions.csv').open('w',newline='',encoding='utf-8-sig') as stream:
    writer=csv.writer(stream)
    writer.writerow(['start_va','end_va','log_derived_names','direct_named_callees','direct_imports'])
    for function in result['functions']:
        writer.writerow([function['address'],function['end'],' | '.join(function['labels']),
            '; '.join(sorted(set(hex(c['callee']) for c in function['callees'] if c.get('callee') in labels))),
            '; '.join(sorted(set(c['import'] for c in function['callees'] if c.get('import'))))])
with (root/'named-call-edges.csv').open('w',newline='',encoding='utf-8-sig') as stream:
    writer=csv.writer(stream)
    writer.writerow(['caller_va','instruction_va','kind','callee_va','caller_names','callee_names'])
    for function in result['functions']:
        for call in function['callees']:
            if call.get('callee') in labels:
                writer.writerow([function['address'],hex(call['instruction']),call['kind'],hex(call['callee']),
                                 ' | '.join(function['labels']),' | '.join(labels[call['callee']])])
import_by_instruction = {c['instruction']:c['import'] for c in calls if c.get('import')}
with (root/'focused-functions.asm.txt').open('w') as out:
    for f in sorted(selected):
        out.write('\n; '+hex(f)+' '+ ' | '.join(labels[f])+'\n')
        for a,op,args,line in perfunc.get(f,[]):
            comment=' ; '+import_by_instruction[a] if a in import_by_instruction else ''
            out.write(f'{a:016X}: {op:12} {args}{comment}\n')
print('Runtime functions:',len(funcs),'selected:',len(selected),'edges:',len(calls))
for f in result['functions']:
    print(f['address'],f['end'],' | '.join(f['labels'])[:160])

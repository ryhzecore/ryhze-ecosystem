"""Validate the bounded viewport ABI from compiled DXIL reflection using pinned DXC."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import struct


MANIFEST_BYTES=640
SHADER_MAX_BYTES=4*1024*1024
FLAGS='-HV 2021 -WX -Ges -O3'
BINDINGS=[(1,0,0,1,3,1,176,1),(2,0,0,1,2,1,0,1),(3,0,0,1,2,1,0,1)]


def compiler_identity(dxc, lock_path):
    lock=json.loads(lock_path.read_text(encoding='utf-8-sig'))
    pinned={Path(item['path']).name.lower():item['sha256'] for item in lock['installed']}
    records=[]
    for name in ['dxc.exe','dxcompiler.dll','dxil.dll']:
        path=dxc if name=='dxc.exe' else dxc.with_name(name)
        value=hashlib.sha256(path.read_bytes()).hexdigest()
        if pinned.get(name)!=value:
            raise RuntimeError(f'compiler identity differs from lock: {name}')
        records.append({'file':name,'sha256':value})
    return records


def binary_manifest(records, compiler, source):
    # Independent fixed little-endian writer. All unused bytes start at zero.
    out=bytearray(MANIFEST_BYTES)
    out[:8]=b'RACESHD1'
    struct.pack_into('<11I',out,8,1,64,640,1,3,2,2021,1,SHADER_MAX_BYTES,3,176)
    identities=[item['sha256'] for item in compiler]+[source]
    for index,digest in enumerate(identities):
        out[64+index*32:96+index*32]=bytes.fromhex(digest)
    for index,record in enumerate(records):
        at=192+index*128
        struct.pack_into('<IIQ',out,at,1 if index==0 else 2,1,record['bytes'])
        out[at+16:at+48]=bytes.fromhex(record['sha256'])
        for offset,value in [(48,record['entry']),(64,record['target'])]:
            encoded=value.encode('ascii')
            if len(encoded)>=16:
                raise ValueError('stage field exceeds manifest bound')
            out[at+offset:at+offset+len(encoded)]=encoded
        struct.pack_into('<7I',out,at+80,176,2,0,64,128,144,160)
    for index,binding in enumerate(BINDINGS):
        struct.pack_into('<8I',out,448+index*32,*binding)
    out[544:544+len(FLAGS)]=FLAGS.encode('ascii')
    out[608:640]=hashlib.sha256(out[:608]).digest()
    return bytes(out)


def identity_header(compiler,source,records):
    identities=[item['sha256'] for item in compiler]+[source]+[item['sha256'] for item in records]
    values=[]
    for digest in identities:
        values.append('  {'+', '.join('std::byte{0x'+digest[i:i+2]+'}' for i in range(0,64,2))+'}')
    return ('#pragma once\n#include "race/render/resource_contract.hpp"\n'
            'namespace race::render {\n'
            'inline constexpr ViewportShaderIdentity compiled_viewport_shader_identity{\n'+
            ',\n'.join(values)+'\n};\n} // namespace race::render\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc',type=Path,required=True)
    parser.add_argument('--shaders',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--manifest',type=Path)
    parser.add_argument('--header',type=Path)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'shaders/viewport.hlsl')
    parser.add_argument('--lock',type=Path,default=Path(__file__).resolve().with_name('desktop_dependencies.lock.json'))
    args=parser.parse_args()
    compiler=compiler_identity(args.dxc,args.lock)
    source=hashlib.sha256(args.source.read_bytes()).hexdigest()
    records=[]
    for stage in ['vs','ps']:
        path=args.shaders/f'viewport.{stage}.dxil'
        shader=path.read_bytes()
        if not 32<=len(shader)<=SHADER_MAX_BYTES or shader[:4]!=b'DXBC':
            raise RuntimeError(f'{stage}: invalid bounded DXIL container')
        result=subprocess.run([str(args.dxc),'-dumpbin',str(path)],capture_output=True,text=True,timeout=30,check=True)
        text=result.stdout
        entry='VSMain' if stage=='vs' else 'PSMain'
        if not re.search(r'!\{!"'+stage+r'",\s*i32 6,\s*i32 0\}',text):
            raise RuntimeError(f'{stage}: shader model does not match admitted profile')
        if not re.search(r'\bdefine\s+void\s+@'+entry+r'\s*\(',text):
            raise RuntimeError(f'{stage}: shader entry does not match admitted profile')
        for name,offset in [('modelViewProjection',0),('normalTransform',64),('baseColor',128),('illumination',144),('parameters',160)]:
            if not re.search(r'\b'+name+r';\s*; Offset:\s*'+str(offset)+r'\b',text):
                raise RuntimeError(f'{stage}: DrawConstants.{name} ABI mismatch')
        if not re.search(r'} DrawConstants;\s*; Offset:\s*0 Size:\s*176\b',text):
            raise RuntimeError(f'{stage}: root constants size mismatch')
        start=text.index('; Resource Bindings:')
        section=text[start:text.find('target datalayout',start)]
        bindings=[]
        for line in section.splitlines():
            match=re.match(r';\s*(\w+)\s+(cbuffer|sampler|texture)\s+\S+\s+\S+\s+\S+\s+(\w+)\s+(\d+)\s*$',line)
            if match:
                bindings.append(match.groups())
        expected=[('DrawConstants','cbuffer','cb0','1')]
        if stage=='ps':
            expected += [('colorSampler','sampler','s0','1'),('colorTexture','texture','t0','1')]
        if sorted(bindings)!=sorted(expected):
            raise RuntimeError(f'{stage}: binding profile mismatch {bindings}')
        if not re.search(r';\s*TEXCOORD\s+0\s+xy\s+\d+\s+NONE\s+float\s+xy\s*$',text,re.M):
            raise RuntimeError(f'{stage}: UV interface missing')
        if path.read_bytes()!=shader:
            raise RuntimeError(f'{stage}: shader changed during reflection')
        records.append({'stage':stage,'file':path.name,'sha256':hashlib.sha256(shader).hexdigest(),
                        'bytes':len(shader),'entry':entry,'target':stage+'_6_0',
                        'bindings':[{'name':b[0],'type':b[1],'register':b[2],'count':int(b[3])} for b in bindings],
                        'draw_constants_bytes':176,'uv':'float2 TEXCOORD0'})
    if compiler_identity(args.dxc,args.lock)!=compiler or hashlib.sha256(args.source.read_bytes()).hexdigest()!=source:
        raise RuntimeError('compiler or source changed during reflection')
    manifest=binary_manifest(records,compiler,source)
    manifest_path=args.manifest or args.output.with_suffix('.rmanifest')
    receipt={'schema':2,'profile':'race-viewport-opaque-diffuse-1','passed':True,
             'compiler_sha256':compiler[0]['sha256'],'compiler':compiler,'source_sha256':source,
             'hlsl':2021,'flags':FLAGS.split(),'stages':records,
             'sampled_formats':['rgba8-srgb','rgba8-linear-data'],
             'manifest':{'file':manifest_path.name,'bytes':len(manifest),'sha256':hashlib.sha256(manifest).hexdigest()}}
    manifest_path.parent.mkdir(parents=True,exist_ok=True)
    manifest_path.write_bytes(manifest)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    if args.header:
        args.header.parent.mkdir(parents=True,exist_ok=True)
        args.header.write_bytes(identity_header(compiler,source,records).replace('\n','\r\n').encode('utf-8'))
    print('viewport DXIL reflection: pinned compiler/source, b0/t0/s0, constants and UV ABI passed')


if __name__=='__main__':
    main()

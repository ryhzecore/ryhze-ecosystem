"""Validate the P03.04 light/output shader ABI from compiled DXIL reflection using pinned DXC.

Sibling of check_viewport_bindings.py (imported, never modified): the legacy
viewport program and its RACESHD1 manifest stay byte-identical. This writes the
RACESHD2 manifest that links the physical pixel stage to the admitted legacy
vertex stage by digest and reflects the fullscreen output program's own ABI.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from check_viewport_bindings import FLAGS, SHADER_MAX_BYTES, compiler_identity


MANIFEST_BYTES=896
PHYSICAL_BINDINGS=[(1,0,0,1,3,1,176,1),(2,0,0,1,2,1,0,1),(3,0,0,1,2,1,0,1)]
OUTPUT_BINDINGS=[(1,0,0,1,2,1,32,1),(2,0,0,1,2,1,0,1)]
STAGES=[
    {'file':'viewport-physical.ps.dxil','stage':'ps','program':0,'entry':'PSPhysical','cbuffer':'DrawConstants','constants':176,
     'members':[('modelViewProjection',0),('normalTransform',64),('baseColor',128),('illumination',144),('parameters',160)],
     'bindings':[('DrawConstants','cbuffer','cb0','1'),('colorSampler','sampler','s0','1'),('colorTexture','texture','t0','1')],
     'interface':r';\s*TEXCOORD\s+0\s+xy\s+\d+\s+NONE\s+float\s+xy\s*$','load_only':False},
    {'file':'viewport-output.vs.dxil','stage':'vs','program':1,'entry':'VSOutput','cbuffer':None,'constants':0,'members':[],
     'bindings':[],'interface':r';\s*SV_VertexID\s+0\s+x\s+\d+\s+VERTID\s+uint\s+x\s*$','load_only':False},
    {'file':'viewport-output.ps.dxil','stage':'ps','program':1,'entry':'PSOutput','cbuffer':'OutputConstants','constants':32,
     'members':[('exposure',0),('reserved',16)],
     'bindings':[('OutputConstants','cbuffer','cb0','1'),('sceneLinear','texture','t0','1')],
     'interface':r';\s*SV_Position\s+0\s+xyzw\s+\d+\s+POS\s+float\s+\w*\s*$','load_only':True},
]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def binary_manifest(records, compiler, sources, legacy_vertex):
    # Independent fixed little-endian writer. All unused bytes start at zero.
    out=bytearray(MANIFEST_BYTES)
    out[:8]=b'RACESHD2'
    struct.pack_into('<11I',out,8,2,64,MANIFEST_BYTES,2,5,3,2021,1,SHADER_MAX_BYTES,6,176)
    identities=[item['sha256'] for item in compiler]+sources+[legacy_vertex]
    for index,value in enumerate(identities):
        out[64+index*32:96+index*32]=bytes.fromhex(value)
    for index,record in enumerate(records):
        at=256+index*128
        struct.pack_into('<IIQ',out,at,1 if record['stage']=='vs' else 2,record['program'],record['bytes'])
        out[at+16:at+48]=bytes.fromhex(record['sha256'])
        for offset,value in [(48,record['entry']),(64,record['target'])]:
            encoded=value.encode('ascii')
            if len(encoded)>=16:
                raise ValueError('stage field exceeds manifest bound')
            out[at+offset:at+offset+len(encoded)]=encoded
        offsets=[offset for _,offset in record['members']]+[0]*(5-len(record['members']))
        struct.pack_into('<7I',out,at+80,record['constants_bytes'],len(record['bindings']),*offsets)
    for index,binding in enumerate(PHYSICAL_BINDINGS+OUTPUT_BINDINGS):
        struct.pack_into('<8I',out,640+index*32,*binding)
    out[800:800+len(FLAGS)]=FLAGS.encode('ascii')
    out[864:896]=hashlib.sha256(out[:864]).digest()
    return bytes(out)


def identity_header(compiler,sources,legacy_vertex,records):
    identities=[item['sha256'] for item in compiler]+sources+[legacy_vertex]+[item['sha256'] for item in records]
    values=[]
    for value in identities:
        values.append('  {'+', '.join('std::byte{0x'+value[i:i+2]+'}' for i in range(0,64,2))+'}')
    return ('#pragma once\n#include "race/render/resource_contract.hpp"\n'
            'namespace race::render {\n'
            'inline constexpr LightOutputShaderIdentity compiled_light_output_shader_identity{\n'+
            ',\n'.join(values)+'\n};\n} // namespace race::render\n')


def reflect(dxc,path,spec):
    shader=path.read_bytes()
    if not 32<=len(shader)<=SHADER_MAX_BYTES or shader[:4]!=b'DXBC':
        raise RuntimeError(f"{spec['file']}: invalid bounded DXIL container")
    text=subprocess.run([str(dxc),'-dumpbin',str(path)],capture_output=True,text=True,timeout=30,check=True).stdout
    stage=spec['stage']
    if not re.search(r'!\{!"'+stage+r'",\s*i32 6,\s*i32 0\}',text):
        raise RuntimeError(f"{spec['file']}: shader model does not match admitted profile")
    if not re.search(r'\bdefine\s+void\s+@'+spec['entry']+r'\s*\(',text):
        raise RuntimeError(f"{spec['file']}: shader entry does not match admitted profile")
    if spec['cbuffer']:
        for name,offset in spec['members']:
            if not re.search(r'\b'+name+r';\s*; Offset:\s*'+str(offset)+r'\b',text):
                raise RuntimeError(f"{spec['file']}: {spec['cbuffer']}.{name} ABI mismatch")
        if not re.search(r'} '+spec['cbuffer']+r';\s*; Offset:\s*0 Size:\s*'+str(spec['constants'])+r'\b',text):
            raise RuntimeError(f"{spec['file']}: root constants size mismatch")
    bindings=[]
    start=text.find('; Resource Bindings:')
    if start>=0:
        section=text[start:text.find('target datalayout',start)]
        for line in section.splitlines():
            match=re.match(r';\s*(\w+)\s+(cbuffer|sampler|texture)\s+\S+\s+\S+\s+\S+\s+(\w+)\s+(\d+)\s*$',line)
            if match:
                bindings.append(match.groups())
    if sorted(bindings)!=sorted(spec['bindings']):
        raise RuntimeError(f"{spec['file']}: binding profile mismatch {bindings}")
    if not re.search(spec['interface'],text,re.M):
        raise RuntimeError(f"{spec['file']}: stage interface missing")
    if spec['load_only']:
        # The output transform reads exact pixels: Load only, no sampler, no derivatives.
        if not re.search(r'dx\.op\.textureLoad',text) or re.search(r'dx\.op\.sample',text):
            raise RuntimeError(f"{spec['file']}: output transform must read the HDR target by Load only")
    if path.read_bytes()!=shader:
        raise RuntimeError(f"{spec['file']}: shader changed during reflection")
    return {'stage':stage,'program':spec['program'],'file':path.name,'sha256':digest(shader),'bytes':len(shader),
            'entry':spec['entry'],'target':stage+'_6_0','members':spec['members'],
            'bindings':[{'name':b[0],'type':b[1],'register':b[2],'count':int(b[3])} for b in bindings],
            'constants_bytes':spec['constants'],'constants':spec['cbuffer']}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc',type=Path,required=True)
    parser.add_argument('--shaders',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--manifest',type=Path)
    parser.add_argument('--header',type=Path)
    parser.add_argument('--physical-source',type=Path,default=Path(__file__).resolve().parents[1]/'shaders/viewport_physical.hlsl')
    parser.add_argument('--output-source',type=Path,default=Path(__file__).resolve().parents[1]/'shaders/viewport_output.hlsl')
    parser.add_argument('--lock',type=Path,default=Path(__file__).resolve().with_name('desktop_dependencies.lock.json'))
    args=parser.parse_args()
    compiler=compiler_identity(args.dxc,args.lock)
    sources=[digest(args.physical_source.read_bytes()),digest(args.output_source.read_bytes())]
    legacy=(args.shaders/'viewport.vs.dxil').read_bytes()
    if not 32<=len(legacy)<=SHADER_MAX_BYTES or legacy[:4]!=b'DXBC':
        raise RuntimeError('legacy vertex stage: invalid bounded DXIL container')
    legacy_vertex=digest(legacy)
    records=[reflect(args.dxc,args.shaders/spec['file'],spec) for spec in STAGES]
    if compiler_identity(args.dxc,args.lock)!=compiler or \
       [digest(args.physical_source.read_bytes()),digest(args.output_source.read_bytes())]!=sources or \
       digest((args.shaders/'viewport.vs.dxil').read_bytes())!=legacy_vertex:
        raise RuntimeError('compiler, sources or legacy vertex stage changed during reflection')
    manifest=binary_manifest(records,compiler,sources,legacy_vertex)
    manifest_path=args.manifest or args.output.with_suffix('.rmanifest')
    receipt={'schema':1,'profile':'race-viewport-light-output-1','passed':True,
             'compiler_sha256':compiler[0]['sha256'],'compiler':compiler,
             'physical_source_sha256':sources[0],'output_source_sha256':sources[1],'legacy_vertex_sha256':legacy_vertex,
             'hlsl':2021,'flags':FLAGS.split(),'stages':records,
             'hdr_target_format':'R16G16B16A16_FLOAT','output_read':'Load',
             'manifest':{'file':manifest_path.name,'bytes':len(manifest),'sha256':digest(manifest)}}
    manifest_path.parent.mkdir(parents=True,exist_ok=True)
    manifest_path.write_bytes(manifest)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    if args.header:
        args.header.parent.mkdir(parents=True,exist_ok=True)
        args.header.write_bytes(identity_header(compiler,sources,legacy_vertex,records).replace('\n','\r\n').encode('utf-8'))
    print('light/output DXIL reflection: pinned compiler/sources, legacy vertex link, physical b0/t0/s0, output b0/t0 Load ABI passed')


if __name__=='__main__':
    main()

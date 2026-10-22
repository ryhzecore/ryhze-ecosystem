#!/usr/bin/env python3
"""TEST ONLY: real offscreen Vulkan graphics on explicitly selected CPU SwiftShader.
No shipping backend, hardware claim, shader cache, window system or performance timer.
Uses the installed official Vulkan/shaderc C ABI; native libraries are not sanitizer
instrumented by RACE. Fixture dimensions are bounded to 512x512 RGBA; explicit
image/readback allocations are capped separately. Driver/compiler host allocations
are external overhead, not controlled by the RACE allocator.
"""
from __future__ import annotations
import argparse
import ctypes as C
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import zlib

U=C.c_uint32; I=C.c_int32; Q=C.c_uint64; F=C.c_float; P=C.c_void_p
PTR=C.POINTER

def structure(name, fields, typed=None):
    if typed is not None: fields=[('sType',U),('pNext',P)]+fields
    cls=type(name,(C.Structure,),{'_fields_':fields})
    if typed is not None:
        def init(self, **kw):
            C.Structure.__init__(self, sType=typed, **kw)
        cls.__init__=init
    return cls

App=structure('App',[('name',C.c_char_p),('version',U),('engine',C.c_char_p),('engineVersion',U),('api',U)],0)
InstanceInfo=structure('InstanceInfo',[('flags',U),('app',PTR(App)),('layers',U),('layerNames',PTR(C.c_char_p)),('extensions',U),('extensionNames',PTR(C.c_char_p))],1)
QueueInfo=structure('QueueInfo',[('flags',U),('family',U),('count',U),('priorities',PTR(F))],2)
DeviceInfo=structure('DeviceInfo',[('flags',U),('count',U),('queues',PTR(QueueInfo)),('layers',U),('layerNames',PTR(C.c_char_p)),('extensions',U),('extensionNames',PTR(C.c_char_p)),('features',P)],3)
QueueProps=structure('QueueProps',[('flags',U),('count',U),('timestamp',U),('granularity',U*3)])
MemoryType=structure('MemoryType',[('flags',U),('heap',U)])
MemoryHeap=structure('MemoryHeap',[('size',Q),('flags',U)])
MemoryProps=structure('MemoryProps',[('count',U),('types',MemoryType*32),('heapsCount',U),('heaps',MemoryHeap*16)])
Requirements=structure('Requirements',[('size',Q),('alignment',Q),('bits',U)])
Allocate=structure('Allocate',[('size',Q),('index',U)],5)
Extent=structure('Extent',[('width',U),('height',U),('depth',U)])
ImageInfo=structure('ImageInfo',[('flags',U),('type',U),('format',U),('extent',Extent),('mips',U),('layers',U),('samples',U),('tiling',U),('usage',U),('sharing',U),('familyCount',U),('families',PTR(U)),('layout',U)],14)
Range=structure('Range',[('aspect',U),('baseMip',U),('mips',U),('baseLayer',U),('layers',U)])
ViewInfo=structure('ViewInfo',[('flags',U),('image',P),('type',U),('format',U),('components',U*4),('range',Range)],15)
BufferInfo=structure('BufferInfo',[('flags',U),('size',Q),('usage',U),('sharing',U),('familyCount',U),('families',PTR(U))],12)
Attachment=structure('Attachment',[('flags',U),('format',U),('samples',U),('load',U),('store',U),('stencilLoad',U),('stencilStore',U),('initial',U),('final',U)])
AttachmentRef=structure('AttachmentRef',[('attachment',U),('layout',U)])
Subpass=structure('Subpass',[('flags',U),('bindPoint',U),('inputCount',U),('inputs',PTR(AttachmentRef)),('colorCount',U),('colors',PTR(AttachmentRef)),('resolves',PTR(AttachmentRef)),('depth',PTR(AttachmentRef)),('preserveCount',U),('preserves',PTR(U))])
Dependency=structure('Dependency',[('src',U),('dst',U),('srcStage',U),('dstStage',U),('srcAccess',U),('dstAccess',U),('flags',U)])
PassInfo=structure('PassInfo',[('flags',U),('attachmentCount',U),('attachments',PTR(Attachment)),('subpassCount',U),('subpasses',PTR(Subpass)),('dependencyCount',U),('dependencies',PTR(Dependency))],38)
FrameInfo=structure('FrameInfo',[('flags',U),('renderPass',P),('count',U),('attachments',PTR(P)),('width',U),('height',U),('layers',U)],37)
ShaderInfo=structure('ShaderInfo',[('flags',U),('size',C.c_size_t),('code',PTR(U))],16)
Stage=structure('Stage',[('flags',U),('stage',U),('module',P),('name',C.c_char_p),('specialization',P)],18)
VertexInput=structure('VertexInput',[('flags',U),('bindingCount',U),('bindings',P),('attributeCount',U),('attributes',P)],19)
Assembly=structure('Assembly',[('flags',U),('topology',U),('restart',U)],20)
Viewport=structure('Viewport',[('x',F),('y',F),('width',F),('height',F),('minDepth',F),('maxDepth',F)])
Rect=structure('Rect',[('x',I),('y',I),('width',U),('height',U)])
ViewportInfo=structure('ViewportInfo',[('flags',U),('count',U),('viewports',PTR(Viewport)),('scissorCount',U),('scissors',PTR(Rect))],22)
Raster=structure('Raster',[('flags',U),('clamp',U),('discard',U),('polygon',U),('cull',U),('front',U),('depthBias',U),('constant',F),('clampFactor',F),('slope',F),('lineWidth',F)],23)
Multisample=structure('Multisample',[('flags',U),('samples',U),('sampleShading',U),('minShading',F),('mask',PTR(U)),('alphaCoverage',U),('alphaOne',U)],24)
BlendAttachment=structure('BlendAttachment',[('enable',U),('srcColor',U),('dstColor',U),('colorOp',U),('srcAlpha',U),('dstAlpha',U),('alphaOp',U),('writeMask',U)])
Blend=structure('Blend',[('flags',U),('logicEnable',U),('logic',U),('count',U),('attachments',PTR(BlendAttachment)),('constants',F*4)],26)
PushRange=structure('PushRange',[('stages',U),('offset',U),('size',U)])
LayoutInfo=structure('LayoutInfo',[('flags',U),('sets',U),('setLayouts',PTR(P)),('pushCount',U),('pushRanges',PTR(PushRange))],30)
PipelineInfo=structure('PipelineInfo',[('flags',U),('stageCount',U),('stages',PTR(Stage)),('vertex',PTR(VertexInput)),('assembly',PTR(Assembly)),('tessellation',P),('viewport',PTR(ViewportInfo)),('raster',PTR(Raster)),('multisample',PTR(Multisample)),('depth',P),('blend',PTR(Blend)),('dynamic',P),('layout',P),('renderPass',P),('subpass',U),('base',P),('baseIndex',I)],28)
PoolInfo=structure('PoolInfo',[('flags',U),('family',U)],39)
CommandInfo=structure('CommandInfo',[('pool',P),('level',U),('count',U)],40)
BeginInfo=structure('BeginInfo',[('flags',U),('inheritance',P)],42)
PassBegin=structure('PassBegin',[('renderPass',P),('framebuffer',P),('area',Rect),('clearCount',U),('clears',PTR(F*4))],43)
ImageBarrier=structure('ImageBarrier',[('srcAccess',U),('dstAccess',U),('oldLayout',U),('newLayout',U),('srcFamily',U),('dstFamily',U),('image',P),('range',Range)],45)
BufferBarrier=structure('BufferBarrier',[('srcAccess',U),('dstAccess',U),('srcFamily',U),('dstFamily',U),('buffer',P),('offset',Q),('size',Q)],44)
Layers=structure('Layers',[('aspect',U),('mip',U),('base',U),('count',U)])
Copy=structure('Copy',[('offset',Q),('rowLength',U),('imageHeight',U),('layers',Layers),('imageOffset',I*3),('extent',Extent)])
FenceInfo=structure('FenceInfo',[('flags',U)],8)
Submit=structure('Submit',[('waitCount',U),('waits',PTR(P)),('stages',PTR(U)),('commandCount',U),('commands',PTR(P)),('signalCount',U),('signals',PTR(P))],4)

VERTEX='''#version 450
layout(push_constant) uniform Fixture { vec4 positions[3]; vec4 color; } data;
layout(location=0) out vec4 color;
void main() { gl_Position=data.positions[gl_VertexIndex]; color=data.color; }
'''
FRAGMENT='''#version 450
layout(location=0) in vec4 color;
layout(location=0) out vec4 outputColor;
void main() { outputColor=color; }
'''

def bind(lib,name,result,*args):
    fn=getattr(lib,name);fn.restype=result;fn.argtypes=args;return fn

def checked(result, name):
    if result != 0: raise RuntimeError(f'{name}: VkResult {result}')

def shader(source,kind):
    lib=C.CDLL('libshaderc.so.1')
    compiler=bind(lib,'shaderc_compiler_initialize',P)()
    options=bind(lib,'shaderc_compile_options_initialize',P)()
    result=None
    try:
        if not compiler or not options: raise RuntimeError('shaderc initialization failed')
        bind(lib,'shaderc_compile_options_set_target_env',None,P,I,U)(options,0,1<<22)
        bind(lib,'shaderc_compile_options_set_target_spirv',None,P,U)(options,0x10000)
        data=source.encode()
        result=bind(lib,'shaderc_compile_into_spv',P,P,C.c_char_p,C.c_size_t,I,C.c_char_p,C.c_char_p,P)(compiler,data,len(data),kind,b'fixture',b'main',options)
        if not result: raise RuntimeError('shaderc returned no result')
        status=bind(lib,'shaderc_result_get_compilation_status',I,P)(result)
        if status: raise RuntimeError(bind(lib,'shaderc_result_get_error_message',C.c_char_p,P)(result).decode())
        n=bind(lib,'shaderc_result_get_length',C.c_size_t,P)(result)
        p=bind(lib,'shaderc_result_get_bytes',P,P)(result)
        if n%4 or not 20<=n<=65536: raise RuntimeError('invalid bounded SPIR-V length')
        return C.string_at(p,n)
    finally:
        if result: bind(lib,'shaderc_result_release',None,P)(result)
        if options: bind(lib,'shaderc_compile_options_release',None,P)(options)
        if compiler: bind(lib,'shaderc_compiler_release',None,P)(compiler)

def png(path,width,height,rgba):
    def chunk(tag,data): return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
    rows=b''.join(b'\0'+rgba[y*width*4:(y+1)*width*4] for y in range(height))
    data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')
    path.write_bytes(data)
    return {'filename':path.name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}

BACKGROUND = (8, 12, 20, 255)
PIXEL_TOLERANCE = 1

def validate_input(width,height,positions,color):
    if type(width) is not int or type(height) is not int or not 16<=width<=512 or not 16<=height<=512:
        raise ValueError('fixture dimensions must be integers in [16,512]')
    if len(positions)!=3 or any(len(p)!=4 for p in positions): raise ValueError('exactly three vec4 positions required')
    if any(not math.isfinite(x) or abs(x)>1 for p in positions for x in p) or any(p[2]!=0 or p[3]!=1 for p in positions): raise ValueError('only finite bounded clip-space fixture positions supported')
    if len(color)!=4 or any(not math.isfinite(x) or not 0<=x<=1 for x in color) or color[3]!=1: raise ValueError('opaque UNORM color required')
    if all(abs(round(color[i]*255)-BACKGROUND[i])<=PIXEL_TOLERANCE for i in range(3)):
        raise ValueError('foreground indistinguishable from clear color at pixel tolerance')
    a,b,c=positions
    if abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))<.01: raise ValueError('degenerate triangle')

def verify_pixels(width,height,positions,color,pixels):
    validate_input(width,height,positions,color)
    if len(pixels)!=width*height*4: raise RuntimeError('readback byte count mismatch')
    background=bytes(BACKGROUND);expected=tuple(round(x*255) for x in color)
    def pixel(x,y): return tuple(pixels[(y*width+x)*4:(y*width+x+1)*4])
    # Independent edge-function oracle excludes boundary pixels (raster rules).
    screen=[((p[0]+1)*width/2,(p[1]+1)*height/2) for p in positions]
    inside=outside=0
    for y in range(height):
        for x in range(width):
            edges=[]
            for j in range(3):
                a,b=screen[j],screen[(j+1)%3]
                edges.append(((b[0]-a[0])*(y+.5-a[1])-(b[1]-a[1])*(x+.5-a[0]))/math.hypot(b[0]-a[0],b[1]-a[1]))
            if min(edges)>1 or max(edges)<-1:
                if any(abs(a-b)>PIXEL_TOLERANCE for a,b in zip(pixel(x,y),expected)): raise RuntimeError('interior triangle pixel mismatch')
                inside+=1
            elif min(edges)<-1 and max(edges)>1:
                if pixel(x,y)!=tuple(background): raise RuntimeError('background pixel mismatch')
                outside+=1
    if inside<8 or outside<8: raise RuntimeError('insufficient triangle/background coverage')
    return {'interior_pixels_checked':inside,'background_pixels_checked':outside,'corner_rgba':pixel(0,0),'center_rgba':pixel(width//2,height//2)}

def draw(width,height,positions,color,output,receipt=None):
    validate_input(width,height,positions,color)
    # Caller selects this process's known installed ICD. Never alter machine config.
    lib=C.CDLL('libvulkan.so.1')
    instance=P();device=P();memories=[];destroy=[];pending=False
    if receipt is None: receipt={}
    receipt.update({'kind':'test-only-software-vulkan-triangle','width':width,'height':height,'hardware_accelerated':False,'validation_layers_enabled':False,'sanitizer_instrumented':False,'calls':[]})
    def call(name,result,*types): return bind(lib,name,result,*types)
    def create(name,info):
        handle=P();checked(call('vkCreate'+name,I,P,P,P,PTR(P))(device,C.byref(info),None,C.byref(handle)),'vkCreate'+name)
        destroy.append((name,handle));return handle
    try:
        app=App(name=b'RACE test-only fixture',engine=b'RACE',api=1<<22)
        ii=InstanceInfo(app=C.pointer(app))
        checked(call('vkCreateInstance',I,PTR(InstanceInfo),P,PTR(P))(C.byref(ii),None,C.byref(instance)),'vkCreateInstance')
        count=U();enum=call('vkEnumeratePhysicalDevices',I,P,PTR(U),PTR(P))
        checked(enum(instance,C.byref(count),None),'enumerate devices count')
        if not 1<=count.value<=16: raise RuntimeError('no bounded physical device list')
        devs=(P*count.value)();checked(enum(instance,C.byref(count),devs),'enumerate devices')
        physical=devs[0];props=(Q*512)();call('vkGetPhysicalDeviceProperties',None,P,P)(physical,props)
        raw=bytes(props);api,driver,vendor,identity,kind=struct.unpack_from('5I',raw)
        receipt['device']={'name':raw[20:276].split(b'\0',1)[0].decode(),'type':kind,'api_version_raw':api,'driver_version_raw':driver,'vendor':vendor,'id':identity}
        if kind!=4 or 'SwiftShader' not in receipt['device']['name']: raise RuntimeError('adapter requires explicit CPU SwiftShader, not an arbitrary device')
        count=U();qf=call('vkGetPhysicalDeviceQueueFamilyProperties',None,P,PTR(U),PTR(QueueProps));qf(physical,C.byref(count),None)
        if not 1<=count.value<=64: raise RuntimeError('invalid queue family count')
        queues=(QueueProps*count.value)();qf(physical,C.byref(count),queues)
        family=next((i for i,q in enumerate(queues) if q.flags&1 and q.count),None)
        if family is None: raise RuntimeError('no graphics queue')
        priority=F(1);qi=QueueInfo(family=family,count=1,priorities=C.pointer(priority));di=DeviceInfo(count=1,queues=C.pointer(qi))
        checked(call('vkCreateDevice',I,P,PTR(DeviceInfo),P,PTR(P))(physical,C.byref(di),None,C.byref(device)),'vkCreateDevice')
        queue=P();call('vkGetDeviceQueue',None,P,U,U,PTR(P))(device,family,0,C.byref(queue))
        mp=MemoryProps();call('vkGetPhysicalDeviceMemoryProperties',None,P,PTR(MemoryProps))(physical,C.byref(mp))
        def allocate(handle,kind,flags):
            req=Requirements();call('vkGet'+kind+'MemoryRequirements',None,P,P,PTR(Requirements))(device,handle,C.byref(req))
            index=next((i for i in range(mp.count) if req.bits&(1<<i) and mp.types[i].flags&flags==flags),None)
            if index is None: raise RuntimeError('required coherent memory unavailable')
            if req.size>8*1024*1024: raise RuntimeError('fixture allocation exceeds 8 MiB cap')
            info=Allocate(size=req.size,index=index);memory=P()
            checked(call('vkAllocateMemory',I,P,PTR(Allocate),P,PTR(P))(device,C.byref(info),None,C.byref(memory)),'vkAllocateMemory')
            memories.append(memory);checked(call('vkBind'+kind+'Memory',I,P,P,P,Q)(device,handle,memory,0),'bind '+kind)
            receipt.setdefault('allocations',[]).append({'kind':kind,'bytes':req.size,'memory_type_flags':mp.types[index].flags})
            return memory
        extent=Extent(width,height,1);subrange=Range(1,0,1,0,1)
        image=create('Image',ImageInfo(type=1,format=37,extent=extent,mips=1,layers=1,samples=1,usage=0x11))
        allocate(image,'Image',0)
        view=create('ImageView',ViewInfo(image=image,type=1,format=37,range=subrange))
        buffer=create('Buffer',BufferInfo(size=width*height*4,usage=2));readback=allocate(buffer,'Buffer',6)
        attachment=Attachment(0,37,1,1,0,2,1,0,2);ar=AttachmentRef(0,2)
        sub=Subpass(colorCount=1,colors=C.pointer(ar))
        dep=Dependency(0xffffffff,0,0x400,0x400,0,0x100,0)
        renderpass=create('RenderPass',PassInfo(attachmentCount=1,attachments=C.pointer(attachment),subpassCount=1,subpasses=C.pointer(sub),dependencyCount=1,dependencies=C.pointer(dep)))
        views=(P*1)(view);frame=create('Framebuffer',FrameInfo(renderPass=renderpass,count=1,attachments=views,width=width,height=height,layers=1))
        stages=(Stage*2)()
        for i,(code,kind,flag) in enumerate([(VERTEX,0,1),(FRAGMENT,1,16)]):
            spv=shader(code,kind);words=(U*(len(spv)//4)).from_buffer_copy(spv)
            module=create('ShaderModule',ShaderInfo(size=len(spv),code=words))
            stages[i]=Stage(stage=flag,module=module,name=b'main')
            receipt.setdefault('spirv_sha256',[]).append(hashlib.sha256(spv).hexdigest())
        pr=PushRange(1,0,64);layout=create('PipelineLayout',LayoutInfo(pushCount=1,pushRanges=C.pointer(pr)))
        vertex=VertexInput();assembly=Assembly(topology=3);vp=Viewport(0,0,width,height,0,1);rect=Rect(0,0,width,height)
        viewport=ViewportInfo(count=1,viewports=C.pointer(vp),scissorCount=1,scissors=C.pointer(rect));raster=Raster(lineWidth=1)
        ms=Multisample(samples=1);ba=BlendAttachment(writeMask=15);blend=Blend(count=1,attachments=C.pointer(ba))
        pi=PipelineInfo(stageCount=2,stages=stages,vertex=C.pointer(vertex),assembly=C.pointer(assembly),viewport=C.pointer(viewport),raster=C.pointer(raster),multisample=C.pointer(ms),blend=C.pointer(blend),layout=layout,renderPass=renderpass,baseIndex=-1)
        pipeline=P();rc=call('vkCreateGraphicsPipelines',I,P,P,U,PTR(PipelineInfo),P,PTR(P))(device,None,1,C.byref(pi),None,C.byref(pipeline))
        if pipeline: destroy.append(('Pipeline',pipeline))
        checked(rc,'vkCreateGraphicsPipelines')
        pool=create('CommandPool',PoolInfo(family=family));ci=CommandInfo(pool=pool,count=1);command=P()
        checked(call('vkAllocateCommandBuffers',I,P,PTR(CommandInfo),PTR(P))(device,C.byref(ci),C.byref(command)),'vkAllocateCommandBuffers')
        bi=BeginInfo(flags=1);checked(call('vkBeginCommandBuffer',I,P,PTR(BeginInfo))(command,C.byref(bi)),'vkBeginCommandBuffer')
        clear=(F*4)(*(x/255 for x in BACKGROUND));pb=PassBegin(renderPass=renderpass,framebuffer=frame,area=rect,clearCount=1,clears=C.pointer(clear))
        call('vkCmdBeginRenderPass',None,P,PTR(PassBegin),U)(command,C.byref(pb),0)
        call('vkCmdBindPipeline',None,P,U,P)(command,0,pipeline)
        push=(F*16)(*[x for p in positions for x in p],*color)
        call('vkCmdPushConstants',None,P,P,U,U,U,P)(command,layout,1,0,64,push)
        call('vkCmdDraw',None,P,U,U,U,U)(command,3,1,0,0);receipt['calls'].append('vkCmdDraw(3,1,0,0)')
        call('vkCmdEndRenderPass',None,P)(command)
        barrier=ImageBarrier(srcAccess=0x100,dstAccess=0x800,oldLayout=2,newLayout=6,srcFamily=0xffffffff,dstFamily=0xffffffff,image=image,range=subrange)
        barrier_fn=call('vkCmdPipelineBarrier',None,P,U,U,U,U,P,U,P,U,P)
        barrier_fn(command,0x400,0x1000,0,0,None,0,None,1,C.byref(barrier))
        cp=Copy(layers=Layers(1,0,0,1),extent=extent)
        call('vkCmdCopyImageToBuffer',None,P,P,U,P,U,PTR(Copy))(command,image,6,buffer,1,C.byref(cp))
        bb=BufferBarrier(srcAccess=0x1000,dstAccess=0x2000,srcFamily=0xffffffff,dstFamily=0xffffffff,buffer=buffer,size=width*height*4)
        barrier_fn(command,0x1000,0x4000,0,0,None,1,C.byref(bb),0,None)
        checked(call('vkEndCommandBuffer',I,P)(command),'vkEndCommandBuffer')
        fence=create('Fence',FenceInfo());si=Submit(commandCount=1,commands=C.pointer(command))
        checked(call('vkQueueSubmit',I,P,U,PTR(Submit),P)(queue,1,C.byref(si),fence),'vkQueueSubmit');pending=True
        result=call('vkWaitForFences',I,P,U,PTR(P),U,Q)(device,1,C.pointer(fence),1,5_000_000_000)
        receipt['fence_wait_result']=result;checked(result,'vkWaitForFences');pending=False
        receipt['calls'].append('vkWaitForFences: VK_SUCCESS')
        mapped=P();checked(call('vkMapMemory',I,P,P,Q,Q,U,PTR(P))(device,readback,0,width*height*4,0,C.byref(mapped)),'vkMapMemory')
        try: pixels=C.string_at(mapped,width*height*4)
        finally: call('vkUnmapMemory',None,P,P)(device,readback)
        receipt['pixels_sha256']=hashlib.sha256(pixels).hexdigest()
        receipt.update(verify_pixels(width,height,positions,color,pixels))
        receipt.update({'png':png(output,width,height,pixels),'status':'passed'})
    finally:
        if device:
            idle=call('vkDeviceWaitIdle',I,P)(device)
            receipt['device_wait_idle_result']=idle
            # A timeout never licenses freeing live resources. Device loss is terminal.
            if idle not in (0,-4): raise RuntimeError(f'cannot prove quiescence: VkResult {idle}; no manual object destruction')
            receipt['pending_at_cleanup']=pending
            for name,handle in reversed(destroy): call('vkDestroy'+name,None,P,P,P)(device,handle,None)
            for memory in memories: call('vkFreeMemory',None,P,P,P)(device,memory,None)
            call('vkDestroyDevice',None,P,P)(device,None)
            receipt['destroyed_objects']=len(destroy);receipt['freed_allocations']=len(memories)
        if instance: call('vkDestroyInstance',None,P,P)(instance,None)
    return receipt

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--width',type=int,default=128);p.add_argument('--height',type=int,default=96);p.add_argument('--fixture',type=Path)
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    positions=[[-.7,.7,0,1],[.7,.7,0,1],[0,-.7,0,1]];color=[64/255,160/255,240/255,1]
    source=None;result={}
    try:
        if args.fixture:
            with args.fixture.open('rb') as stream: source=stream.read(65537)
            if len(source)>65536: raise ValueError('fixture input exceeds cap')
            data=json.loads(source);positions=data['positions'];color=data['color']
        draw(args.width,args.height,positions,color,args.output/'triangle.png',result)
        result['fixture_input_sha256']=hashlib.sha256(source).hexdigest() if source else None
        result['adapter_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        result['icd_path']=os.environ.get('VK_DRIVER_FILES')
        (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2));return 0
    except Exception as error:
        result.update({'status':'failed','error':f'{type(error).__name__}: {error}'})
        (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));return 1

if __name__=='__main__': sys.exit(main())

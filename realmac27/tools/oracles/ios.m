// ios: render red into an IOSurface-backed texture, then read the IOSurface on the CPU.
//  nosync - commit + wait, lock, read           (what an Apple-silicon-only renderer does)
//  sync   - same plus a blit synchronizeResource (what a discrete-GPU-aware renderer does)
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
 "vertex V vs(uint i [[vertex_id]]) { float2 q[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; V o; o.p = float4(q[i],0,1); return o; }\n"
 "fragment float4 fs(V in [[stage_in]]) { return float4(1,0,0,1); }\n";
int main(void) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *err = nil;
    id<MTLLibrary> L = [d newLibraryWithSource:src options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [L newFunctionWithName:@"vs"]; pd.fragmentFunction = [L newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&err];
    printf("device %s unified %d\n", d.name.UTF8String, d.hasUnifiedMemory);
    for (int sync = 0; sync < 2; sync++) {
        NSDictionary *p = @{ (id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @64, (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
        IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
        IOSurfaceLock(s, 0, NULL); memset(IOSurfaceGetBaseAddress(s), 0, IOSurfaceGetAllocSize(s)); IOSurfaceUnlock(s, 0, NULL);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> t = [d newTextureWithDescriptor:td iosurface:s plane:0];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [[d newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:ps]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
        if (sync && t.storageMode == MTLStorageModeManaged) { id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder]; [b synchronizeResource:t]; [b endEncoding]; }
        [cb commit]; [cb waitUntilCompleted];
        IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *px = (const uint8_t *)IOSurfaceGetBaseAddress(s) + 32 * IOSurfaceGetBytesPerRow(s) + 32 * 4;
        printf("%-6s storage %lu: BGRA %02x %02x %02x %02x %s\n", sync ? "sync" : "nosync", (unsigned long)t.storageMode,
               px[0], px[1], px[2], px[3], (px[2] == 0xff && px[3] == 0xff) ? "ok (red)" : "WRONG");
        IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL); CFRelease(s);
    }
} return 0; }

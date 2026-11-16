// rt: draw into a 64x64 texture on the default Metal device and read pixels back. Arms:
//  clear      - load action clear to blue, no draw                     (expect 0000ffff)
//  tri        - clear blue, full-screen triangle writing red via color(0) (expect ff0000ff)
//  tri_fc     - same, shader specialised with a function constant        (expect ff0000ff)
//  tri_blend  - same, alpha blending on (src over)                       (expect ff0000ff)
//  tri_att1   - two attachments, writes color(1) green; reads attachment 1 (expect 00ff00ff)
//  shared/private: storage of the target (private read back through a blit)
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
 "constant bool fc [[function_constant(0)]];\n"
 "struct V { float4 p [[position]]; };\n"
 "vertex V vs(uint i [[vertex_id]]) { float2 q[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; V o; o.p = float4(q[i],0,1); return o; }\n"
 "fragment float4 fs(V in [[stage_in]]) { return float4(1,0,0,1); }\n"
 "fragment float4 fsfc(V in [[stage_in]]) { return fc ? float4(1,0,0,1) : float4(0,0,0,0); }\n"
 "struct O2 { float4 a [[color(0)]]; float4 b [[color(1)]]; };\n"
 "fragment O2 fs2(V in [[stage_in]]) { O2 o; o.a = float4(1,0,0,1); o.b = float4(0,1,0,1); return o; }\n";
static uint32_t run(id<MTLDevice> d, id<MTLLibrary> L, const char *arm, MTLStorageMode sm) {
    BOOL two = !strcmp(arm, "tri_att1");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = sm;
    id<MTLTexture> t0 = [d newTextureWithDescriptor:td], t1 = two ? [d newTextureWithDescriptor:td] : nil;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t0; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (two) { rp.colorAttachments[1].texture = t1; rp.colorAttachments[1].loadAction = MTLLoadActionClear;
        rp.colorAttachments[1].clearColor = MTLClearColorMake(0, 0, 1, 1); rp.colorAttachments[1].storeAction = MTLStoreActionStore; }
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    if (strcmp(arm, "clear")) {
        NSError *err = nil; id<MTLFunction> fs;
        if (!strcmp(arm, "tri_fc")) { MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; bool v = true;
            [cv setConstantValue:&v type:MTLDataTypeBool atIndex:0]; fs = [L newFunctionWithName:@"fsfc" constantValues:cv error:&err]; }
        else fs = [L newFunctionWithName:two ? @"fs2" : @"fs"];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [L newFunctionWithName:@"vs"]; pd.fragmentFunction = fs;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (two) pd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (!strcmp(arm, "tri_blend")) { pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha; pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha; }
        id<MTLRenderPipelineState> ps = [d newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!ps) { printf("%-10s pipeline FAILED: %s\n", arm, err.localizedDescription.UTF8String); [e endEncoding]; return 0xdead; }
        [e setRenderPipelineState:ps]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [e endEncoding];
    id<MTLTexture> rd = two ? t1 : t0; id<MTLBuffer> rb = [d newBufferWithLength:64*64*4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:rd sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*64];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) printf("%-10s command buffer error: %s\n", arm, cb.error.localizedDescription.UTF8String);
    const uint8_t *p = rb.contents; p += (32*64 + 32) * 4;
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}
int main(void) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); printf("device %s\n", d.name.UTF8String);
    NSError *err = nil; id<MTLLibrary> L = [d newLibraryWithSource:src options:nil error:&err];
    if (!L) { printf("library FAILED: %s\n", err.localizedDescription.UTF8String); return 1; }
    const char *arms[] = { "clear", "tri", "tri_fc", "tri_blend", "tri_att1" };
    uint32_t want[] = { 0x0000ffff, 0xff0000ff, 0xff0000ff, 0xff0000ff, 0x00ff00ff };
    for (int s = 0; s < 2; s++) for (int i = 0; i < 5; i++) {
        uint32_t got = run(d, L, arms[i], s ? MTLStorageModePrivate : MTLStorageModeManaged);
        printf("%-10s %-7s got %08x want %08x %s\n", arms[i], s ? "private" : "managed", got, want[i], got == want[i] ? "ok" : "WRONG");
    }
} return 0; }

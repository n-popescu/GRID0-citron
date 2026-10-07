// SPDX-License-Identifier: GPL-2.0-or-later
// Compare the existing SPIRV-Cross helpers with explicit FMA on real Metal hardware.
import Foundation
import Metal

let device = MTLCreateSystemDefaultDevice()!
let source = """
#include <metal_stdlib>
using namespace metal;
[[clang::optnone]] float oldAdd(float a, float b) { return fma(1.0f, a, b); }
[[clang::optnone]] float oldMul(float a, float b) { return fma(a, b, 0.0f); }
kernel void check(const device float4* input [[buffer(0)]],
                  device float4* output [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    float4 v = input[i];
    output[i] = float4(oldAdd(oldMul(v.x, v.y), v.z),
                       fma(1.0f, fma(v.x, v.y, 0.0f), v.z),
                       oldMul(v.x, v.y), fma(v.x, v.y, 0.0f));
}
"""
var state: UInt64 = 0x123456789abcdef
func randomBits() -> UInt32 {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17
    return UInt32(truncatingIfNeeded: state)
}
var inputs = [SIMD4<Float>]()
for _ in 0..<65536 {
    func operand() -> Float {
        let bits = randomBits()
        // Cover finite normals across the full exponent range, including
        // overflow, cancellation and subnormal products.
        return Float(bitPattern: (bits & 0x807fffff) | ((bits % 254 + 1) << 23))
    }
    inputs.append(SIMD4(operand(), operand(), operand(), 0))
}
let edges: [Float] = [0, -0.0, .infinity, -.infinity, .nan,
                      .leastNonzeroMagnitude, -.leastNonzeroMagnitude,
                      .leastNormalMagnitude, -.leastNormalMagnitude, 1, -1]
for a in edges { for b in edges { for c in edges { inputs.append(SIMD4(a,b,c,0)) } } }
// If the compiler incorrectly fuses the multiply and addition, cancellation
// exposes the unrounded product instead of returning zero.
for _ in 0..<65536 {
    let a = Float(bitPattern: 0x3f800000 | (randomBits() & 0x7fffff))
    let b = Float(bitPattern: 0x3f800000 | (randomBits() & 0x7fffff))
    inputs.append(SIMD4(a,b,-(a*b),0))
}
for fast in [false, true] {
    let options = MTLCompileOptions()
    options.fastMathEnabled = fast
    let library = try device.makeLibrary(source: source, options: options)
    let pipeline = try device.makeComputePipelineState(function: library.makeFunction(name: "check")!)
    let input = inputs.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count)! }
    let output = device.makeBuffer(length: inputs.count * MemoryLayout<SIMD4<Float>>.stride)!
    let command = device.makeCommandQueue()!.makeCommandBuffer()!
    let encoder = command.makeComputeCommandEncoder()!
    encoder.setComputePipelineState(pipeline)
    encoder.setBuffer(input, offset: 0, index: 0); encoder.setBuffer(output, offset: 0, index: 1)
    encoder.dispatchThreads(MTLSize(width: inputs.count, height: 1, depth: 1),
                            threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
    encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
    guard command.status == .completed else { fatalError("Metal execution failed: \(String(describing: command.error))") }
    let values = output.contents().bindMemory(to: SIMD4<Float>.self, capacity: inputs.count)
    var differences = 0
    var zeroSignDifferences = 0
    for i in inputs.indices {
        for (a,b) in [(values[i].x,values[i].y),(values[i].z,values[i].w)] {
            if a.bitPattern == b.bitPattern || (a.isNaN && b.isNaN) { continue }
            if a == 0 && b == 0 { zeroSignDifferences += 1; continue }
            differences += 1
            if differences <= 3 { print("difference",fast,i,inputs[i],a,b) }
        }
    }
    print("Metal fastMath=\(fast): \(inputs.count) inputs, \(differences) numeric differences, \(zeroSignDifferences) zero-sign differences")
    if differences != 0 || zeroSignDifferences != 0 { exit(1) }
}

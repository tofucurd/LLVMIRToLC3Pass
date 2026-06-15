; This must fail: floating-point sqrt is not supported
declare float @llvm.sqrt.f32(float)

define void @LC3_main() {
  %x = call float @llvm.sqrt.f32(float 4.000000e+00)
  ret void
}

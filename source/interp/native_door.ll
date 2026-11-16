; native_door.ll — the interpreter's call into native x86 code. App-independent, so it is compiled once and
; linked into the host, not generated per program.
;
; Why this is not C. The proven way to call an unknown native function with arm64's register state is a
;    variadic call returning {i64, i64, double, double} — rax, rdx, xmm0, xmm1 — carrying 8 integers, 8
;    doubles and 16 stack words. C cannot express that return, and the equivalent struct would be classified
;    MEMORY and returned through a hidden pointer, which is a different ABI. xlat2's native adapter makes
;    exactly this call for translated code; this is the same call reachable from the interpreter loop.
; Integer arguments pass through __arm2x86_cbarg first: an argument that is a known guest function start is
;    a CALLBACK, and native code must receive a native-callable trampoline, never an arm64 address.

declare void @__arm2x86_stack_args(i64, ptr)
declare i64 @__arm2x86_cbarg(i64)

define void @__arm2x86_interp_native(i64 %t, ptr %xs, ptr %vs, i64 %asp) {
entry:
  %stk = alloca [16 x i64], align 16
  call void @__arm2x86_stack_args(i64 %asp, ptr %stk)
  %s0p = getelementptr i64, ptr %stk, i64 0
  %s0 = load i64, ptr %s0p
  %s1p = getelementptr i64, ptr %stk, i64 1
  %s1 = load i64, ptr %s1p
  %s2p = getelementptr i64, ptr %stk, i64 2
  %s2 = load i64, ptr %s2p
  %s3p = getelementptr i64, ptr %stk, i64 3
  %s3 = load i64, ptr %s3p
  %s4p = getelementptr i64, ptr %stk, i64 4
  %s4 = load i64, ptr %s4p
  %s5p = getelementptr i64, ptr %stk, i64 5
  %s5 = load i64, ptr %s5p
  %s6p = getelementptr i64, ptr %stk, i64 6
  %s6 = load i64, ptr %s6p
  %s7p = getelementptr i64, ptr %stk, i64 7
  %s7 = load i64, ptr %s7p
  %s8p = getelementptr i64, ptr %stk, i64 8
  %s8 = load i64, ptr %s8p
  %s9p = getelementptr i64, ptr %stk, i64 9
  %s9 = load i64, ptr %s9p
  %s10p = getelementptr i64, ptr %stk, i64 10
  %s10 = load i64, ptr %s10p
  %s11p = getelementptr i64, ptr %stk, i64 11
  %s11 = load i64, ptr %s11p
  %s12p = getelementptr i64, ptr %stk, i64 12
  %s12 = load i64, ptr %s12p
  %s13p = getelementptr i64, ptr %stk, i64 13
  %s13 = load i64, ptr %s13p
  %s14p = getelementptr i64, ptr %stk, i64 14
  %s14 = load i64, ptr %s14p
  %s15p = getelementptr i64, ptr %stk, i64 15
  %s15 = load i64, ptr %s15p

  %f = inttoptr i64 %t to ptr

  %g0 = getelementptr [32 x i64], ptr %xs, i64 0, i64 0
  %a0 = load i64, ptr %g0
  %n0 = call i64 @__arm2x86_cbarg(i64 %a0)
  %g1 = getelementptr [32 x i64], ptr %xs, i64 0, i64 1
  %a1 = load i64, ptr %g1
  %n1 = call i64 @__arm2x86_cbarg(i64 %a1)
  %g2 = getelementptr [32 x i64], ptr %xs, i64 0, i64 2
  %a2 = load i64, ptr %g2
  %n2 = call i64 @__arm2x86_cbarg(i64 %a2)
  %g3 = getelementptr [32 x i64], ptr %xs, i64 0, i64 3
  %a3 = load i64, ptr %g3
  %n3 = call i64 @__arm2x86_cbarg(i64 %a3)
  %g4 = getelementptr [32 x i64], ptr %xs, i64 0, i64 4
  %a4 = load i64, ptr %g4
  %n4 = call i64 @__arm2x86_cbarg(i64 %a4)
  %g5 = getelementptr [32 x i64], ptr %xs, i64 0, i64 5
  %a5 = load i64, ptr %g5
  %n5 = call i64 @__arm2x86_cbarg(i64 %a5)
  %g6 = getelementptr [32 x i64], ptr %xs, i64 0, i64 6
  %a6 = load i64, ptr %g6
  %n6 = call i64 @__arm2x86_cbarg(i64 %a6)
  %g7 = getelementptr [32 x i64], ptr %xs, i64 0, i64 7
  %a7 = load i64, ptr %g7
  %n7 = call i64 @__arm2x86_cbarg(i64 %a7)

  %h0 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 0
  %q0 = load <2 x i64>, ptr %h0
  %e0 = extractelement <2 x i64> %q0, i32 0
  %m0 = bitcast i64 %e0 to double
  %h1 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 1
  %q1 = load <2 x i64>, ptr %h1
  %e1 = extractelement <2 x i64> %q1, i32 0
  %m1 = bitcast i64 %e1 to double
  %h2 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 2
  %q2 = load <2 x i64>, ptr %h2
  %e2 = extractelement <2 x i64> %q2, i32 0
  %m2 = bitcast i64 %e2 to double
  %h3 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 3
  %q3 = load <2 x i64>, ptr %h3
  %e3 = extractelement <2 x i64> %q3, i32 0
  %m3 = bitcast i64 %e3 to double
  %h4 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 4
  %q4 = load <2 x i64>, ptr %h4
  %e4 = extractelement <2 x i64> %q4, i32 0
  %m4 = bitcast i64 %e4 to double
  %h5 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 5
  %q5 = load <2 x i64>, ptr %h5
  %e5 = extractelement <2 x i64> %q5, i32 0
  %m5 = bitcast i64 %e5 to double
  %h6 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 6
  %q6 = load <2 x i64>, ptr %h6
  %e6 = extractelement <2 x i64> %q6, i32 0
  %m6 = bitcast i64 %e6 to double
  %h7 = getelementptr [32 x <2 x i64>], ptr %vs, i64 0, i64 7
  %q7 = load <2 x i64>, ptr %h7
  %e7 = extractelement <2 x i64> %q7, i32 0
  %m7 = bitcast i64 %e7 to double

  %r = call { i64, i64, double, double } (i64, ...) %f(i64 %n0, i64 %n1, i64 %n2, i64 %n3, i64 %n4, i64 %n5, i64 %n6, i64 %n7, double %m0, double %m1, double %m2, double %m3, double %m4, double %m5, double %m6, double %m7, i64 %s0, i64 %s1, i64 %s2, i64 %s3, i64 %s4, i64 %s5, i64 %s6, i64 %s7, i64 %s8, i64 %s9, i64 %s10, i64 %s11, i64 %s12, i64 %s13, i64 %s14, i64 %s15)
  %rx0 = extractvalue { i64, i64, double, double } %r, 0
  %rx1 = extractvalue { i64, i64, double, double } %r, 1
  %rv0 = extractvalue { i64, i64, double, double } %r, 2
  %rv1 = extractvalue { i64, i64, double, double } %r, 3

  ; x0, x1 and the low lanes of v0, v1 are everything AAPCS64 returns in registers
  store i64 %rx0, ptr %g0
  store i64 %rx1, ptr %g1
  %rv0b = bitcast double %rv0 to i64
  %rv1b = bitcast double %rv1 to i64
  %w0 = insertelement <2 x i64> zeroinitializer, i64 %rv0b, i32 0
  store <2 x i64> %w0, ptr %h0
  %w1 = insertelement <2 x i64> zeroinitializer, i64 %rv1b, i32 0
  store <2 x i64> %w1, ptr %h1
  ret void
}

@__arm2x86_interp_native_p = constant ptr @__arm2x86_interp_native

; eh_guard.ll — every native call the interpreter makes runs inside this frame, so a C++ exception thrown by native
; code meets a personality that can see the GUEST's catch blocks (a64_eh.c). IR, not C: C cannot name a personality.
; fn is called with five integer arguments and its rax is stored to *out; a callee with fewer arguments ignores
;    the rest (x86-64 SysV), a void callee leaves rax unread by the caller.
; The landing pad is a catch-all, but __arm2x86_eh_personality only lets the native personality claim it when a
;    guest handler exists; otherwise the exception unwinds past this frame exactly as it did before the guard.
; Returns 0 = returned normally (*out = rax), 1 = landed here with an exception (*out = its _Unwind_Exception).

declare i32 @__arm2x86_eh_personality(...)

define i32 @__arm2x86_eh_guard(ptr %fn, ptr %out, i64 %a, i64 %b, i64 %c, i64 %d, i64 %e) uwtable personality ptr @__arm2x86_eh_personality {
entry:
  %r = invoke i64 %fn(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e) to label %ok unwind label %lp
ok:
  store i64 %r, ptr %out
  ret i32 0
lp:
  %x = landingpad { ptr, i32 } catch ptr null
  %ue = extractvalue { ptr, i32 } %x, 0
  %ui = ptrtoint ptr %ue to i64
  store i64 %ui, ptr %out
  ret i32 1
}

; SPDX-License-Identifier: MIT
; GPU scanout conversion (GpuScanoutPresenter.h): RGBA 16F rows to the scanout's BGRX rows, one thread per pixel. Each half's 16-bit
; pattern indexes a 65536-entry table of 8-bit codes (the display driver's own rounding: linear light to sRGB, or values already
; encoded; negatives and NaNs 0), so the result is exactly what the CPU path stores. Hand-written AIR, the form Metal's front end emits:
;   kernel void scanout_convert(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]],
;                               device const uint *table [[buffer(2)]], constant uint4 &p [[buffer(3)]],
;                               uint2 gid [[thread_position_in_grid]])
; p: width, height, source row pitch in words, destination row pitch in words.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define void @scanout_convert(ptr addrspace(1) noundef readonly "air-buffer-no-alias" %src, ptr addrspace(1) noundef "air-buffer-no-alias" %dst, ptr addrspace(1) noundef readonly "air-buffer-no-alias" %table, ptr addrspace(2) noundef readonly align 16 dereferenceable(16) "air-buffer-no-alias" %p, <2 x i32> noundef %gid) local_unnamed_addr #0 {
entry:
  %w = load i32, ptr addrspace(2) %p, align 16
  %ph = getelementptr inbounds i32, ptr addrspace(2) %p, i64 1
  %h = load i32, ptr addrspace(2) %ph, align 4
  %pss = getelementptr inbounds i32, ptr addrspace(2) %p, i64 2
  %ss = load i32, ptr addrspace(2) %pss, align 8
  %pds = getelementptr inbounds i32, ptr addrspace(2) %p, i64 3
  %ds = load i32, ptr addrspace(2) %pds, align 4
  %x = extractelement <2 x i32> %gid, i64 0
  %y = extractelement <2 x i32> %gid, i64 1
  %inx = icmp ult i32 %x, %w
  %iny = icmp ult i32 %y, %h
  %in = and i1 %inx, %iny
  br i1 %in, label %body, label %done

body:
  %x64 = zext i32 %x to i64
  %y64 = zext i32 %y to i64
  %ss64 = zext i32 %ss to i64
  %srow = mul i64 %y64, %ss64
  %x2 = shl i64 %x64, 1
  %si = add i64 %srow, %x2
  %sp = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %si
  %lo = load i32, ptr addrspace(1) %sp, align 4
  %si1 = add i64 %si, 1
  %sp1 = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %si1
  %hi = load i32, ptr addrspace(1) %sp1, align 4
  %r = and i32 %lo, 65535
  %g = lshr i32 %lo, 16
  %b = and i32 %hi, 65535
  %r64 = zext i32 %r to i64
  %g64 = zext i32 %g to i64
  %b64 = zext i32 %b to i64
  %rp = getelementptr inbounds i32, ptr addrspace(1) %table, i64 %r64
  %gp = getelementptr inbounds i32, ptr addrspace(1) %table, i64 %g64
  %bp = getelementptr inbounds i32, ptr addrspace(1) %table, i64 %b64
  %rv = load i32, ptr addrspace(1) %rp, align 4
  %gv = load i32, ptr addrspace(1) %gp, align 4
  %bv = load i32, ptr addrspace(1) %bp, align 4
  %rs = shl i32 %rv, 16
  %gs = shl i32 %gv, 8
  %rg = or i32 %rs, %gs
  %rgb = or i32 %rg, %bv
  %px = or i32 %rgb, -16777216
  %ds64 = zext i32 %ds to i64
  %drow = mul i64 %y64, %ds64
  %di = add i64 %drow, %x64
  %dp = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %di
  store i32 %px, ptr addrspace(1) %dp, align 4
  br label %done

done:
  ret void
}

; scanout_opaque: BGRA rows to the scanout's rows with alpha 0xff. The display fetches the scanout with its per-pixel alpha (SSPP
; SRC_FORMAT 0x237ff: bit 8, alpha enable) and blends it over its own background; the compositor's framebuffer alpha is not opacity, so
; a frame copied as it is showed translucent tinted blocks wherever the alpha was below 1 (around Liquid Glass, i23c80d087).
;   kernel void scanout_opaque(device const uint *src [[buffer(0)]], device uint *dst [[buffer(1)]], constant uint4 &p [[buffer(3)]],
;                              uint2 gid [[thread_position_in_grid]])
define void @scanout_opaque(ptr addrspace(1) noundef readonly "air-buffer-no-alias" %src, ptr addrspace(1) noundef "air-buffer-no-alias" %dst, ptr addrspace(2) noundef readonly align 16 dereferenceable(16) "air-buffer-no-alias" %p, <2 x i32> noundef %gid) local_unnamed_addr #0 {
entry:
  %w = load i32, ptr addrspace(2) %p, align 16
  %ph = getelementptr inbounds i32, ptr addrspace(2) %p, i64 1
  %h = load i32, ptr addrspace(2) %ph, align 4
  %pss = getelementptr inbounds i32, ptr addrspace(2) %p, i64 2
  %ss = load i32, ptr addrspace(2) %pss, align 8
  %pds = getelementptr inbounds i32, ptr addrspace(2) %p, i64 3
  %ds = load i32, ptr addrspace(2) %pds, align 4
  %x = extractelement <2 x i32> %gid, i64 0
  %y = extractelement <2 x i32> %gid, i64 1
  %inx = icmp ult i32 %x, %w
  %iny = icmp ult i32 %y, %h
  %in = and i1 %inx, %iny
  br i1 %in, label %body, label %done

body:
  %x64 = zext i32 %x to i64
  %y64 = zext i32 %y to i64
  %ss64 = zext i32 %ss to i64
  %srow = mul i64 %y64, %ss64
  %si = add i64 %srow, %x64
  %sp = getelementptr inbounds i32, ptr addrspace(1) %src, i64 %si
  %v = load i32, ptr addrspace(1) %sp, align 4
  %px = or i32 %v, -16777216
  %ds64 = zext i32 %ds to i64
  %drow = mul i64 %y64, %ds64
  %di = add i64 %drow, %x64
  %dp = getelementptr inbounds i32, ptr addrspace(1) %dst, i64 %di
  store i32 %px, ptr addrspace(1) %dp, align 4
  br label %done

done:
  ret void
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7, !8}
!air.kernel = !{!9, !22, !29}
!air.compile_options = !{!17, !18}
!llvm.ident = !{!19}
!air.version = !{!20}
!air.language_version = !{!21}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 27, i32 0]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"frame-pointer", i32 2}
!3 = !{i32 7, !"air.max_device_buffers", i32 31}
!4 = !{i32 7, !"air.max_constant_buffers", i32 31}
!5 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!6 = !{i32 7, !"air.max_textures", i32 128}
!7 = !{i32 7, !"air.max_read_write_textures", i32 8}
!8 = !{i32 7, !"air.max_samplers", i32 16}
!9 = !{ptr @scanout_convert, !10, !11}
!10 = !{}
!11 = !{!12, !13, !14, !15, !16}
!12 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"src"}
!13 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dst"}
!14 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"table"}
!15 = !{i32 3, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"p"}
!16 = !{i32 4, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
!17 = !{!"air.compile.denorms_disable"}
!18 = !{!"air.compile.fast_math_enable"}
!19 = !{!"Apple metal version 32023.921 (metalfe-32023.921.6)"}
!20 = !{i32 2, i32 8, i32 0}
!21 = !{!"Metal", i32 4, i32 0, i32 0}
!22 = !{ptr @scanout_opaque, !10, !23}
!23 = !{!12, !13, !25, !24}
!24 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}
!25 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"p"}

; SPDX-License-Identifier: MIT
; Full-frame bilinear scanout scaling. The existing scanout transfer owns the
; source mapping, completion fence and destination buffer lifetime.
define internal i32 @scanout_read_pixel(ptr addrspace(1) %src, ptr addrspace(1) %table, i32 %offset, i32 %format) alwaysinline {
entry:
  %o64 = zext i32 %offset to i64
  %sp = getelementptr i32, ptr addrspace(1) %src, i64 %o64
  %lo = load i32, ptr addrspace(1) %sp, align 4
  %bgra = icmp eq i32 %format, 1
  br i1 %bgra, label %packed, label %half
packed:
  %opaque = or i32 %lo, -16777216
  ret i32 %opaque
half:
  %sp1 = getelementptr i32, ptr addrspace(1) %sp, i64 1
  %hi = load i32, ptr addrspace(1) %sp1, align 4
  %rb = and i32 %lo, 65535
  %gb = lshr i32 %lo, 16
  %bb = and i32 %hi, 65535
  %r64 = zext i32 %rb to i64
  %g64 = zext i32 %gb to i64
  %b64 = zext i32 %bb to i64
  %rp = getelementptr i32, ptr addrspace(1) %table, i64 %r64
  %gp = getelementptr i32, ptr addrspace(1) %table, i64 %g64
  %bp = getelementptr i32, ptr addrspace(1) %table, i64 %b64
  %rc = load i32, ptr addrspace(1) %rp, align 4
  %gc = load i32, ptr addrspace(1) %gp, align 4
  %bc = load i32, ptr addrspace(1) %bp, align 4
  %rs = shl i32 %rc, 16
  %gs = shl i32 %gc, 8
  %rg = or i32 %rs, %gs
  %rgb = or i32 %rg, %bc
  %rgba = or i32 %rgb, -16777216
  ret i32 %rgba
}

define void @scanout_scale(ptr addrspace(1) noundef readonly "air-buffer-no-alias" %src, ptr addrspace(1) noundef "air-buffer-no-alias" %dst, ptr addrspace(1) noundef readonly "air-buffer-no-alias" %table, ptr addrspace(2) noundef readonly align 16 dereferenceable(48) "air-buffer-no-alias" %p, <2 x i32> noundef %gid) local_unnamed_addr #0 {
entry:
  %p_sw = getelementptr i32, ptr addrspace(2) %p, i64 0
  %sw = load i32, ptr addrspace(2) %p_sw, align 4
  %p_sh = getelementptr i32, ptr addrspace(2) %p, i64 1
  %sh = load i32, ptr addrspace(2) %p_sh, align 4
  %p_ss = getelementptr i32, ptr addrspace(2) %p, i64 2
  %ss = load i32, ptr addrspace(2) %p_ss, align 4
  %p_ds = getelementptr i32, ptr addrspace(2) %p, i64 3
  %ds = load i32, ptr addrspace(2) %p_ds, align 4
  %p_dw = getelementptr i32, ptr addrspace(2) %p, i64 4
  %dw = load i32, ptr addrspace(2) %p_dw, align 4
  %p_dh = getelementptr i32, ptr addrspace(2) %p, i64 5
  %dh = load i32, ptr addrspace(2) %p_dh, align 4
  %p_ox = getelementptr i32, ptr addrspace(2) %p, i64 6
  %ox = load i32, ptr addrspace(2) %p_ox, align 4
  %p_oy = getelementptr i32, ptr addrspace(2) %p, i64 7
  %oy = load i32, ptr addrspace(2) %p_oy, align 4
  %p_format = getelementptr i32, ptr addrspace(2) %p, i64 8
  %format = load i32, ptr addrspace(2) %p_format, align 4
  %x = extractelement <2 x i32> %gid, i64 0
  %y = extractelement <2 x i32> %gid, i64 1
  %inx = icmp ult i32 %x, %dw
  %iny = icmp ult i32 %y, %dh
  %in = and i1 %inx, %iny
  br i1 %in, label %body, label %done
body:
  %xf = uitofp i32 %x to float
  %xc = fadd float %xf, 5.000000e-01
  %xsf = uitofp i32 %sw to float
  %xdf = uitofp i32 %dw to float
  %xn = fmul float %xc, %xsf
  %xq = fdiv float %xn, %xdf
  %xu = fsub float %xq, 5.000000e-01
  %xnegative = fcmp olt float %xu, 0.000000e+00
  %xlow = select i1 %xnegative, float 0.000000e+00, float %xu
  %xlast = sub i32 %sw, 1
  %xlastf = uitofp i32 %xlast to float
  %xoutside = fcmp ogt float %xlow, %xlastf
  %xpos = select i1 %xoutside, float %xlastf, float %xlow
  %x0 = fptoui float %xpos to i32
  %xnext = add i32 %x0, 1
  %xedge = icmp ugt i32 %xnext, %xlast
  %x1 = select i1 %xedge, i32 %xlast, i32 %xnext
  %x0f = uitofp i32 %x0 to float
  %xfraction = fsub float %xpos, %x0f
  %yf = uitofp i32 %y to float
  %yc = fadd float %yf, 5.000000e-01
  %ysf = uitofp i32 %sh to float
  %ydf = uitofp i32 %dh to float
  %yn = fmul float %yc, %ysf
  %yq = fdiv float %yn, %ydf
  %yu = fsub float %yq, 5.000000e-01
  %ynegative = fcmp olt float %yu, 0.000000e+00
  %ylow = select i1 %ynegative, float 0.000000e+00, float %yu
  %ylast = sub i32 %sh, 1
  %ylastf = uitofp i32 %ylast to float
  %youtside = fcmp ogt float %ylow, %ylastf
  %ypos = select i1 %youtside, float %ylastf, float %ylow
  %y0 = fptoui float %ypos to i32
  %ynext = add i32 %y0, 1
  %yedge = icmp ugt i32 %ynext, %ylast
  %y1 = select i1 %yedge, i32 %ylast, i32 %ynext
  %y0f = uitofp i32 %y0 to float
  %yfraction = fsub float %ypos, %y0f
  %isbgra = icmp eq i32 %format, 1
  %words = select i1 %isbgra, i32 1, i32 2
  %sx0 = add i32 %x0, %ox
  %sy0 = add i32 %y0, %oy
  %row0 = mul i32 %sy0, %ss
  %col0 = mul i32 %sx0, %words
  %off0 = add i32 %row0, %col0
  %c0 = call i32 @scanout_read_pixel(ptr addrspace(1) %src, ptr addrspace(1) %table, i32 %off0, i32 %format)
  %sx1 = add i32 %x1, %ox
  %sy1 = add i32 %y0, %oy
  %row1 = mul i32 %sy1, %ss
  %col1 = mul i32 %sx1, %words
  %off1 = add i32 %row1, %col1
  %c1 = call i32 @scanout_read_pixel(ptr addrspace(1) %src, ptr addrspace(1) %table, i32 %off1, i32 %format)
  %sx2 = add i32 %x0, %ox
  %sy2 = add i32 %y1, %oy
  %row2 = mul i32 %sy2, %ss
  %col2 = mul i32 %sx2, %words
  %off2 = add i32 %row2, %col2
  %c2 = call i32 @scanout_read_pixel(ptr addrspace(1) %src, ptr addrspace(1) %table, i32 %off2, i32 %format)
  %sx3 = add i32 %x1, %ox
  %sy3 = add i32 %y1, %oy
  %row3 = mul i32 %sy3, %ss
  %col3 = mul i32 %sx3, %words
  %off3 = add i32 %row3, %col3
  %c3 = call i32 @scanout_read_pixel(ptr addrspace(1) %src, ptr addrspace(1) %table, i32 %off3, i32 %format)
  %shift00 = lshr i32 %c0, 0
  %byte00 = and i32 %shift00, 255
  %f00 = uitofp i32 %byte00 to float
  %shift01 = lshr i32 %c1, 0
  %byte01 = and i32 %shift01, 255
  %f01 = uitofp i32 %byte01 to float
  %shift02 = lshr i32 %c2, 0
  %byte02 = and i32 %shift02, 255
  %f02 = uitofp i32 %byte02 to float
  %shift03 = lshr i32 %c3, 0
  %byte03 = and i32 %shift03, 255
  %f03 = uitofp i32 %byte03 to float
  %a0 = fsub float %f01, %f00
  %at0 = fmul float %a0, %xfraction
  %ab0 = fadd float %f00, %at0
  %b0 = fsub float %f03, %f02
  %bt0 = fmul float %b0, %xfraction
  %bb0 = fadd float %f02, %bt0
  %delta0 = fsub float %bb0, %ab0
  %ct0 = fmul float %delta0, %yfraction
  %value0 = fadd float %ab0, %ct0
  %rounded0 = fadd float %value0, 5.000000e-01
  %u0 = fptoui float %rounded0 to i32
  %out0 = shl i32 %u0, 0
  %shift10 = lshr i32 %c0, 8
  %byte10 = and i32 %shift10, 255
  %f10 = uitofp i32 %byte10 to float
  %shift11 = lshr i32 %c1, 8
  %byte11 = and i32 %shift11, 255
  %f11 = uitofp i32 %byte11 to float
  %shift12 = lshr i32 %c2, 8
  %byte12 = and i32 %shift12, 255
  %f12 = uitofp i32 %byte12 to float
  %shift13 = lshr i32 %c3, 8
  %byte13 = and i32 %shift13, 255
  %f13 = uitofp i32 %byte13 to float
  %a1 = fsub float %f11, %f10
  %at1 = fmul float %a1, %xfraction
  %ab1 = fadd float %f10, %at1
  %b1 = fsub float %f13, %f12
  %bt1 = fmul float %b1, %xfraction
  %bb1 = fadd float %f12, %bt1
  %delta1 = fsub float %bb1, %ab1
  %ct1 = fmul float %delta1, %yfraction
  %value1 = fadd float %ab1, %ct1
  %rounded1 = fadd float %value1, 5.000000e-01
  %u1 = fptoui float %rounded1 to i32
  %out1 = shl i32 %u1, 8
  %shift20 = lshr i32 %c0, 16
  %byte20 = and i32 %shift20, 255
  %f20 = uitofp i32 %byte20 to float
  %shift21 = lshr i32 %c1, 16
  %byte21 = and i32 %shift21, 255
  %f21 = uitofp i32 %byte21 to float
  %shift22 = lshr i32 %c2, 16
  %byte22 = and i32 %shift22, 255
  %f22 = uitofp i32 %byte22 to float
  %shift23 = lshr i32 %c3, 16
  %byte23 = and i32 %shift23, 255
  %f23 = uitofp i32 %byte23 to float
  %a2 = fsub float %f21, %f20
  %at2 = fmul float %a2, %xfraction
  %ab2 = fadd float %f20, %at2
  %b2 = fsub float %f23, %f22
  %bt2 = fmul float %b2, %xfraction
  %bb2 = fadd float %f22, %bt2
  %delta2 = fsub float %bb2, %ab2
  %ct2 = fmul float %delta2, %yfraction
  %value2 = fadd float %ab2, %ct2
  %rounded2 = fadd float %value2, 5.000000e-01
  %u2 = fptoui float %rounded2 to i32
  %out2 = shl i32 %u2, 16
  %bg = or i32 %out0, %out1
  %rgb = or i32 %bg, %out2
  %opaque = or i32 %rgb, -16777216
  %row = mul i32 %y, %ds
  %off = add i32 %row, %x
  %off64 = zext i32 %off to i64
  %dp = getelementptr i32, ptr addrspace(1) %dst, i64 %off64
  store i32 %opaque, ptr addrspace(1) %dp, align 4
  br label %done
done:
  ret void
}

!29 = !{ptr @scanout_scale, !10, !30}
!30 = !{!12, !13, !14, !31, !16}
!31 = !{i32 3, !"air.buffer", !"air.buffer_size", i32 48, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 48, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"ScaleParams", !"air.arg_name", !"p"}

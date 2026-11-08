set pagination off
set confirm off
set breakpoint pending on
break __driDriverGetExtensions_gsgpu
run
set $gsgpu_base = (unsigned long)&__driDriverGetExtensions_gsgpu - 0x7cbc0
printf "GSGPU_BASE=0x%lx\n", $gsgpu_base
disable 1
break *($gsgpu_base + 0xfd5b8)
commands
 silent
 set $realctx = $s6
 set $realfn = *(unsigned long*)($realctx + 512)
 printf "REAL_SET_VERTEX_BUFFERS ctx=0x%lx fn=0x%lx offset=0x%lx\n", $realctx, $realfn, $realfn - $gsgpu_base
 continue
end
break *($gsgpu_base + 0xfe6c4)
commands
 silent
 set $realctx = $s1
 set $realfn = *(unsigned long*)($realctx + 48)
 printf "REAL_DRAW_VBO ctx=0x%lx fn=0x%lx offset=0x%lx\n", $realctx, $realfn, $realfn - $gsgpu_base
 continue
end
continue

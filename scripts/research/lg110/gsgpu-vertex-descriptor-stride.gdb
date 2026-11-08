set pagination off
set confirm off
set breakpoint pending on
break __driDriverGetExtensions_gsgpu
run
set $gsgpu_base = (unsigned long)&__driDriverGetExtensions_gsgpu - 0x7cbc0
printf "GSGPU_BASE=0x%lx\n", $gsgpu_base
disable 1
break *($gsgpu_base + 0x17bcbc)
commands
 silent
 set $stride = *(unsigned short*)($s5 - 456)
 printf "VBO_DESC index=%u stride=%u resource=0x%lx desc=0x%lx\n", $s8, $stride, $s6, $s4
 x/4wx $s4
 continue
end
continue

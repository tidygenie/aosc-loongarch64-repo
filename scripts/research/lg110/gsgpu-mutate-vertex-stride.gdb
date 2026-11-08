# Test-process mutation only; never patch the installed driver.
# Require gsgpu_dri.so SHA-256 aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a.
# Use pipeline/UV16/frame-limit=3 and require exactly three writes plus AAA pixels.
set pagination off
set confirm off
set breakpoint pending on
break __driDriverGetExtensions_gsgpu
run
set $gsgpu_base = (unsigned long)&__driDriverGetExtensions_gsgpu - 0x7cbc0
set $mutations = 0
disable 1
break *($gsgpu_base + 0x17bcbc)
commands
 silent
 set $off = $s4 & 0xffffff
 if $s8 == 2
  printf "OBSERVE_UV off=0x%x word1=0x%x\n", $off, *(unsigned int*)($s4 + 4)
 end
 if $s8 == 2 && ($off == 0x2b10 || $off == 0x8f90 || $off == 0xf390)
  set $old = *(unsigned int*)($s4 + 4)
  if $old != 0x00100001
   printf "UNEXPECTED_DESCRIPTOR off=0x%x old=0x%x\n", $off, $old
  else
   set *(unsigned int*)($s4 + 4) = ($old & 0xc000ffff) | 0x00080000
   set $mutations = $mutations + 1
   printf "MUTATE_UV_STRIDE off=0x%x old=0x%x new=0x%x\n", $off, $old, *(unsigned int*)($s4 + 4)
  end
 end
 continue
end
continue
printf "MUTATIONS=%u\n", $mutations

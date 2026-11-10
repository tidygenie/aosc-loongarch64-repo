# Test-process mutation only; never patch the installed driver.
# Require gsgpu_dri.so SHA-256 aff7101ab877db0026fa0f8274e03a579f1b4fd2c16fabe2478d08f224d0da0a.
# Invoke with -ex 'set $source_word3 = 0x...' and -ex 'set $dest_word3 = 0x0005f22c'.
# In the format A/B/A control, only B may match source; A must remain R32G32.
set pagination off
set confirm off
set breakpoint pending on
break __driDriverGetExtensions_gsgpu
run
set $gsgpu_base = (unsigned long)&__driDriverGetExtensions_gsgpu - 0x7cbc0
set $mutations = 0
set $unexpected = 0
set $desc_base = 0
disable 1
break *($gsgpu_base + 0x17bcbc)
commands
 silent
 set $off = 0
 if $s8 == 2
  if $desc_base == 0
   set $desc_base = $s4 - 0x2b10
  end
  set $off = $s4 - $desc_base
  printf "OBSERVE_UV off=0x%x word3=0x%x\n", $off, *(unsigned int*)($s4 + 12)
 end
 if $s8 == 2 && ($off == 0x2b10 || $off == 0x8f90 || $off == 0xf390)
  set $old = *(unsigned int*)($s4 + 12)
  if $old == $source_word3
   set *(unsigned int*)($s4 + 12) = ($old & 0xfff80000) | ($dest_word3 & 0x0007ffff)
   set $mutations = $mutations + 1
   printf "MUTATE_UV_FORMAT off=0x%x old=0x%x new=0x%x\n", $off, $old, *(unsigned int*)($s4 + 12)
  else
   if $old != 0x0005f22c
    set $unexpected = $unexpected + 1
    printf "UNEXPECTED_DESCRIPTOR off=0x%x old=0x%x\n", $off, $old
   end
  end
 end
 continue
end
continue
printf "MUTATIONS=%u UNEXPECTED=%u\n", $mutations, $unexpected
